/* device.cpp - abstract base class for devices

   Copyright (c) 2018, Joerg Hoppe
   j_hoppe@t-online.de, www.retrocmp.com

   Permission is hereby granted, free of charge, to any person obtaining a
   copy of this software and associated documentation files (the "Software"),
   to deal in the Software without restriction, including without limitation
   the rights to use, copy, modify, merge, publish, distribute, sublicense,
   and/or sell copies of the Software, and to permit persons to whom the
   Software is furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
   JOERG HOPPE BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
   IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


   12-nov-2018  JH      entered beta phase

 Abstract device, with or without UNIBUS registers.
 maybe mass storage controller, storage drive or other UNIBUS device
 sets device register values depending on internal status,
 reacts on register read/write over UNIBUS by evaluation of PRU events.

 A device
 - has a worker()
 - has a logger
 - has parameters
*/
#define _DEVICE_CPP_

#include <string.h>
#include <assert.h>
#include <iostream>
#include <fstream>
#include <string>
#include <algorithm>
using namespace std;

#include "utils.hpp"
#include "logger.hpp"
#include "device.hpp"

// declare device list of class separate
list<device_c *> device_c::mydevices;

// argument is a device_c
// called reentrant in parallel for all different devices

// called on cancel and exit()
static void device_worker_pthread_cleanup_handler(void *context) {
	device_c *device = (device_c *) context;
#define this device // make INFO work
	device->worker_terminate = false;
	device->worker_terminated = true; // ended on its own or on worker_terminate
	INFO("Worker terminated for device %s.", device->name.value.c_str());
	device->worker_terminate = false;
	device->worker_terminated = true; // ended on its own or on worker_terminate
//	printf("cleanup for device %s\n", device->name.value.c_str()) ;
#undef this
}

static void *device_worker_pthread_wrapper(void *context) {
	device_c *device = (device_c *) context;
	int	oldstate ; // not used
#define this device // make INFO work
	// call real worker
	INFO("%s::worker() started", device->name.value.c_str());
	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &oldstate) ;
	pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, &oldstate) ; //ASYNCH not allowed!
	device->worker_terminate = false;
	device->worker_terminated = false;
	pthread_cleanup_push(device_worker_pthread_cleanup_handler, device) ;
	device->worker();
	pthread_cleanup_pop(1) ; // call cleanup_handler on regular exit
	// not reached on pthread_cancel()
#undef this
	return NULL;
}

device_c::device_c() {
	// create work thread and run virtual "worker()" function in parallel

	// add reference to device to class list
	mydevices.push_back(this);

	parent = NULL;

	worker_terminate = false;
	worker_terminated = true;

	// do not link params to this device over param constructor
	// creation order of vector vs params?
	name.parameterized = this;
	type_name.parameterized = this;
	verbosity.parameterized = this;
	verbosity.value = *log_level_ptr; // global default value from logger->logsource
	param_add(&name);
	param_add(&type_name);
	param_add(&emulation_speed);
	param_add(&verbosity);
	emulation_speed.value = 1;
	init_asserted = false;

	// use registered parameters for logger interface
	log_label = name.value; // link logger params to this
	log_level_ptr = &(verbosity.value);
	// do not call virtual "reset()" here, sub class constructors must finish first
}

device_c::~device_c() {
	// registered parameters must be deleted by allocator
	parameter.clear();

	// remove device from class list
	// https://www.safaribooksonline.com/library/view/c-cookbook/0596007612/ch08s05.html
	list<device_c*>::iterator p = find(mydevices.begin(), mydevices.end(), this);
	if (p != mydevices.end())
		mydevices.erase(p);
}



// set priority to max, keep policy, return current priority
// do not change worker_sched_priority
void device_c::worker_boost_realtime_priority() {
	int ret;
	// int prev_priority ;
	struct sched_param params;
	// ret = pthread_getschedparam(pthread_self(), &policy, &params);
	// assert(ret == 0) ;
	// prev_priority = params.sched_priority ;
	// set to max for current policy (FIFO or RR)
	params.sched_priority = sched_get_priority_max(worker_sched_policy);
	ret = pthread_setschedparam(pthread_self(), worker_sched_policy, &params);
	assert(ret == 0);
	// return prev_priority ;
}

// fast set to saved worker_sched_policy
void device_c::worker_restore_realtime_priority() {
	int ret;
	struct sched_param params;
	params.sched_priority = worker_sched_priority;		// std val, from init()
	ret = pthread_setschedparam(pthread_self(), worker_sched_policy, &params);
	assert(ret == 0);
}

