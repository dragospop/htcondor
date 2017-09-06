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

#include "condor_common.h"
#include "condor_debug.h"
#include "condor_config.h"
#include "condor_attributes.h"
#include "stat_info.h"
#include "spooled_job_files.h"
#include "directory.h"
#include "filename_tools.h"
#include "proc.h"
#include "condor_uid.h"
#include "basename.h"

char *
gen_ckpt_name( char const *directory, int cluster, int proc, int subproc )
{
	char *answer = NULL;
	int bufpos = 0;
	int buflen = 0;
	int rc;

		// for efficiency, pre-allocate what is likely enough space
	buflen = (directory ? strlen(directory) : 0) + 80;
	answer = (char *)malloc(buflen);
	if( !answer ) {
		return NULL;
	}

	if( directory && directory[0] ) {
		rc = sprintf_realloc(&answer,&bufpos,&buflen,"%s%c%d%c",
							 directory, DIR_DELIM_CHAR,
							 cluster % 10000, DIR_DELIM_CHAR);
		if( rc < 0 ) goto error_cleanup;

		if( proc != ICKPT ) {
			rc = sprintf_realloc(&answer,&bufpos,&buflen,"%d%c",
								 proc % 10000, DIR_DELIM_CHAR);
			if( rc < 0 ) goto error_cleanup;
		}
	}

		// prior to 7.5.5, condor_submit generated the spooled executable
		// name and the schedd required that the name generated by submit
		// agreed with the name generated by the schedd, so to retain
		// backward compatibility with old versions of submit, the
		// basename that we generate must not change

	rc = sprintf_realloc(&answer,&bufpos,&buflen,"cluster%d",cluster);
	if( rc < 0 ) goto error_cleanup;

	if( proc == ICKPT ) {
		rc = sprintf_realloc(&answer,&bufpos,&buflen,".ickpt");
		if( rc < 0 ) goto error_cleanup;
	}
	else {
		rc = sprintf_realloc(&answer,&bufpos,&buflen,".proc%d",proc);
		if( rc < 0 ) goto error_cleanup;
	}

	rc = sprintf_realloc(&answer,&bufpos,&buflen,".subproc%d",subproc);
	if( rc < 0 ) goto error_cleanup;

	return answer;

error_cleanup:
	free( answer );
	return NULL;
}

char *GetSpooledExecutablePath( int cluster, const char *dir )
{
	if ( dir ) {
		return gen_ckpt_name( dir, cluster, ICKPT, 0 );
	} else {
		std::string spool;
		param( spool, "SPOOL" );
		return gen_ckpt_name( spool.c_str(), cluster, ICKPT, 0 );
	}
}

void GetSpooledSubmitDigestPath(MyString &path, int cluster, const char *dir /*= NULL*/ )
{
	auto_free_ptr spooldir;
	if ( ! dir) {
		spooldir.set(param("SPOOL"));
		dir = spooldir;
	}

	path.formatstr("%s%c%d%ccondor_submit.%d.digest", dir, DIR_DELIM_CHAR, cluster % 10000, DIR_DELIM_CHAR, cluster);
}

void
GetJobExecutable( const classad::ClassAd *job_ad, std::string &executable )
{
	char *Spool = param( "SPOOL" );
	if ( Spool ) {
		int cluster = 0;
		job_ad->EvaluateAttrInt( ATTR_CLUSTER_ID, cluster );
		char *ickpt = gen_ckpt_name( Spool, cluster, ICKPT, 0 );
		free( Spool );
		// TODO Should we just check existence?
		if ( ickpt && access( ickpt, F_OK | X_OK ) >= 0 ) {
			// we can access an executable in the spool dir
			executable = ickpt;
			free( ickpt );
			return;
		}
		free( ickpt );
	}
	std::string cmd;
	job_ad->EvaluateAttrString( ATTR_JOB_CMD, cmd );
	if ( fullpath( cmd.c_str() ) ) {
		executable = cmd;
	} else {
		job_ad->EvaluateAttrString( ATTR_JOB_IWD, executable );
		executable += DIR_DELIM_CHAR;
		executable += cmd;
	}
}

