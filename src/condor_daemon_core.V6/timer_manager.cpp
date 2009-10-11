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
#include "condor_daemon_core.h"

static char* DEFAULT_INDENT = "DaemonCore--> ";

static	TimerManager*	_t = NULL;

	/*	MAX_FIRES_PER_TIMEOUT sets the maximum number of timer handlers
		we will invoke per call to Timeout().  This limit prevents timers
		from starving other kinds other DC handlers (i.e. it make certain
		that we make it back to the Driver() loop occasionally.  The higher
		the number, the more "timely" timer callbacks will be.  The lower
		the number, the more responsive non-timer calls (like commands)
		will be in the face of many timers coming due.
	*/	
const int MAX_FIRES_PER_TIMEOUT = 3;

extern void **curr_dataptr;
extern void **curr_regdataptr;


TimerManager::TimerManager()
{
	if(_t)
	{
		EXCEPT("TimerManager object exists!");
	}
	timer_list = NULL;
	list_tail = NULL;
	timer_ids = 0;
	in_timeout = NULL;
	_t = this; 
	did_reset = false;
	did_cancel = false;
}

TimerManager::~TimerManager()
{
	CancelAllTimers();
}

int TimerManager::NewTimer(Service* s, unsigned deltawhen, Event event, const char* event_descrip,
						   unsigned period)
{
	return( NewTimer(s,deltawhen,event,(Eventcpp)NULL,(Release)NULL,(Releasecpp)NULL,event_descrip,period,NULL) );
}

int TimerManager::NewTimer(Service* s, unsigned deltawhen, Event event, 
						   Release release, const char* event_descrip,
						   unsigned period)
{
	return( NewTimer(s,deltawhen,event,(Eventcpp)NULL,release,(Releasecpp)NULL,event_descrip,period,NULL) );
}

int TimerManager::NewTimer(unsigned deltawhen, Event event, const char* event_descrip,
						   unsigned period)
{
	return( NewTimer((Service *)NULL,deltawhen,event,(Eventcpp)NULL,(Release)NULL,(Releasecpp)NULL,event_descrip,period,NULL) );
}

int TimerManager::NewTimer(Service* s, unsigned deltawhen, Eventcpp event, const char* event_descrip,
						   unsigned period)
{
	if ( !s ) {
		dprintf( D_DAEMONCORE,"DaemonCore NewTimer() called with c++ pointer & NULL Service*\n");
		return -1;
	}
	return( NewTimer(s,deltawhen,(Event)NULL,event,(Release)NULL,(Releasecpp)NULL,event_descrip,period,NULL) );
}

int TimerManager::NewTimer (Service* s,const Timeslice &timeslice,Eventcpp event,const char * event_descrip)
{
	return NewTimer(s,0,(Event)NULL,event,(Release)NULL,(Releasecpp)NULL,event_descrip,0,&timeslice);
}


// Add a new event in the timer list. if period is 0, this event is a one time
// event instead of periodical
int TimerManager::NewTimer(Service* s, unsigned deltawhen, Event event, Eventcpp eventcpp,
		Release release, Releasecpp releasecpp, const char *event_descrip, unsigned period, 
		const Timeslice *timeslice)
{
	Timer*		new_timer;

	dprintf( D_DAEMONCORE, "in DaemonCore NewTimer()\n" );
	new_timer = new Timer;
	if ( new_timer == NULL ) {
		dprintf( D_ALWAYS, "DaemonCore: Unable to allocate new timer\n" );
		return -1;
	}

	new_timer->handler = event;
	new_timer->handlercpp = eventcpp;
	new_timer->release = release;
	new_timer->releasecpp = releasecpp;
	new_timer->period = period;
	new_timer->service = s; 

	if( timeslice ) {
		new_timer->timeslice = new Timeslice( *timeslice );
		deltawhen = new_timer->timeslice->getTimeToNextRun();
	}
	else {
		new_timer->timeslice = NULL;
	}

	if ( TIMER_NEVER == deltawhen ) {
		new_timer->when = TIME_T_NEVER;
	} else {
		new_timer->when = deltawhen + time(NULL);
	}
	new_timer->data_ptr = NULL;
	if ( event_descrip ) 
		new_timer->event_descrip = strdup(event_descrip);
	else
		new_timer->event_descrip = EMPTY_DESCRIP;


	new_timer->id = timer_ids++;		


	InsertTimer( new_timer );

	DumpTimerList(D_DAEMONCORE | D_FULLDEBUG);

	// Update curr_regdataptr for SetDataPtr()
	curr_regdataptr = &(new_timer->data_ptr);

	dprintf(D_DAEMONCORE,"leaving DaemonCore NewTimer, id=%d\n",new_timer->id);

	return	new_timer->id;
}

