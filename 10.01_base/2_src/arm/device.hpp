/* device.hpp - abstract base class for devices

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
 */
#ifndef _DEVICE_HPP_
#define _DEVICE_HPP_

#include <string>
#include <list>
#include <vector>
#include <mutex>
using namespace std;

#include "parameter.hpp"
#include "logsource.hpp"

// abstract unibus device
// maybe mass storage controller, storage drive or other device
// sets device register values depending on internal status,
// reacts on register read/write over UNIBUS by evaluation of PRU events.
class device_c: public logsource_c, public parameterized_c {
private:
	void worker_start(void);
	void worker_stop(void);

public:
	// the class holds a list of pointers to instantiated devices
	// also needed to have a list of threads
	static list<device_c *> mydevices;

	device_c *parent; // example: storagedrive_c.parent is storage-controller

	// name of instance: "RL3"
	parameter_string_c name = parameter_string_c(NULL, "name", "name", /*readonly*/
	true, "Unique identifier of device");
	// name of type: "RL02". normally readonly.
	parameter_string_c type_name = parameter_string_c(NULL, "type", "type", /*readonly*/
	true, "Type");
	// NULL: do not link params to this device automatically over param constructor

	// "enabled": controls device installation to PRU and worker() state.
	parameter_bool_c enabled = parameter_bool_c(NULL, "enabled", "en", true,
			"device installed and ready to use?");

	parameter_unsigned_c emulation_speed = parameter_unsigned_c(NULL, "emulation_speed", "es",
			false, "", "%d", "1 = original speed, > 1: mechanics is this factor faster", 8, 10);
	// 1 = original speed, > 1: mechanics is this factor faster

	parameter_unsigned_c verbosity = parameter_unsigned_c(NULL, "verbosity", "v", false, "",
			"%d", "1 = fatal, 2 = error, 3 = warning, 4 = info, 5 = debug", 8, 10);

	// make data exchange with worker atomic
	std::mutex worker_mutex;

	// scheduler settings for worker thread
	int worker_sched_policy;
	int worker_sched_priority;

	enum worker_priority_e {
		none_rt, // under all RT priorities
		rt_device, // all controeller and storage worker
		rt_max // 100% CPU, uninterruptable
	};
	void worker_init_realtime_priority(enum worker_priority_e priority);
	void worker_boost_realtime_priority(void);
	void worker_restore_realtime_priority(void);

	device_c();
	virtual ~device_c(); // class with virtual functions should have virtual destructors

	virtual bool on_param_changed(parameter_c *param);

	// search in mydevices
	static device_c *find_by_name(char *name);

	// a device can be powered down. use this to define power-up values
	volatile bool power_down;
	virtual void on_power_changed(void) = 0; // reset device, UNIBUS DC_LO

	// every device has a INIT signal, which can be active (asserted) or inactive
	// set/release device from INIT state
	volatile bool init_asserted;
	virtual void on_init_changed(void) = 0; // reset device, like UNIBUS INIT

	// worker thread
	volatile bool worker_terminate; // cmd flag to worker()
	volatile bool worker_terminated; // ACK flag from worker()

	pthread_t worker_pthread;
	virtual void worker(void) = 0; // background worker function
};

#endif