void
SpooledJobFiles::_getJobSpoolPath(int cluster, int proc, const classad::ClassAd * job_ad, std::string &spool_path)
{
	std::string spool_base;
	std::string alt_spool_param;
	ExprTree *alt_spool_expr = NULL;
	bool no_alt_spool = false;
	if ( job_ad ) {
		job_ad->EvaluateAttrBool( ATTR_SOAP_JOB, no_alt_spool );
	} else {
		no_alt_spool = true;
	}
	if ( param( alt_spool_param, "ALTERNATE_JOB_SPOOL" ) && !no_alt_spool ) {
		classad::Value alt_spool_val;
		if ( ParseClassAdRvalExpr(alt_spool_param.c_str(), alt_spool_expr) == 0 ) {
			if ( job_ad->EvaluateExpr( alt_spool_expr, alt_spool_val ) ) {
				if ( alt_spool_val.IsStringValue( spool_base ) ) {
					dprintf( D_FULLDEBUG,"(%d.%d) Using alternate spool direcotry %s\n", cluster, proc, spool_base.c_str() );
				} else {
					dprintf( D_FULLDEBUG,"(%d.%d) ALTERNATE_JOB_SPOOL didn't evaluate to a string\n", cluster, proc );
				}
			} else {
				dprintf( D_FULLDEBUG,"(%d.%d) ALTERNATE_JOB_SPOOL evaluation failed\n", cluster, proc );
			}
			delete alt_spool_expr;
		} else {
			dprintf( D_FULLDEBUG,"(%d.%d) ALTERNATE_JOB_SPOOL parse failed\n", cluster, proc );
		}
	}
	if ( spool_base.empty() ) {
		param( spool_base, "SPOOL" );
	}
	char *job_spool = gen_ckpt_name( spool_base.c_str(), cluster, proc, 0 );
	spool_path = job_spool;
	free( job_spool );
}

void
SpooledJobFiles::getJobSpoolPath(int cluster,int proc,std::string &spool_path)
{
	_getJobSpoolPath(cluster, proc, NULL, spool_path);
}

void
SpooledJobFiles::getJobSpoolPath(const classad::ClassAd *job_ad, std::string &spool_path)
{
	int cluster = -1;
	int proc = -1;

	job_ad->EvaluateAttrInt(ATTR_CLUSTER_ID, cluster);
	job_ad->EvaluateAttrInt(ATTR_PROC_ID, proc);

	_getJobSpoolPath( cluster, proc, job_ad, spool_path );
}

static bool
createJobSpoolDirectory(classad::ClassAd const *job_ad,priv_state desired_priv_state,char const *spool_path)
{
	int cluster=-1,proc=-1;

	job_ad->EvaluateAttrInt(ATTR_CLUSTER_ID,cluster);
	job_ad->EvaluateAttrInt(ATTR_PROC_ID,proc);

#ifndef WIN32
	uid_t spool_path_uid;
#endif

	StatInfo si( spool_path );
	if( si.Error() == SINoFile ) {
		// Parameter JOB_SPOOL_PERMISSIONS can be user / group / world and
		// defines permissions on job spool directory (subject to umask)
		int dir_perms = 0700;
		char *who = param( "JOB_SPOOL_PERMISSIONS" );
		if ( who != NULL)	{
			if ( !strcasecmp( who, "user" ) ) {
				dir_perms = 0700;
			} else if ( !strcasecmp( who, "group" ) ) {
				dir_perms = 0750;
			} else if( !strcasecmp( who, "world" ) ) {
				dir_perms = 0755;
			}
			free( who );
		}

		if(!mkdir_and_parents_if_needed(spool_path,dir_perms,0755,PRIV_CONDOR) )
		{
			dprintf( D_ALWAYS,
					 "Failed to create spool directory for job %d.%d: "
					 "mkdir(%s): %s (errno %d)\n",
					 cluster, proc, spool_path, strerror(errno), errno );
			return false;
		}
#ifndef WIN32
		spool_path_uid = get_condor_uid();
#endif
	} else { 
#ifndef WIN32
			// spool_path already exists, check owner
		spool_path_uid = si.GetOwner();
#endif
	}

	if( !can_switch_ids() ||
		desired_priv_state == PRIV_UNKNOWN ||
		desired_priv_state == PRIV_CONDOR )
	{
		return true; // no need/desire for chowning
	}

	ASSERT( desired_priv_state == PRIV_USER );

#ifndef WIN32

	std::string owner;
	job_ad->EvaluateAttrString( ATTR_OWNER, owner );

	uid_t src_uid = get_condor_uid();
	uid_t dst_uid;
	gid_t dst_gid;
	passwd_cache* p_cache = pcache();
	if( !p_cache->get_user_ids(owner.c_str(), dst_uid, dst_gid) ) {
		dprintf( D_ALWAYS, "(%d.%d) Failed to find UID and GID for "
				 "user %s. Cannot chown %s to user.\n",
				 cluster, proc, owner.c_str(), spool_path );
		return false;
	}

	if( (spool_path_uid != dst_uid) && 
		!recursive_chown(spool_path,src_uid,dst_uid,dst_gid,true) )
	{
		dprintf( D_ALWAYS, "(%d.%d) Failed to chown %s from %d to %d.%d.\n",
				 cluster, proc, spool_path, src_uid, dst_uid, dst_gid );
		return false;
	}

#else	/* WIN32 */

	std::string owner;
	job_ad->EvaluateAttrString(ATTR_OWNER, owner);

	std::string nt_domain;
	job_ad->EvaluateAttrString(ATTR_NT_DOMAIN, nt_domain);

	if(!recursive_chown(spool_path, owner.c_str(), nt_domain.c_str()))
	{
		dprintf( D_ALWAYS, "(%d.%d) Failed to chown %s from to %d\\%d.\n",
		         cluster, proc, spool_path,
				 nt_domain.c_str(), owner.c_str() );
		return false;
	}
#endif

	return true;  // All happy paths lead here
}