int TimerManager::ResetTimer(int id, unsigned when, unsigned period)
{
	Timer*			timer_ptr;
	Timer*			trail_ptr;

	dprintf( D_DAEMONCORE, "In reset_timer(), id=%d, time=%d, period=%d\n",id,when,period);
	if (timer_list == NULL) {
		dprintf( D_DAEMONCORE, "Reseting Timer from empty list!\n");
		return -1;
	}

	timer_ptr = timer_list;
	trail_ptr = NULL;
	while ( timer_ptr && timer_ptr->id != id ) {
		trail_ptr = timer_ptr;
		timer_ptr = timer_ptr->next;
	}

	if ( timer_ptr == NULL ) {
		dprintf( D_ALWAYS, "Timer %d not found\n",id );
		return -1;
	}
	if ( timer_ptr->timeslice ) {
		dprintf( D_DAEMONCORE, "Timer %d with timeslice can't be reset\n",
				 id );
		return 0;
	} else {
		if ( when == TIMER_NEVER ) {
			timer_ptr->when = TIME_T_NEVER;
		} else {
			timer_ptr->when = when + time(NULL);
		}
		timer_ptr->period = period;
	}

	RemoveTimer( timer_ptr, trail_ptr );
	InsertTimer( timer_ptr );

	if ( in_timeout == timer_ptr ) {
		// We're inside the handler for this timer. Let Timeout() know
		// the timer has already been reset for its next call.
		did_reset = true;
	}

	return 0;
}

int TimerManager::CancelTimer(int id)
{
	Timer*		timer_ptr;
	Timer*		trail_ptr;

	dprintf( D_DAEMONCORE, "In cancel_timer(), id=%d\n",id);
	if (timer_list == NULL) {
		dprintf( D_DAEMONCORE, "Removing Timer from empty list!\n");
		return -1;
	}

	timer_ptr = timer_list;
	trail_ptr = NULL;
	while ( timer_ptr && timer_ptr->id != id ) {
		trail_ptr = timer_ptr;
		timer_ptr = timer_ptr->next;
	}

	if ( timer_ptr == NULL ) {
		dprintf( D_ALWAYS, "Timer %d not found\n",id );
		return -1;
	}

	RemoveTimer( timer_ptr, trail_ptr );

	if ( in_timeout == timer_ptr ) {
		// We're inside the handler for this timer. Don't delete it,
		// since Timeout() still needs it. Timeout() will delete it once
		// it's done with it.
		did_cancel = true;
	} else {
		DeleteTimer( timer_ptr );
	}

	return 0;
}

void TimerManager::CancelAllTimers()
{
	Timer		*timer_ptr;
	Service		*service;
	Release		release;
	Releasecpp	releasecpp;

	while( timer_list != NULL ) {
		timer_ptr = timer_list;
		timer_list = timer_list->next;
		if( in_timeout == timer_ptr ) {
				// We get here if somebody calls exit from inside a timer.
			did_cancel = true;
		}
		else {
			DeleteTimer( timer_ptr );
		}
	}
	timer_list = NULL;
	list_tail = NULL;
}