// http://www.yonch.com/tech/82-linux-thread-priority
void device_c::worker_init_realtime_priority(enum worker_priority_e priority) {
	/* 1. set RT priority to 100% CPU time, without scheduler failsave.
	 * Endless loop in worker() will now hang the machine
	 */
	string rtperiod_path = "/proc/sys/kernel/sched_rt_runtime_us";
	// disable while debugging:: each error in thread requires a powercycle-reboot
	bool rttotal = true;
//	bool rttotal = false;
	fstream rtperiod;
	// /proc/sys/kernel/sched_rt_period_us containing 1000000 and /proc/sys/kernel/sched_rt_runtime_us containing 950000
	// See https://www.kernel.org/doc/Documentation/scheduler/sched-rt-group.txt

	switch (priority) {
	case rt_max:
		// 1. assert path exists
		if (!fileExists(rtperiod_path)) {
			WARNING("kernel param %s not found.\n"
					"Verify \"uname -a\" shows a \"PREEMPT RT\" kernel build!",
					rtperiod_path.c_str());
		} else {
			if (rttotal) {
				// 2. set to -1 = unlimited
				rtperiod.open(rtperiod_path, ios::out | ios::trunc);
				rtperiod << "-1\n";
				rtperiod.close();
			}
			// 3. verify -1
			string line;
			rtperiod.open(rtperiod_path, ios::in);
			getline(rtperiod, line);
			rtperiod.close();
			if (line != "-1") {
				WARNING("can not set kernel param %s to \"-1\", is \"%s\".\n"
						"unibusadapter_c::worker() may get interrupt by other tasks,\n"
						"resulting in ultra-long MSYN/SSYN cycles.", rtperiod_path.c_str(),
						line.c_str());
			} else {
				INFO(
						"%s set to -1:\n"
								"unibusadapter_c::worker() is now un-interruptible and using 100%% RT cpu time.",
						rtperiod_path.c_str());
			}
		}
		worker_sched_policy = SCHED_FIFO;
		worker_sched_priority = sched_get_priority_max(SCHED_FIFO);
		break;
	case rt_device:
		// all device controllers and storage worker must run in parallel
		// (SO RR instead of SCHED), but higher than every Linux stad thread.
		worker_sched_policy = SCHED_RR;
		worker_sched_priority = 50;
		break;
	case none_rt:
		// default Linux time-share scheduling
		worker_sched_policy = SCHED_OTHER;
		worker_sched_priority = 0;
		break;
	}
	/* 2. set thread to max RT priority */
	{
		int ret;
		// We'll operate on the currently running thread.
		pthread_t this_thread = pthread_self();
		// struct sched_param is used to store the scheduling priority
		struct sched_param params;
		params.sched_priority = worker_sched_priority;
		INFO("Trying to set thread realtime priority = %d", (int)params.sched_priority);

		// Attempt to set thread real-time priority
		ret = pthread_setschedparam(this_thread, worker_sched_policy, &params);
		if (ret != 0) {
			// Print the error
			ERROR("Unsuccessful in setting thread realtime prio");
			return;
		}
		// Now verify the change in thread priority
		int policy = 0;
		ret = pthread_getschedparam(this_thread, &policy, &params);
		if (ret != 0) {
			ERROR("Couldn't retrieve real-time scheduling parameters");
			return;
		}

		// Check the correct policy was applied
		if (policy != SCHED_FIFO && policy != SCHED_RR) {
			INFO("Scheduling is not RT: neither SCHED_FIFO nor SCHED_RR!");
		} else {
			INFO("Scheduling is at RT priority.");
		}

		// Print thread scheduling priority
		INFO("Thread priority is %d", (int)params.sched_priority);
	}
}

/* worker_start - executes threads
 *
 * use of C++11 std::thread failed:
 * thead.join() crashes with random system_errors
 * So use classic "pthread" wrapper
 */

void device_c::worker_start(void) {
	worker_terminate = false;
	{
		// start pthread
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		// pthread_attr_setstacksize(&attr, 1024*1024);
		int status = pthread_create(&worker_pthread, &attr, &device_worker_pthread_wrapper,
				(void *) this);
		if (status != 0) {
			FATAL("Failed to create pthread with status = %d", status);
		}
		pthread_attr_destroy(&attr) ; // why?
	}
}

void device_c::worker_stop(void) {
	timeout_c timeout;
	int status;
	if (worker_terminated) {
		DEBUG("%s.worker_stop(): already terminated.", name.name.c_str());
		return;
	}
	INFO("Waiting for %s.worker() to stop ...", name.value.c_str());
	worker_terminate = true;
	// 100ms
	timeout.wait_ms(100);
	// worker_wrapper must do "worker_terminated = true;" on exit
	if (!worker_terminated) {
		// if thread is hanging in pthread_cond_wait(): send a cancellation request
		status = pthread_cancel(worker_pthread);
		if (status != 0)
			FATAL("Failed to send cancellation request to worker_pthread with status = %d", status);
	}

	// !! If crosscompling: this causes a crash in the worker thread
	// !! at pthread_cond_wait() or other cancellation points.
	// !! No problem for compiles build on BBB itself.
	status = pthread_join(worker_pthread, NULL);
	if (status != 0) {
		FATAL("Failed to join worker_pthread with status = %d", status);
	}
}