bool
SpooledJobFiles::createJobSwapSpoolDirectory(classad::ClassAd const *job_ad,priv_state desired_priv_state )
{
	int cluster=-1,proc=-1;

	if ( param_boolean( "CHOWN_JOB_SPOOL_FILES", false ) == false ) {
		desired_priv_state = PRIV_USER;
	}

	job_ad->EvaluateAttrInt(ATTR_CLUSTER_ID,cluster);
	job_ad->EvaluateAttrInt(ATTR_PROC_ID,proc);

	std::string spool_path;
	_getJobSpoolPath(cluster, proc, job_ad, spool_path);
	spool_path += ".swap";

	if( !::createJobSpoolDirectory(job_ad,desired_priv_state,spool_path.c_str()) )
	{
		return false;
	}

	return true;
}

bool
SpooledJobFiles::createJobSpoolDirectory(classad::ClassAd const *job_ad,priv_state desired_priv_state )
{
	int universe=-1;

	job_ad->EvaluateAttrInt(ATTR_JOB_UNIVERSE,universe);
	if( universe == CONDOR_UNIVERSE_STANDARD ) {
		return createParentSpoolDirectories(job_ad);
	}

	if ( param_boolean( "CHOWN_JOB_SPOOL_FILES", false ) == false ) {
		desired_priv_state = PRIV_USER;
	}

	int cluster=-1,proc=-1;

	job_ad->EvaluateAttrInt(ATTR_CLUSTER_ID,cluster);
	job_ad->EvaluateAttrInt(ATTR_PROC_ID,proc);

	std::string spool_path;
	_getJobSpoolPath(cluster, proc, job_ad, spool_path);

	std::string spool_path_tmp = spool_path.c_str();
	spool_path_tmp += ".tmp";

	if( !::createJobSpoolDirectory(job_ad,desired_priv_state,spool_path.c_str()) ||
		!::createJobSpoolDirectory(job_ad,desired_priv_state,spool_path_tmp.c_str()) )
	{
		return false;
	}

	return true;
}

bool
SpooledJobFiles::createJobSpoolDirectory_PRIV_CONDOR(int cluster, int proc, bool is_standard_universe )
{
	classad::ClassAd dummy_ad;
	dummy_ad.InsertAttr(ATTR_CLUSTER_ID,cluster);
	dummy_ad.InsertAttr(ATTR_PROC_ID,proc);

		// All that matters about the job universe is whether it is
		// standard universe or not.
	int dummy_universe = is_standard_universe ?
		CONDOR_UNIVERSE_STANDARD : CONDOR_UNIVERSE_VANILLA;

	dummy_ad.InsertAttr(ATTR_JOB_UNIVERSE,dummy_universe);

	return createJobSpoolDirectory(&dummy_ad,PRIV_CONDOR);
}

bool
SpooledJobFiles::createParentSpoolDirectories(classad::ClassAd const *job_ad)
{
	int cluster=-1,proc=-1;

	job_ad->EvaluateAttrInt(ATTR_CLUSTER_ID,cluster);
	job_ad->EvaluateAttrInt(ATTR_PROC_ID,proc);

	std::string spool_path;
	_getJobSpoolPath(cluster, proc, job_ad, spool_path);

	std::string parent,junk;
	if( filename_split(spool_path.c_str(),parent,junk) ) {
			// Create directory hierarchy within spool directory.
			// All sub-dirs in hierarchy are owned by condor.
		if( !mkdir_and_parents_if_needed(parent.c_str(),0755,PRIV_CONDOR) ) {
			dprintf(D_ALWAYS,
					"Failed to create parent spool directory %s for job "
					"%d.%d: %s\n",
					parent.c_str(), cluster, proc, strerror(errno));
			return false;
		}
	}
	return true;
}