// Timeout() is called when a select() time out.  Returns number of seconds
// until next timeout event, a 0 if there are no more events, or a -1 if
// called while a handler is active (i.e. handler calls Timeout;
// Timeout is not re-entrant).
int
TimerManager::Timeout()
{
	int				result, timer_check_cntr;
	time_t			now, time_sample;
	int				num_fires = 0;	// num of handlers called in this timeout

	if ( in_timeout != NULL ) {
		dprintf(D_DAEMONCORE,"DaemonCore Timeout() called and in_timeout is non-NULL\n");
		if ( timer_list == NULL ) {
			result = 0;
		} else {
			result = (timer_list->when) - time(NULL);
		}
		if ( result < 0 ) {
			result = 0;
		}
		return(result);
	}
		
	dprintf( D_DAEMONCORE, "In DaemonCore Timeout()\n");

	if (timer_list == NULL) {
		dprintf( D_DAEMONCORE, "Empty timer list, nothing to do\n" );
	}

	time(&now);
	timer_check_cntr = 0;

	DumpTimerList(D_DAEMONCORE | D_FULLDEBUG);

	// loop until all handlers that should have been called by now or before
	// are invoked and renewed if periodic.  Remember that NewTimer and CancelTimer
	// keep the timer_list happily sorted on "when" for us.  We use "now" as a 
	// variable so that if some of these handler functions run for a long time,
	// we do not sit in this loop forever.
	// we make certain we do not call more than "max_fires" handlers in a 
	// single timeout --- this ensures that timers don't starve out the rest
	// of daemonCore if a timer handler resets itself to 0.
	while( (timer_list != NULL) && (timer_list->when <= now ) && 
		   (num_fires++ < MAX_FIRES_PER_TIMEOUT)) 
	{
		// DumpTimerList(D_DAEMONCORE | D_FULLDEBUG);

		in_timeout = timer_list;

		// In some cases, resuming from a suspend can cause the system
		// clock to become temporarily skewed, causing crazy things to 
		// happen with our timers (particularly for sending updates to
		// the collector). So, to correct for such skews, we routinely
		// check to make sure that 'now' is not in the future.

		timer_check_cntr++; 

			// since time() is somewhat expensive, we 
			// only check every 10 times we loop 
			
		if ( timer_check_cntr > 10 ) {

			timer_check_cntr = 0;

			time(&time_sample);
			if (now > time_sample) {
				dprintf(D_ALWAYS, "DaemonCore: Clock skew detected "
					"(time=%ld; now=%ld). Resetting TimerManager's "
					"notion of 'now'\n", (long) time_sample, 
					(long) now);
				now = time_sample;
			}
		}

		// Update curr_dataptr for GetDataPtr()
		curr_dataptr = &(in_timeout->data_ptr);

		// Initialize our flag so we know if ResetTimer was called.
		did_reset = false;
		did_cancel = false;

		// Log a message before calling handler, but only if
		// D_FULLDEBUG is also enabled.
		if (DebugFlags & D_FULLDEBUG) {
			dprintf(D_COMMAND, "Calling Timer handler %d (%s)\n",
					in_timeout->id, in_timeout->event_descrip);
		}

		if( in_timeout->timeslice ) {
			in_timeout->timeslice->setStartTimeNow();
		}

		// Now we call the registered handler.  If we were told that the handler
		// is a c++ method, we call the handler from the c++ object referenced 
		// by service*.  If we were told the handler is a c function, we call
		// it and pass the service* as a parameter.
		if ( in_timeout->handlercpp ) {
			// typedef int (*Eventcpp)()
			((in_timeout->service)->*(in_timeout->handlercpp))();
		} else {
			// typedef int (*Event)(Service*)
			(*(in_timeout->handler))(in_timeout->service);
		}

		if( in_timeout->timeslice ) {
			in_timeout->timeslice->setFinishTimeNow();
		}

		if (DebugFlags & D_FULLDEBUG) {
			if( in_timeout->timeslice ) {
				dprintf(D_COMMAND, "Return from Timer handler %d (%s) - took %.3fs\n",
						in_timeout->id, in_timeout->event_descrip,
						in_timeout->timeslice->getLastDuration() );
			}
			else {
				dprintf(D_COMMAND, "Return from Timer handler %d (%s)\n",
						in_timeout->id, in_timeout->event_descrip);
			}
		}

		// Make sure we didn't leak our priv state
		daemonCore->CheckPrivState();

		// Clear curr_dataptr
		curr_dataptr = NULL;

		if ( did_cancel ) {
			// Timer was canceled inside its handler. All we need to do
			// is delete it.
			DeleteTimer( in_timeout );
		} else if ( !did_reset ) {
			// here we remove the timer we just serviced, or renew it if it is 
			// periodic.
			RemoveTimer( in_timeout, NULL );
			if ( in_timeout->period > 0 || in_timeout->timeslice ) {
				if ( in_timeout->timeslice ) {
					in_timeout->when = in_timeout->timeslice->getTimeToNextRun();
				} else {
					in_timeout->when = in_timeout->period + time(NULL);
				}
				InsertTimer( in_timeout );
			} else {
				// timer is not perodic; it is just a one-time event.  we just called
				// the handler, so now just delete it. 
				DeleteTimer( in_timeout );
			}
		}
	}  // end of while loop


	// set result to number of seconds until next event.  get an update on the
	// time from time() in case the handlers we called above took significant time.
	if ( timer_list == NULL ) {
		// we set result to be -1 so that we do not busy poll.
		// a -1 return value will tell the DaemonCore:Driver to use select with
		// no timeout.
		result = -1;
	} else {
		result = (timer_list->when) - time(NULL);
		if (result < 0)
			result = 0;
	}

	dprintf( D_DAEMONCORE, "DaemonCore Timeout() Complete, returning %d \n",result);
	in_timeout = NULL;
	return(result);
}


