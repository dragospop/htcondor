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
#include "condor_classad.h"
#include "condor_config.h"
#include "condor_debug.h"
#include "env.h"
#include "user_proc.h"
#include "os_proc.h"
#include "starter.h"
#include "condor_daemon_core.h"
#include "condor_attributes.h"
#include "condor_syscall_mode.h"
#include "syscall_numbers.h"
#include "classad_helpers.h"
#include "sig_name.h"
#include "exit.h"
#include "condor_uid.h"
#include "filename_tools.h"
#include "my_popen.h"
#ifdef WIN32
#include "perm.h"
#include "profile.WINDOWS.h"
#endif

extern CStarter *Starter;


/* OsProc class implementation */

OsProc::OsProc( ClassAd* ad )
{
    dprintf ( D_FULLDEBUG, "In OsProc::OsProc()\n" );
	JobAd = ad;
	is_suspended = false;
	is_checkpointed = false;
	num_pids = 0;
	dumped_core = false;
	m_using_priv_sep = false;
	UserProc::initialize();
}


OsProc::~OsProc()
{
}


int
OsProc::StartJob(FamilyInfo* family_info)
{
	int nice_inc = 0;
	bool has_wrapper = false;

	dprintf(D_FULLDEBUG,"in OsProc::StartJob()\n");

	if ( !JobAd ) {
		dprintf ( D_ALWAYS, "No JobAd in OsProc::StartJob()!\n" );
		return 0;
	}

	MyString JobName;
	if ( JobAd->LookupString( ATTR_JOB_CMD, JobName ) != 1 ) {
		dprintf( D_ALWAYS, "%s not found in JobAd.  Aborting StartJob.\n", 
				 ATTR_JOB_CMD );
		return 0;
	}

	const char* job_iwd = Starter->jic->jobRemoteIWD();
	dprintf( D_ALWAYS, "IWD: %s\n", job_iwd );

		// some operations below will require a PrivSepHelper if
		// PrivSep is enabled (if it's not, privsep_helper will be
		// NULL)
	PrivSepHelper* privsep_helper = Starter->privSepHelper();

		// // // // // // 
		// Arguments
		// // // // // // 

		// prepend the full path to this name so that we
		// don't have to rely on the PATH inside the
		// USER_JOB_WRAPPER or for exec().
	if ( strcmp(CONDOR_EXEC,JobName.Value()) == 0 ) {
		JobName.sprintf( "%s%c%s",
		                 Starter->GetWorkingDir(),
		                 DIR_DELIM_CHAR,
		                 CONDOR_EXEC );
        }
	else if (is_relative_to_cwd(JobName.Value()) && job_iwd && *job_iwd) {
		MyString full_name;
		full_name.sprintf("%s%c%s",
		                  job_iwd,
		                  DIR_DELIM_CHAR,
		                  JobName.Value());
		JobName = full_name;

	}

	if( Starter->isGridshell() ) {
			// if we're a gridshell, just try to chmod our job, since
			// globus probably transfered it for us and left it with
			// bad permissions...
		priv_state old_priv = set_user_priv();
		int retval = chmod( JobName.Value(), S_IRWXU | S_IRWXO | S_IRWXG );
		set_priv( old_priv );
		if( retval < 0 ) {
			dprintf ( D_ALWAYS, "Failed to chmod %s!\n", JobName.Value() );
			return 0;
		}
	} 

	ArgList args;

		// Since we may be adding to the argument list, we may need to deal
		// with platform-specific arg syntax in the user's args in order
		// to successfully merge them with the additional wrapper args.
	args.SetArgV1SyntaxToCurrentPlatform();

		// First, put "condor_exec" at the front of Args, since that
		// will become argv[0] of what we exec(), either the wrapper
		// or the actual job.
		//
		// The Java universe cannot tolerate an incorrect argv[0].
		// For Java, set it correctly.  In a future version, we
		// may consider removing the CONDOR_EXEC feature entirely.
		//
	if( (job_universe == CONDOR_UNIVERSE_JAVA) ) {
		args.AppendArg(JobName.Value());
	} else {
		args.AppendArg(CONDOR_EXEC);
	}

		// Support USER_JOB_WRAPPER parameter...

	char *wrapper = NULL;
	if( (wrapper=param("USER_JOB_WRAPPER")) ) {

			// make certain this wrapper program exists and is executable
		if( access(wrapper,X_OK) < 0 ) {
			dprintf( D_ALWAYS, 
					 "Cannot find/execute USER_JOB_WRAPPER file %s\n",
					 wrapper );
			free( wrapper );
			return 0;
		}
		has_wrapper = true;
			// Now, we've got a valid wrapper.  We want that to become
			// "JobName" so we exec it directly, and we want to put
			// what was the JobName (with the full path) as the first
			// argument to the wrapper
		args.AppendArg(JobName.Value());
		JobName = wrapper;
		free(wrapper);
	} 
		// Either way, we now have to add the user-specified args as
		// the rest of the Args string.
	MyString args_error;
	if(!args.AppendArgsFromClassAd(JobAd,&args_error)) {
		dprintf(D_ALWAYS, "Failed to read job arguments from JobAd.  "
				"Aborting OsProc::StartJob: %s\n",args_error.Value());
		return 0;
	}

		// // // // // // 
		// Environment 
		// // // // // // 

		// Now, instantiate an Env object so we can manipulate the
		// environment as needed.
	Env job_env;

	char *env_str = param( "STARTER_JOB_ENVIRONMENT" );

	MyString env_errors;
	if( ! job_env.MergeFromV1RawOrV2Quoted(env_str,&env_errors) ) {
		dprintf( D_ALWAYS, "Aborting OSProc::StartJob: "
				 "%s\nThe full value for STARTER_JOB_ENVIRONMENT: "
				 "%s\n",env_errors.Value(),env_str);
		free(env_str);
		return 0;
	}
	free(env_str);

	if(!job_env.MergeFrom(JobAd,&env_errors)) {
		dprintf( D_ALWAYS, "Invalid environment found in JobAd.  "
		         "Aborting OsProc::StartJob: %s\n",
		         env_errors.Value());
		return 0;
	}

		// Now, let the starter publish any env vars it wants to into
		// the mainjob's env...
	Starter->PublishToEnv( &job_env );


		// // // // // // 
		// Standard Files
		// // // // // // 

	// handle stdin, stdout, and stderr redirection
	int fds[3];
		// initialize these to -2 to mean they're not specified.
		// -1 will be treated as an error.
	fds[0] = -2; fds[1] = -2; fds[2] = -2;

		// in order to open these files we must have the user's privs:
	priv_state priv;
	priv = set_user_priv();

		// if we're in PrivSep mode, we won't necessarily be able to
		// open the files for the job. getStdFile will return us an
		// open FD in some situations, but otherwise will give us
		// a filename that we'll pass to the PrivSep Switchboard
		//
	bool stdin_ok;
	bool stdout_ok;
	bool stderr_ok;
	MyString privsep_stdin_name;
	MyString privsep_stdout_name;
	MyString privsep_stderr_name;
	if (privsep_helper != NULL) {
		stdin_ok = getStdFile(SFT_IN,
		                      NULL,
		                      true,
		                      "Input file",
		                      &fds[0],
		                      &privsep_stdin_name);
		stdout_ok = getStdFile(SFT_OUT,
		                       NULL,
		                       true,
		                       "Output file",
		                       &fds[1],
		                       &privsep_stdout_name);
		stderr_ok = getStdFile(SFT_ERR,
		                       NULL,
		                       true,
		                       "Error file",
		                       &fds[2],
		                       &privsep_stderr_name);
	}
	else {
		fds[0] = openStdFile( SFT_IN,
		                      NULL,
		                      true,
		                      "Input file");
		stdin_ok = (fds[0] != -1);
		fds[1] = openStdFile( SFT_OUT,
		                      NULL,
		                      true,
		                      "Output file");
		stdout_ok = (fds[1] != -1);
		fds[2] = openStdFile( SFT_ERR,
		                      NULL,
		                      true,
		                      "Error file");
		stderr_ok = (fds[2] != -1);
	}

	/* Bail out if we couldn't open the std files correctly */
	if( !stdin_ok || !stdout_ok || !stderr_ok ) {
			/* only close ones that had been opened correctly */
		if( fds[0] >= 0 ) {
			close(fds[0]);
		}
		if( fds[1] >= 0 ) {
			close(fds[1]);
		}
		if( fds[2] >= 0 ) {
			close(fds[2]);
		}
		dprintf(D_ALWAYS, "Failed to open some/all of the std files...\n");
		dprintf(D_ALWAYS, "Aborting OsProc::StartJob.\n");
		set_priv(priv); /* go back to original priv state before leaving */
		return 0;
	}

		// // // // // // 
		// Misc + Exec
		// // // // // // 

	Starter->jic->notifyJobPreSpawn();

	// compute job's renice value by evaluating the machine's
	// JOB_RENICE_INCREMENT in the context of the job ad...

    char* ptmp = param( "JOB_RENICE_INCREMENT" );
	if( ptmp ) {
			// insert renice expr into our copy of the job ad
		MyString reniceAttr = "Renice = ";
		reniceAttr += ptmp;
		if( !JobAd->Insert( reniceAttr.Value() ) ) {
			dprintf( D_ALWAYS, "ERROR: failed to insert JOB_RENICE_INCREMENT "
				"into job ad, Aborting OsProc::StartJob...\n" );
			free( ptmp );
			return 0;
		}
			// evaluate
		if( JobAd->EvalInteger( "Renice", NULL, nice_inc ) ) {
			dprintf( D_ALWAYS, "Renice expr \"%s\" evaluated to %d\n",
					 ptmp, nice_inc );
		} else {
			dprintf( D_ALWAYS, "WARNING: job renice expr (\"%s\") doesn't "
					 "eval to int!  Using default of 10...\n", ptmp );
			nice_inc = 10;
		}

			// enforce valid ranges for nice_inc
		if( nice_inc < 0 ) {
			dprintf( D_FULLDEBUG, "WARNING: job renice value (%d) is too "
					 "low: adjusted to 0\n", nice_inc );
			nice_inc = 0;
		}
		else if( nice_inc > 19 ) {
			dprintf( D_FULLDEBUG, "WARNING: job renice value (%d) is too "
					 "high: adjusted to 19\n", nice_inc );
			nice_inc = 19;
		}

		ASSERT( ptmp );
		free( ptmp );
		ptmp = NULL;
	} else {
			// if JOB_RENICE_INCREMENT is undefined, default to 10
		nice_inc = 10;
	}

		// in the below dprintfs, we want to skip past argv[0], which
		// is sometimes condor_exec, in the Args string. 

	MyString args_string;
	int start_arg = has_wrapper ? 2 : 1;
	args.GetArgsStringForDisplay(&args_string,start_arg);
	if( has_wrapper ) { 
			// print out exactly what we're doing so folks can debug
			// it, if they need to.
		dprintf( D_ALWAYS, "Using wrapper %s to exec %s\n", JobName.Value(), 
				 args_string.Value() );
	} else {
		dprintf( D_ALWAYS, "About to exec %s %s\n", JobName.Value(),
				 args_string.Value() );
	}

		// Grab the full environment back out of the Env object 
	if(DebugFlags & D_FULLDEBUG) {
		MyString env_string;
		job_env.getDelimitedStringForDisplay(&env_string);
		dprintf(D_FULLDEBUG, "Env = %s\n", env_string.Value());
	}

	// Check to see if we need to start this process paused, and if
	// so, pass the right flag to DC::Create_Process().
	int job_opt_mask = 0;
	if (!param_boolean("JOB_INHERITS_STARTER_ENVIRONMENT",false)) {
		job_opt_mask = 	DCJOBOPT_NO_ENV_INHERIT;
	}
	int suspend_job_at_exec = 0;
	JobAd->LookupBool( ATTR_SUSPEND_JOB_AT_EXEC, suspend_job_at_exec);
	if( suspend_job_at_exec ) {
		dprintf( D_FULLDEBUG, "OsProc::StartJob(): "
				 "Job wants to be suspended at exec\n" );
		job_opt_mask |= DCJOBOPT_SUSPEND_ON_EXEC;
	}

	// If there is a requested coresize for this job, enforce it.
	// It is truncated because you can't put an unsigned integer
	// into a classad. I could rewrite condor's use of ATTR_CORE_SIZE to
	// be a float, but then when that attribute is read/written to the
	// job queue log by/or shared between versions of Condor which view the
	// type of that attribute differently, calamity would arise.
	int core_size_truncated;
	size_t core_size;
	size_t *core_size_ptr = NULL;
	if ( JobAd->LookupInteger( ATTR_CORE_SIZE, core_size_truncated ) ) {
		core_size = (size_t)core_size_truncated;
		core_size_ptr = &core_size;
	}

	int *affinity_mask = makeCpuAffinityMask(Starter->getMySlotNumber());

#if defined ( WIN32 )
    owner_profile_.update ();
    /*************************************************************
    NOTE: We currently *ONLY* support loading slot-user profiles.
    This limitation will be addressed shortly, by allowing regular 
    users to load their registry hive - Ben [2008-09-31]
    **************************************************************/
    bool load_profile = false,
         run_as_owner = false;
    JobAd->LookupBool ( ATTR_JOB_LOAD_PROFILE, load_profile );
    JobAd->LookupBool ( ATTR_JOB_RUNAS_OWNER,  run_as_owner );
    if ( load_profile && !run_as_owner ) {
        if ( owner_profile_.load () ) {
            /* publish the users environment into that of the main 

            job's environment */
            if ( !owner_profile_.environment ( job_env ) ) {
                dprintf ( D_ALWAYS, "OsProc::StartJob(): Failed to "
                    "export owner's environment.\n" );
            }            
        } else {
            dprintf ( D_ALWAYS, "OsProc::StartJob(): Failed to load "
                "owner's profile.\n" );
        }
    }
#endif

	set_priv ( priv );

	if (privsep_helper != NULL) {
		const char* std_file_names[3] = {
			privsep_stdin_name.Value(),
			privsep_stdout_name.Value(),
			privsep_stderr_name.Value()
		};
		JobPid = privsep_helper->create_process(JobName.Value(),
		                                        args,
		                                        job_env,
		                                        job_iwd,
		                                        fds,
		                                        std_file_names,
		                                        nice_inc,
		                                        core_size_ptr,
		                                        1,
		                                        job_opt_mask,
		                                        family_info);
	}
	else {
		JobPid = daemonCore->Create_Process( JobName.Value(),
		                                     args,
		                                     PRIV_USER_FINAL,
		                                     1,
		                                     FALSE,
		                                     &job_env,
		                                     job_iwd,
		                                     family_info,
		                                     NULL,
		                                     fds,
		                                     NULL,
		                                     nice_inc,
		                                     NULL,
		                                     job_opt_mask, 
		                                     core_size_ptr,
											 affinity_mask );
	}

	//NOTE: Create_Process() saves the errno for us if it is an
	//"interesting" error.
	char const *create_process_error = NULL;
	int create_process_errno = errno;
	if(JobPid == FALSE && errno) create_process_error = strerror(errno);

	// now close the descriptors in fds array.  our child has inherited
	// them already, so we should close them so we do not leak descriptors.
	// NOTE, we want to use a special method to close the starter's
	// versions, if that's what we're using, so we don't think we've
	// still got those available in other parts of the code for any
	// reason.
	if ( fds[0] >= 0 ) {
		close(fds[0]);
	}
	if ( fds[1] >= 0 ) {
		close(fds[1]);
	}
	if ( fds[2] >= 0 ) {
		close(fds[2]);
	}

	if ( JobPid == FALSE ) {
		JobPid = -1;

		if(create_process_error) {

			// if the reason Create_Process failed was that registering
			// a family with the ProcD failed, it is indicative of a
			// problem regarding this execute machine, not the job. in
			// this case, we'll want to EXCEPT instead of telling the
			// Shadow to put the job on hold. there are probably other
			// error conditions where EXCEPTing would be more appropriate
			// as well...
			//
			if (create_process_errno == DaemonCore::ERRNO_REGISTRATION_FAILED) {
				EXCEPT("Create_Process failed to register the job with the ProcD");
			}

			MyString err_msg = "Failed to execute '";
			err_msg += JobName;
			err_msg += "'";
			if(!args_string.IsEmpty()) {
				err_msg += " with arguments ";
				err_msg += args_string.Value();
			}
			err_msg += ": ";
			err_msg += create_process_error;
			Starter->jic->notifyStarterError( err_msg.Value(),
			                                  true,
			                                  CONDOR_HOLD_CODE_FailedToCreateProcess,
			                                  create_process_errno );
		}

		EXCEPT("Create_Process(%s,%s, ...) failed",
			JobName.Value(), args_string.Value() );
		return 0;
	}

	num_pids++;

	dprintf(D_ALWAYS,"Create_Process succeeded, pid=%d\n",JobPid);

	job_start_time.getTime();

	return 1;
}


