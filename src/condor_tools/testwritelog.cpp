/***************************************************************
 *
 * Copyright (C) 1990-2007, Condor Team, Computer Sciences Department,
 * University of Wisconsin-Madison, WI.
 * 
 * Licensed under the Apache License, Version 2.0 (the "License"); you
 * may not use this file except in compliance with the License.  You may
 * obtain a copy of the License at
 * 
 *    http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ***************************************************************/


 

/*********************************************************************
* Find files which may have been left lying around on somebody's workstation
* by condor.  Depending on command line options we may remove the file
* and/or mail appropriate authorities about it.
*********************************************************************/

#include "condor_common.h"
#include "condor_debug.h"
#include "condor_io.h"
#include "condor_config.h"
#include "condor_uid.h"
#include "string_list.h"
#include "directory.h"
#include "condor_qmgr.h"
#include "condor_classad.h"
#include "condor_attributes.h"
#include "condor_state.h"
#include "sig_install.h"
#include "condor_email.h"
#include "daemon.h"
#include "condor_distribution.h"
#include "basename.h" // for condor_basename 
#include "extArray.h"
#include "link.h"
#include "shared_port_endpoint.h"
#include "file_lock.h"
#include "../condor_privsep/condor_privsep.h"
#include "filename_tools.h"
#include "ipv6_hostname.h"
#include "subsystem_info.h"


extern void		_condor_set_debug_flags( const char *strflags, int flags );

// Define this to check for memory leaks

char		*Spool;				// dir for condor job queue
StringList   ExecuteDirs;		// dirs for execution of condor jobs
char		*Log;				// dir for condor program logs
char		*DaemonSockDir;     // dir for daemon named sockets
char		*PreenAdmin;		// who to send mail to in case of trouble
char		*MyName;			// name this program was invoked by
char		*LogName;			// name of requested log file
char        *ValidSpoolFiles;   // well known files in the spool dir
char        *InvalidLogFiles;   // files we know we want to delete from log
BOOLEAN		LogFlag;			// true if we should set the log file name
BOOLEAN		VerboseFlag;		// true if we should produce verbose output

// prototypes of local interest
void usage();
void init_params();

/*
  Tell folks how to use this program.
*/
void
usage()
{
	fprintf( stderr, "Usage: %s [-log logname] [-verbose] [-debug]\n", MyName );
	exit( 1 );
}


int
main( int argc, char *argv[] )
{
#ifndef WIN32
		// Ignore SIGPIPE so if we cannot connect to a daemon we do
		// not blowup with a sig 13.
    install_sig_handler(SIGPIPE, SIG_IGN );
#endif

	// initialize the config settings
	config_ex(false,false,0);
	
		// Initialize things
	MyName = argv[0];
	myDistro->Init( argc, argv );
	get_mySubSystem()->setName( "WRITELOG");
	config();
	dprintf_config(get_mySubSystem()->getName());

	VerboseFlag = FALSE;

		// Parse command line arguments
	for( argv++; *argv; argv++ ) {
		if( (*argv)[0] == '-' ) {
			switch( (*argv)[1] ) {
			
			  case 'd':
                dprintf_set_tool_debug("WRITELOG", 0);
				fprintf( stderr, "Setting debug");
				break;

			  case 'v':
				VerboseFlag = TRUE;
				fprintf( stderr, "Setting verbose flag");
				break;

			  case 'l':
				LogFlag = TRUE;
				argv++;
				LogName = *argv;
				fprintf( stderr, "Logname requested: %s\n", LogName );
				break;

			  default:
				usage();

			}
		} else {
			usage();
		}
	}
	
	init_params();

	if (VerboseFlag)
	{
		// always append D_FULLDEBUG locally when verbose.
		// shouldn't matter if it already exists.
		std::string szVerbose="D_FULLDEBUG";
		char * pval = param("WRITELOG_DEBUG");
		if( pval ) {
			szVerbose+="|";
			szVerbose+=pval;
			free( pval );
		}
		_condor_set_debug_flags( szVerbose.c_str(), D_FULLDEBUG );
		
	}



	dprintf( D_ALWAYS, "********************************\n");
	dprintf( D_ALWAYS, "STARTING: condor_testwritelog\n");
	dprintf( D_ALWAYS, "********************************\n");
	
    //dprintf( D_ALWAYS, "WRITELOG_LOG = %s\n", param("WRITELOG_LOG"));
	//dprintf( D_ALWAYS, "LOG = %s\n", param("LOG"));

	dprintf( D_ALWAYS, "********************************\n");
	dprintf( D_ALWAYS, "ENDING: condor_testwritelog\n");
	dprintf( D_ALWAYS, "********************************\n");
	
	return 0;
}

/*
  Grab an integer value which has been embedded in a file name.  We're given
  a name to examine, and a pattern to search for.  If the pattern exists,
  then the number we want follows it immediately.  If the pattern doesn't
  exist, we return -1.
*/
inline int
grab_val( const char *str, const char *pattern )
{
	char const *ptr;

	if( (ptr = strstr(str,pattern)) ) {
		return atoi(ptr + strlen(pattern) );
	}
	return -1;
}

extern "C" int
SetSyscalls( int foo ) { return foo; }

/*
  Initialize information from the configuration files.
*/
void
init_params()
{
	Spool = param("SPOOL");
    if( Spool == NULL ) {
        EXCEPT( "SPOOL not specified in config file\n" );
    }

	Log = param("LOG");
    if( Log == NULL ) {
        EXCEPT( "LOG not specified in config file\n" );
    }

	DaemonSockDir = param("DAEMON_SOCKET_DIR");
	if( DaemonSockDir == NULL ) {
		EXCEPT("DAEMON_SOCKET_DIR not defined\n");
	}

	char *Execute = param("EXECUTE");
	if( Execute ) {
		ExecuteDirs.append(Execute);
		free(Execute);
		Execute = NULL;
	}
		// In addition to EXECUTE, there may be SLOT1_EXECUTE, ...
#if 1
	ExtArray<const char *> params;
	Regex re; int err = 0; const char * pszMsg = 0;
	ASSERT(re.compile("slot([0-9]*)_execute", &pszMsg, &err, PCRE_CASELESS));
	if (param_names_matching(re, params)) {
		for (int ii = 0; ii < params.length(); ++ii) {
			Execute = param(params[ii]);
			if (Execute) {
				if ( ! ExecuteDirs.contains(Execute)) {
					ExecuteDirs.append(Execute);
				}
				free(Execute);
			}
		}
	}
	params.truncate(0);
#else
	ExtArray<ParamValue> *params = param_all();
	for( int p=params->length(); p--; ) {
		char const *name = (*params)[p].name.Value();
		char *tail = NULL;
		if( strncasecmp( name, "SLOT", 4 ) != 0 ) continue;
		long l = strtol( name+4, &tail, 10 );
		if( l == LONG_MIN || tail <= name || strcasecmp( tail, "_EXECUTE" ) != 0 ) continue;

		Execute = param(name);
		if( Execute ) {
			if( !ExecuteDirs.contains( Execute ) ) {
				ExecuteDirs.append( Execute );
			}
			free( Execute );
		}
	}
	delete params;
#endif


	ValidSpoolFiles = param("VALID_SPOOL_FILES");

	InvalidLogFiles = param("INVALID_LOG_FILES");
}