/*
Removes a single directory passed in.
Returns true on success, false on failure.
On a failure, errno will be set (but may be of dubious quality)
and an error logged.
If the path does not exist, return immediately as success.
if the path exists, but is not a directory, the behavior is currently
to return immediately as success, but in the future might fail with
errno==ENOTDIR.

This assumes that the top level directory requires an euid of condor to
remove.
*/
static bool
remove_spool_directory(const char * dir)
{
	if ( ! IsDirectory(dir) ) { return true; }

	Directory spool_dir(dir, PRIV_ROOT);
	if( ! spool_dir.Remove_Entire_Directory() )
	{
		dprintf(D_ALWAYS,"Failed to remove %s\n", dir);
		errno = EPERM; // Wild guess.
		return false;
	}

	TemporaryPrivSentry tps(PRIV_CONDOR);
	if( rmdir(dir) == 0 ) { return true; }
	// Save errno in case dprintf mangles.
	int tmp_errno = errno;
	if( errno != ENOENT ) {
		dprintf(D_ALWAYS,"Failed to remove %s: %s (errno %d)\n",
			dir, strerror(errno), errno );
	}
	errno = tmp_errno;
	return false;
}

void
SpooledJobFiles::removeJobSpoolDirectory(classad::ClassAd * ad)
{
	ASSERT(ad);
	int cluster = -1;
	int proc = -1;
	ad->EvaluateAttrInt(ATTR_CLUSTER_ID, cluster);
	ad->EvaluateAttrInt(ATTR_PROC_ID, proc);

	std::string spool_path;
	_getJobSpoolPath(cluster, proc, ad, spool_path);

	if ( ! IsDirectory(spool_path.c_str()) ) {
		// In this case, we can be fairly sure that these other spool directories
		// that are removed later in this function do not exist.  If they do, they
		// should be removed by preen.  By returning now, we avoid many potentially-
		// expensive filesystem operations.
		return;
	}

	chownSpoolDirectoryToCondor(ad);
	remove_spool_directory(spool_path.c_str());

	std::string tmp_spool_path = spool_path;
	tmp_spool_path += ".tmp";
	remove_spool_directory(tmp_spool_path.c_str());
	removeJobSwapSpoolDirectory(ad);

		// Now attempt to remove the directory from the spool
		// directory hierarchy that is for jobs belonging to this
		// cluster and proc.  This directory may be shared with other
		// jobs, so the directory may not be empty, in which case we
		// expect rmdir to fail.
		// Also try to remove the next directory up, which is for this
		// cluster mod 10000.

	std::string parent_path,junk;
	if( filename_split(spool_path.c_str(),parent_path,junk) ) {
		if( rmdir(parent_path.c_str()) == -1 ) {
			if( errno != ENOTEMPTY && errno != ENOENT ) {
				dprintf(D_ALWAYS,"Failed to remove %s: %s (errno %d)\n",
						parent_path.c_str(), strerror(errno), errno );
			}
		}
	}

	std::string grandparent_path;
	if( filename_split(parent_path.c_str(),grandparent_path,junk) ) {
		if( rmdir(grandparent_path.c_str()) == -1 ) {
			if( errno != ENOTEMPTY && errno != ENOENT ) {
				dprintf(D_ALWAYS,"Failed to remove %s: %s (errno %d)\n",
						grandparent_path.c_str(), strerror(errno), errno );
			}
		}
	}
}

void
SpooledJobFiles::removeJobSwapSpoolDirectory(classad::ClassAd * ad)
{
	ASSERT(ad);
	int cluster = -1;
	int proc = -1;
	ad->EvaluateAttrInt(ATTR_CLUSTER_ID, cluster);
	ad->EvaluateAttrInt(ATTR_PROC_ID, proc);
	std::string spool_path;
	_getJobSpoolPath(cluster, proc, ad, spool_path);

	std::string swap_spool_path = spool_path;
	swap_spool_path += ".swap";
	remove_spool_directory(swap_spool_path.c_str());
}