bool
OsProc::JobReaper( int pid, int status )
{
	dprintf( D_FULLDEBUG, "Inside OsProc::JobReaper()\n" );

	if (JobPid == pid) {
			// clear out num_pids... everything under this process
			// should now be gone
		num_pids = 0;

			// check to see if the job dropped a core file.  if so, we
			// want to rename that file so that it won't clobber other
			// core files back at the submit site.
		checkCoreFile();
	}
	return UserProc::JobReaper(pid, status);
}


bool
OsProc::JobExit( void )
{
	int reason;	

	dprintf( D_FULLDEBUG, "Inside OsProc::JobExit()\n" );

	if( requested_exit == true ) {
		if( Starter->jic->hadHold() || Starter->jic->hadRemove() ) {
			reason = JOB_KILLED;
		} else {
			reason = JOB_NOT_CKPTED;
		}
	} else if( dumped_core ) {
		reason = JOB_COREDUMPED;
	} else {
		reason = JOB_EXITED;
	}

#if defined ( WIN32 )
    
    /* If we loaded the user's profile, then we should dump it now */
    if ( owner_profile_.loaded () ) {
        owner_profile_.unload ();
        
        /* !!!! DO NOT DO THIS IN THE FUTURE !!!! */
        owner_profile_.destroy ();
        /* !!!! DO NOT DO THIS IN THE FUTURE !!!! */
        
    }

    priv_state old = set_user_priv ();
    HANDLE user_token = priv_state_get_handle ();
    ASSERT ( user_token );
    
    // Check USE_VISIBLE_DESKTOP in condor_config.  If set to TRUE,
    // then removed our users priveleges from the visible desktop.
    char *use_visible = param("USE_VISIBLE_DESKTOP");    
	if (use_visible && (*use_visible=='T' || *use_visible=='t') ) {        
        /* at this point we can revoke the user's access to
        the visible desktop */
        int RevokeDesktopAccess ( HANDLE ); // prototype
        RevokeDesktopAccess ( user_token );
    }

    set_priv ( old );

#endif

	return Starter->jic->notifyJobExit( exit_status, reason, this );
}