void TimerManager::DumpTimerList(int flag, const char* indent)
{
	Timer	*timer_ptr;
	char	*ptmp;

	// we want to allow flag to be "D_FULLDEBUG | D_DAEMONCORE",
	// and only have output if _both_ are specified by the user
	// in the condor_config.  this is a little different than
	// what dprintf does by itself ( which is just 
	// flag & DebugFlags > 0 ), so our own check here:
	if ( (flag & DebugFlags) != flag )
		return;

	if ( indent == NULL) 
		indent = DEFAULT_INDENT;

	dprintf(flag, "\n");
	dprintf(flag, "%sTimers\n", indent);
	dprintf(flag, "%s~~~~~~\n", indent);
	for(timer_ptr = timer_list; timer_ptr != NULL; timer_ptr = timer_ptr->next)
	{
		if ( timer_ptr->event_descrip )
			ptmp = timer_ptr->event_descrip;
		else
			ptmp = "NULL";

		MyString slice_desc;
		if( !timer_ptr->timeslice ) {
			slice_desc.sprintf("period = %d, ", timer_ptr->period);
		}
		else {
			slice_desc.sprintf_cat("timeslice = %.3g, ",
								   timer_ptr->timeslice->getTimeslice());
			if( timer_ptr->timeslice->getDefaultInterval() ) {
				slice_desc.sprintf_cat("period = %.1f, ",
								   timer_ptr->timeslice->getDefaultInterval());
			}
			if( timer_ptr->timeslice->getInitialInterval() ) {
				slice_desc.sprintf_cat("initial period = %.1f, ",
								   timer_ptr->timeslice->getInitialInterval());
			}
			if( timer_ptr->timeslice->getMinInterval() ) {
				slice_desc.sprintf_cat("min period = %.1f, ",
								   timer_ptr->timeslice->getMinInterval());
			}
			if( timer_ptr->timeslice->getMaxInterval() ) {
				slice_desc.sprintf_cat("max period = %.1f, ",
								   timer_ptr->timeslice->getMaxInterval());
			}
		}
		dprintf(flag, 
				"%sid = %d, when = %ld, %shandler_descrip=<%s>\n", 
				indent, timer_ptr->id, (long)timer_ptr->when, 
				slice_desc.Value(),ptmp);
	}
	dprintf(flag, "\n");
}