void
SpooledJobFiles::removeClusterSpooledFiles(int cluster, const char * submit_digest /*=NULL*/)
{
	std::string spool_path;
	std::string parent_path,junk;

	char *buf = GetSpooledExecutablePath(cluster);
	spool_path = buf;
	free(buf);
	int cluster_spool_dir_exists = filename_split(spool_path.c_str(),parent_path,junk) && IsDirectory( parent_path.c_str() );
	
	if ( !cluster_spool_dir_exists ) {
	  // if there is no parent directory of the spool path, there is no cluster spool directory
	  return;
	}

	if( unlink( spool_path.c_str() ) == -1 ) {
		if( errno != ENOENT ) {
			dprintf(D_ALWAYS,"Failed to remove %s: %s (errno %d)\n",
					spool_path.c_str(),strerror(errno),errno);
		}
	}

	// if a submit digest filename was supplied, and the file is in the spool directory
	// try and delete the submit digest.
	//
	if (submit_digest && starts_with_ignore_case(submit_digest, spool_path)) {
		if( unlink( submit_digest ) == -1 ) {
			if( errno != ENOENT ) {
				dprintf(D_ALWAYS,"Failed to remove %s: %s (errno %d)\n",
						submit_digest,strerror(errno),errno);
			}
		}
	}

		// Now attempt to remove the directory from the spool
		// directory hierarchy that is for jobs belonging to this
		// cluster.  This directory may be shared with other jobs, so
		// the directory may not be empty, in which case we expect
		// rmdir to fail.

	if( cluster_spool_dir_exists ) {
		if( rmdir(parent_path.c_str()) == -1 ) {
			if( errno != ENOTEMPTY && errno != ENOENT ) {
				dprintf(D_ALWAYS,"Failed to remove %s: %s (errno %d)\n",
						parent_path.c_str(), strerror(errno), errno );
			}
		}
	}
}

bool
SpooledJobFiles::chownSpoolDirectoryToCondor(classad::ClassAd const *job_ad)
{
	bool result = true;

#ifndef WIN32
	if ( param_boolean( "CHOWN_JOB_SPOOL_FILES", false ) == false ) {
		return true;
	}

	std::string sandbox;
	int cluster=-1,proc=-1;

	job_ad->EvaluateAttrInt(ATTR_CLUSTER_ID,cluster);
	job_ad->EvaluateAttrInt(ATTR_PROC_ID,proc);

	_getJobSpoolPath(cluster, proc, job_ad, sandbox);

	uid_t src_uid = 0;
	uid_t dst_uid = get_condor_uid();
	gid_t dst_gid = get_condor_gid();

	std::string jobOwner;
	job_ad->EvaluateAttrString( ATTR_OWNER, jobOwner );

	passwd_cache* p_cache = pcache();
	if( p_cache->get_user_uid( jobOwner.c_str(), src_uid ) ) {
		if( ! recursive_chown(sandbox.c_str(), src_uid,
							  dst_uid, dst_gid, true) )
		{
			dprintf( D_FULLDEBUG, "(%d.%d) Failed to chown %s from "
					 "%d to %d.%d.  User may run into permissions "
					 "problems when fetching sandbox.\n", 
					 cluster, proc, sandbox.c_str(),
					 src_uid, dst_uid, dst_gid );
			result = false;
		}
	} else {
		dprintf( D_ALWAYS, "(%d.%d) Failed to find UID and GID "
				 "for user %s.  Cannot chown \"%s\".  User may "
				 "run into permissions problems when fetching "
				 "job sandbox.\n", cluster, proc, jobOwner.c_str(),
				 sandbox.c_str() );
		result = false;
	}

#endif

	return result;
}

bool
SpooledJobFiles::jobRequiresSpoolDirectory(classad::ClassAd const *job_ad)
{
	ASSERT(job_ad);
	int stage_in_start = 0;

	job_ad->EvaluateAttrInt( ATTR_STAGE_IN_START, stage_in_start );
	if( stage_in_start > 0 ) {
		return true;
	}

	int univ = CONDOR_UNIVERSE_VANILLA;
	job_ad->EvaluateAttrInt( ATTR_JOB_UNIVERSE, univ );

		// As of 7.5.5, parallel jobs specify JobRequiresSandbox=true,
		// because they use the spool directory for chirp stuff to make
		// sshd work.  For backward compatibility with prior releases,
		// we assume all parallel jobs require this unless they explicitly
		// specify otherwise.
	bool job_requires_sandbox_expr = false;
	bool requires_sandbox = univ == CONDOR_UNIVERSE_PARALLEL ? true : false;

	if( job_ad->EvaluateAttrBool(ATTR_JOB_REQUIRES_SANDBOX, job_requires_sandbox_expr) )
	{
		requires_sandbox = job_requires_sandbox_expr;
	}

	return requires_sandbox;
}