void
OsProc::checkCoreFile( void )
{
	// we must act differently depending on whether PrivSep is enabled.
	// if it's not, we rename any core file we find to
	// core.<cluster>.<proc>. if PrivSep is enabled, we may not have the
	// permissions to do the rename, so we don't. in either case, we
	// add the core file if present to the list of output files
	// (that happens in renameCoreFile for the non-PrivSep case)

	// Since Linux now writes out "core.pid" by default, we should
	// search for that.  Try the JobPid of our first child:
	MyString name_with_pid;
	name_with_pid.sprintf( "core.%d", JobPid );

	if (Starter->privSepHelper() != NULL) {

#if defined(WIN32)
		EXCEPT("PrivSep not yet available on Windows");
#else
		struct stat stat_buf;
		if (stat(name_with_pid.Value(), &stat_buf) != -1) {
			Starter->jic->addToOutputFiles(name_with_pid.Value());
		}
		else if (stat("core", &stat_buf) != -1) {
			Starter->jic->addToOutputFiles("core");
		}
#endif
	}
	else {
		MyString new_name;
		new_name.sprintf( "core.%d.%d",
		                  Starter->jic->jobCluster(), 
		                  Starter->jic->jobProc() );

		if( renameCoreFile(name_with_pid.Value(), new_name.Value()) ) {
			// great, we found it, renameCoreFile() took care of
			// everything we need to do... we're done.
			return;
		}

		// Now, just see if there's a file called "core"
		if( renameCoreFile("core", new_name.Value()) ) {
			return;
		}
	}

	/*
	  maybe we should check for other possible pids (either by
	  using a Directory object to scan all the files in the iwd,
	  or by using the procfamily to test all the known pids under
	  the job).  also, you can configure linux to drop core files
	  with other possible values, not just pid.  however, it gets
	  complicated and it becomes harder and harder to tell if the
	  core files belong to this job or not in the case of a shared
	  file system where we might be running in a directory shared
	  by many jobs...  for now, the above is good enough and will
	  catch most of the problems we're having.
	*/
}


