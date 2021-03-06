      

*condor_master*
================

The master HTCondor Daemon
:index:`condor_master<single: condor_master; HTCondor commands>`
:index:`condor_master daemon`

Synopsis
--------

**condor_master**

Description
-----------

This daemon is responsible for keeping all the rest of the HTCondor
daemons running on each machine in your pool. It spawns the other
daemons, and periodically checks to see if there are new binaries
installed for any of them. If there are, the *condor_master* will
restart the affected daemons. In addition, if any daemon crashes, the
*condor_master* will send e-mail to the HTCondor Administrator of your
pool and restart the daemon. The *condor_master* also supports various
administrative commands that let you start, stop or reconfigure daemons
remotely. The *condor_master* will run on every machine in your
HTCondor pool, regardless of what functions each machine are performing.
Additionally, on Linux platforms, if you start the *condor_master* as
root, it will tune (but never decrease) certain kernel parameters
important to HTCondor's performance.

The ``DAEMON_LIST`` :index:`DAEMON_LIST` configuration macro is
used by the *condor_master* to provide a per-machine list of daemons
that should be started and kept running. For daemons that are specified
in the ``DC_DAEMON_LIST`` configuration macro, the *condor_master*
daemon will spawn them automatically appending a *-f* argument. For
those listed in ``DAEMON_LIST``, but not in ``DC_DAEMON_LIST``, there
will be no *-f* argument.

Options
-------

 **-n** *name*
    Provides an alternate name for the *condor_master* to override that
    given by the ``MASTER_NAME`` :index:`MASTER_NAME`
    configuration variable.

Author
------

Center for High Throughput Computing, University of Wisconsin-Madison

Copyright
---------

Copyright © 1990-2019 Center for High Throughput Computing, Computer
Sciences Department, University of Wisconsin-Madison, Madison, WI. All
Rights Reserved. Licensed under the Apache License, Version 2.0. 7

      
