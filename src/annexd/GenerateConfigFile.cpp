#include "condor_common.h"
#include "classad_collection.h"
#include "gahp-client.h"
#include "Functor.h"
#include "GenerateConfigFile.h"

int
GenerateConfigFile::operator() () {
	dprintf( D_FULLDEBUG, "GenerateConfigFile::operator()\n" );

	std::map< std::string, std::string > mapping;
	mapping[ "S3BucketName" ] = "ANNEX_DEFAULT_S3_BUCKET";
	mapping[ "odiLeaseFunctionARN" ] = "ANNEX_DEFAULT_ODI_LEASE_FUNCTION_ARN";
	mapping[ "sfrLeaseFunctionARN" ] = "ANNEX_DEFAULT_SFR_LEASE_FUNCTION_ARN";
	mapping[ "InstanceProfileARN" ] = "ANNEX_DEFAULT_ODI_INSTANCE_PROFILE_ARN";
	mapping[ "SecurityGroupID" ] = "ANNEX_DEFAULT_ODI_SECURITY_GROUP_IDS";
	mapping[ "KeyName" ] = "ANNEX_DEFAULT_ODI_KEY_NAME";
	mapping[ "AccessKeyFile" ] = "ANNEX_DEFAULT_ACCESS_KEY_FILE";
	mapping[ "SecretKeyFile" ] = "ANNEX_DEFAULT_SECRET_KEY_FILE";

	fprintf( stdout, "\n" );
	fprintf( stdout, "# Generated by condor_annex -setup.\n" );

	std::string value;
	for( auto i = mapping.begin(); i != mapping.end(); ++i ) {
		value.clear();
		scratchpad->LookupString( i->first.c_str(), value );
		if(! value.empty()) {
			fprintf( stdout, "%s = %s\n", i->second.c_str(), value.c_str() );
		}
	}

	fprintf( stdout, "\n" );

	daemonCore->Reset_Timer( gahp->getNotificationTimerId(), 0, TIMER_NEVER );
	return PASS_STREAM;
}

int
GenerateConfigFile::rollback() {
	dprintf( D_FULLDEBUG, "GenerateConfigFile::rollback()\n" );

	// This functor does nothing (to the service), so don't undo anything.

	daemonCore->Reset_Timer( gahp->getNotificationTimerId(), 0, TIMER_NEVER );
	return PASS_STREAM;
}