bool
OsProc::renameCoreFile( const char* old_name, const char* new_name )
{
	bool rval = false;
	int t_errno = 0;

	MyString old_full;
	MyString new_full;
	const char* job_iwd = Starter->jic->jobIWD();
	old_full.sprintf( "%s%c%s", job_iwd, DIR_DELIM_CHAR, old_name );
	new_full.sprintf( "%s%c%s", job_iwd, DIR_DELIM_CHAR, new_name );

	priv_state old_priv;

		// we need to do this rename as the user...
	errno = 0;
	old_priv = set_user_priv();
	int ret = rename(old_full.Value(), new_full.Value());
	if( ret != 0 ) {
			// rename failed
		t_errno = errno; // grab errno right away
		rval = false;
	} else { 
			// rename succeeded
		rval = true;
   	}
	set_priv( old_priv );

	if( rval ) {
		dprintf( D_FULLDEBUG, "Found core file '%s', renamed to '%s'\n",
				 old_name, new_name );
		if( dumped_core ) {
			EXCEPT( "IMPOSSIBLE: inside OsProc::renameCoreFile and "
					"dumped_core is already TRUE" );
		}
		dumped_core = true;
			// make sure it'll get transfered back, too.
		Starter->jic->addToOutputFiles( new_name );
	} else if( t_errno != ENOENT ) {
		dprintf( D_ALWAYS, "Failed to rename(%s,%s): errno %d (%s)\n",
				 old_full.Value(), new_full.Value(), t_errno,
				 strerror(t_errno) );
	}
	return rval;
}