void TimerManager::Start()
{
	struct timeval		timer;
	int					rv;

	for(;;)
	{
		// NOTE: on Linux we need to re-initialize timer
		// everytime we call select().  We might need it on
		// other platforms as well, so we leave it here for
		// everyone.

		// Timeout() also returns how many seconds until next 
		// event, as well as calls any event handlers whose time has come
		timer.tv_sec = Timeout();  
		timer.tv_usec = 0;
		if ( timer.tv_sec == 0 ) {
			// no timer events registered...  only a signal
			// can save us now!!
			dprintf(D_DAEMONCORE,"TimerManager::Start() about to block with no events!\n");
			rv = select(0,0,0,0,NULL);
		} else {
			dprintf(D_DAEMONCORE,
				"TimerManager::Start() about to block, timeout=%ld\n",
				(long)timer.tv_sec);
			rv = select(0,0,0,0, &timer);
		}		
	}
}

void TimerManager::RemoveTimer( Timer *timer, Timer *prev )
{
	if ( timer == NULL || ( prev && prev->next != timer ) ||
		 ( !prev && timer != timer_list ) ) {
		EXCEPT( "Bad call to TimerManager::RemoveTimer()!\n" );
	}

	if ( timer == timer_list ) {
		timer_list = timer_list->next;
	}
	if ( timer == list_tail ) {
		list_tail = prev;
	}
	if ( prev ) {
		prev->next = timer->next;
	}
}

void TimerManager::InsertTimer( Timer *new_timer )
{
	if ( timer_list == NULL ) {
		// list is empty, place ours in front
		timer_list = new_timer;
		list_tail = new_timer;
		new_timer->next = NULL;
			// since we have a new first timer, we must wake up select
		daemonCore->Wake_up_select();
	} else {
		// list is not empty, so keep timer_list ordered from soonest to
		// farthest (i.e. sorted on "when").
		// Note: when doing the comparisons, we always use "<" instead
		// of "<=" -- this makes certain we "round-robin" across 
		// timers that constantly reset themselves to zero.
		if ( new_timer->when < timer_list->when ) {
			// make the this new timer first in line
			new_timer->next = timer_list;
			timer_list = new_timer;
			// since we have a new first timer, we must wake up select
			daemonCore->Wake_up_select();
		} else if ( new_timer->when == TIME_T_NEVER ) {
			// Our new timer goes to the very back of the list.
			new_timer->next = NULL;
			list_tail->next = new_timer;
			list_tail = new_timer;
		} else {
			Timer* timer_ptr;
			Timer* trail_ptr = NULL;
			for (timer_ptr = timer_list; timer_ptr != NULL; 
				 timer_ptr = timer_ptr->next ) 
			{
				if (new_timer->when < timer_ptr->when) {
					break;
				}
				trail_ptr = timer_ptr;
			}
			ASSERT( trail_ptr );
			new_timer->next = timer_ptr;
			trail_ptr->next = new_timer;
			if ( trail_ptr == list_tail ) {
				list_tail = new_timer;
			}
		}
	}
}

void TimerManager::DeleteTimer( Timer *timer )
{
	// free the data_ptr
	if ( timer->releasecpp ) {
		((timer->service)->*(timer->releasecpp))(timer->data_ptr);
	} else if ( timer->release ) {
		(*(timer->release))(timer->data_ptr);
	}

	// free event_descrip
	daemonCore->free_descrip( timer->event_descrip );

	// set curr_dataptr to NULL if a handler is removing itself. 
	if ( curr_dataptr == &(timer->data_ptr) )
		curr_dataptr = NULL;
	if ( curr_regdataptr == &(timer->data_ptr) )
		curr_regdataptr = NULL;

	delete timer->timeslice;
	delete timer;
}