void
OsProc::Suspend()
{
	daemonCore->Send_Signal(JobPid, SIGSTOP);
	is_suspended = true;
}

void
OsProc::Continue()
{
	daemonCore->Send_Signal(JobPid, SIGCONT);
	is_suspended = false;
}

bool
OsProc::ShutdownGraceful()
{
	if ( is_suspended ) {
		Continue();
	}
	requested_exit = true;
	daemonCore->Send_Signal(JobPid, soft_kill_sig);
	return false;	// return false says shutdown is pending	
}


bool
OsProc::ShutdownFast()
{
	// We purposely do not do a SIGCONT here, since there is no sense
	// in potentially swapping the job back into memory if our next
	// step is to hard kill it.
	requested_exit = true;
	daemonCore->Send_Signal(JobPid, SIGKILL);
	return false;	// return false says shutdown is pending
}


bool
OsProc::Remove()
{
	if ( is_suspended ) {
		Continue();
	}
	requested_exit = true;
	daemonCore->Send_Signal(JobPid, rm_kill_sig);
	return false;	// return false says shutdown is pending	
}


bool
OsProc::Hold()
{
	if ( is_suspended ) {
		Continue();
	}
	requested_exit = true;
	daemonCore->Send_Signal(JobPid, hold_kill_sig);
	return false;	// return false says shutdown is pending	
}


bool
OsProc::PublishUpdateAd( ClassAd* ad ) 
{
	dprintf( D_FULLDEBUG, "Inside OsProc::PublishUpdateAd()\n" );
	MyString buf;

	if (m_proc_exited) {
		buf.sprintf( "%s=\"Exited\"", ATTR_JOB_STATE );
	} else if( is_checkpointed ) {
		buf.sprintf( "%s=\"Checkpointed\"", ATTR_JOB_STATE );
	} else if( is_suspended ) {
		buf.sprintf( "%s=\"Suspended\"", ATTR_JOB_STATE );
	} else {
		buf.sprintf( "%s=\"Running\"", ATTR_JOB_STATE );
	}
	ad->Insert( buf.Value() );

	buf.sprintf( "%s=%d", ATTR_NUM_PIDS, num_pids );
	ad->Insert( buf.Value() );

	if (m_proc_exited) {
		if( dumped_core ) {
			buf.sprintf( "%s = True", ATTR_JOB_CORE_DUMPED );
			ad->Insert( buf.Value() );
		} // should we put in ATTR_JOB_CORE_DUMPED = false if not?
	}

	return UserProc::PublishUpdateAd( ad );
}

int *
OsProc::makeCpuAffinityMask(int slotId) {
	bool useAffinity = param_boolean("ENFORCE_CPU_AFFINITY", false);

	if (!useAffinity) {
		dprintf(D_FULLDEBUG, "ENFORCE_CPU_AFFINITY not true, not setting affinity\n");	
		return NULL;
	}

	// slotID of 0 means "no slot".  Punt.
	if (slotId < 1) {
		return NULL;
	}

	MyString affinityParam;

	affinityParam.sprintf("SLOT%d_CPU_AFFINITY", slotId);
	char *affinityParamResult = param(affinityParam.Value());

	if (affinityParamResult == NULL) {
		// No specific cpu, assume one-to-one mapping from slotid
		// to cpu affinity

		dprintf(D_FULLDEBUG, "Setting cpu affinity to %d\n", slotId - 1);	
		int *mask = (int *) malloc(sizeof(int) * 2);
		mask[0] = 2;
		mask[1] = slotId - 1; // slots start at 1, cpus at 0
		return mask;
	}

	StringList cpus(affinityParamResult);

	if (cpus.number() < 1) {
		dprintf(D_ALWAYS, "Could not parse affinity string %s, not setting affinity\n", affinityParamResult);
		return NULL;
	}

	int *mask = (int *) malloc(sizeof(int) * (cpus.number() + 1));
	mask[0] = cpus.number() + 1;
	cpus.rewind();
	char *cpu;
	int index = 1;
	while ((cpu = cpus.next())) {
		mask[index++] = atoi(cpu);
	}

	return mask;
}
