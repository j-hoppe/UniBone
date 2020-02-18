/* menu_devices.cpp: user sub menu

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

 16-Nov-2018  JH      created
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <linux/limits.h>

#include "inputline.hpp"
#include "mcout.h"

#include "application.hpp" // own

#include "pru.hpp"
#include "gpios.hpp"
#include "mailbox.h"
#include "iopageregister.h"
#include "parameter.hpp"
#include "unibus.h"
#include "memoryimage.hpp"

#include "unibusadapter.hpp"
#include "unibusdevice.hpp"

#include "storagedrive.hpp"
#include "panel.hpp"
#include "demo_io.hpp"
#include "testcontroller.hpp"
#include "rl11.hpp"
#include "rk11.hpp"
#include "uda.hpp"
#include "dl11w.hpp"
#include "m9312.hpp"
#include "cpu.hpp"

/*** handle loading of memory content  from macro-11 listing ***/
static char memory_filename[PATH_MAX + 1];

// entry_label is program start, typically "start"
// format: 0 = macrop11, 1 = papertape
static void load_memory(memory_fileformat_t format, char *fname, const char *entry_label) {
	codelabel_map_c codelabels ;
	uint32_t firstaddr, lastaddr;
	uint32_t entry_address = MEMORY_ADDRESS_INVALID ;

	bool load_ok;
	bool timeout;
	switch (format) {
	case fileformat_macro11_listing:
		load_ok = membuffer->load_macro11_listing(fname, &codelabels);
		if (codelabels.is_defined(entry_label))
			entry_address = codelabels.get_address(entry_label) ;
		break;
	case fileformat_papertape:
		load_ok = membuffer->load_papertape(fname, &codelabels);
		if (codelabels.size() > 0)
			entry_address = codelabels.begin()->second;
		break;
	default:
		load_ok = false;
	}
	if (load_ok) {
		strcpy(memory_filename, fname);
		membuffer->get_addr_range(&firstaddr, &lastaddr);
		printf(
				"Loaded MACRO-11 listing from file \"%s\" into memory: %d words from %06o to %06o.\n",
				fname, membuffer->get_word_count(), firstaddr, lastaddr);
		if (entry_label == NULL)
			printf("  No entry address label.\n");
		else if (entry_address != MEMORY_ADDRESS_INVALID)
			printf("  Entry address at \"%s\" label is %06o.\n", entry_label,
					entry_address);
		else
			printf("  No entry address at \"%s\" label is %06o.\n", entry_label,
					entry_address);
		unibus->mem_write(membuffer->data.words, firstaddr, lastaddr,
				&timeout);
		if (timeout)
			printf("  Error writing UNIBUS memory\n");
	}
}

static void print_device(device_c *device) {
	unibusdevice_c *ubdevice = dynamic_cast<unibusdevice_c *>(device);
	if (ubdevice)
		printf("- %-12s  Type %s, %s.\n", ubdevice->name.value.c_str(),
				ubdevice->type_name.value.c_str(), ubdevice->get_unibus_resource_info());
	else
		printf("- %-12s  Type %s.\n", device->name.value.c_str(),
				device->type_name.value.c_str());
}

void application_c::menu_devices(const char *menu_code, bool with_emulated_CPU) {
	/** list of usable devices ***/
	bool with_storage_file_test = false;

	bool ready = false;
	bool show_help = true;
	bool memory_emulated = false;
	device_c *cur_device = NULL;
	unibusdevice_c *unibuscontroller = NULL;
	unsigned n_fields;
	char *s_choice;
	char s_opcode[256], s_param[3][256];

	strcpy(memory_filename, "");

//	iopageregisters_init();
	// UNIBUS activity
	// assert(unibus->arbitrator_client) ; // External Bus Arbitrator required
	hardware_startup(pru_c::PRUCODE_UNIBUS);
	// now PRU executing UNIBUS master/slave code, physical PDP-11 CPU as arbitrator required.
	buslatches_output_enable(true);
	
	// devices need physical or emulated CPU Arbitrator 
	// to answer BR and NPR requests.
	if (with_emulated_CPU)
		// not yet active, switches to CLIENT when emulated CPU started
		unibus->set_arbitrator_active(false) ;
	else 
		unibus->set_arbitrator_active(true) ;

	// without PDP-11 CPU no INIT after power ON was generated.
	// Devices may trash the bus lines.
	unibus->init(50);

	unibusadapter->enabled.set(true);

	// 2 demo controller
	cur_device = NULL;
	demo_io_c *demo_io = new demo_io_c();

	// uses all slot resource, can onyl run alone
	//testcontroller_c *test_controller = new testcontroller_c();

	cpu_c *cpu = NULL;
	// create RL11 +  also 4 RL01/02 drives
	RL11_c *RL11 = new RL11_c();
	paneldriver->reset(); // reset I2C, restart worker()
	// create RK11 + drives
	rk11_c *RK11 = new rk11_c();
	// Create UDA50
	uda_c *UDA50 = new uda_c();
	// Create SLU+ LTC
	slu_c *DL11 = new slu_c();
	// to inject characters into DL11 receiver
	std::stringstream dl11_rcv_stream(std::ios::app | std::ios::in | std::ios::out);
	DL11->rs232adapter.stream_rcv = &dl11_rcv_stream;
	DL11->rs232adapter.stream_xmt = NULL; // do not echo output to stdout
	DL11->rs232adapter.baudrate = DL11->baudrate.value; // limit speed of injected chars

	ltc_c *LTC = new ltc_c();

	m9312_c *m9312 = new m9312_c();


//	//demo_regs.install();
//	//demo_regs.worker_start();

	if (with_emulated_CPU) {
		cpu = new cpu_c();
		cpu->enabled.set(true);
	}

	if (with_storage_file_test) {
		const char *testfname = "/tmp/storagedrive_selftest.bin";
		remove(testfname);
		storagedrive_selftest_c dut(testfname, /* block_size*/1024, /* block_count */137);
		dut.test();
	}

	// now devices are "Plugged in". Reset PDP-11.
	unibus->probe_grant_continuity(true);

	while (!ready) {

		if (show_help) {
			show_help = false; // only once
			printf("\n");
			printf("*** Test of device parameter interface and states.\n");
			print_arbitration_info("    ");
			if (cur_device) {
				printf("    Current device is \"%s\"\n", cur_device->name.value.c_str());
				if (unibuscontroller)
					printf("    UNIBUS unibuscontroller base address = %06o\n",
							unibuscontroller->base_addr.value);
			} else
				printf("    No current device selected\n");
			if (memory_emulated) {
				printf("    UNIBUS memory emulated from %06o to %06o.\n",
						emulated_memory_start_addr, emulated_memory_end_addr);
			} else
				printf("    NO UNIBUS memory installed ... device test limited!\n");
			printf("\n");
			printf("m i                  Install (emulate) max UNIBUS memory\n");
			printf("m f [word]           Fill UNIBUS memory (with 0 or other octal value)\n");
			printf("m d                  Dump UNIBUS memory to disk\n");
			printf(
					"m ll <filename>      Load memory content from MACRO-11 listing file (boot loader)\n");
			if (strlen(memory_filename))
				printf("m ll             Reload last memory content from file \"%s\"\n",
						memory_filename);
			printf("m lp <filename>      Load memory content from absolute papertape image\n");
			printf("m lp                 Reload last memory content from file \"%s\"\n",
					memory_filename);
			printf("ld                   List all defined devices\n");
			printf("en <dev>             Enable a device\n");
			printf("dis <dev>            Disable device\n");
			printf("sd <dev>             Select \"current device\"\n");

			if (cur_device) {
				printf("p <param> <val>      Set parameter value of current device\n");
				printf("p <param>            Get parameter value of current device\n");
				printf("p panel              Force parameter update from panel\n");
				printf("p                    Show all parameter of current device\n");
			}
			if (unibuscontroller) {
				printf("d <regname> <val>    Deposit octal value into named device register\n");
				printf("e <regname>          Examine single device register (regno decimal)\n");
				printf("e                    Examine all device registers\n");
			}
			printf("e <addr>             Examine octal UNIBUS address.\n");
			printf("d <addr> <val>       Deposit octal val into UNIBUS address.\n");
			if (DL11->enabled.value) {
				printf(
						"dl11 rcv [<wait_ms>] <string>   inject characters as if DL11 received them.\n");
				printf(
						"                     Before output there's an optional pause of <wait_ms> milliseconds.\n");
				printf(
						"                     <string> uses C-escapes: \"\\r\"= CR, \040 = space, etc.\n");
				printf(
						"dl11 wait <timeout_ms> <string>	wait time until DL11 was ordered to transmit <string>.\n");
				printf("                     On timeout, script execution is terminated.\n");
			}
			printf("dbg c|s|f            Debug log: Clear, Show on console, dump to File.\n");
			printf("                       (file = %s)\n", logger->default_filepath.c_str());
			printf("init                 Pulse UNIBUS INIT\n");
			printf("pwr                  Simulate UNIBUS power cycle (ACLO/DCLO)\n");
			printf("q                    Quit\n");
		}
		s_choice = getchoice(menu_code);

		printf("\n");
		try {
			n_fields = sscanf(s_choice, "%s %s %s %s", s_opcode, s_param[0], s_param[1],
					s_param[2]);
			if (!strcasecmp(s_opcode, "q")) {
				ready = true;
			} else if (!strcasecmp(s_opcode, "init")) {
				unibus->init(50);
			} else if (!strcasecmp(s_opcode, "pwr")) {
				unibus->probe_grant_continuity(true) ;
			} else if (!strcasecmp(s_opcode, "dbg") && n_fields == 2) {
				if (!strcasecmp(s_param[0], "c")) {
					logger->clear();
					unibusadapter->debug_init(); // special diagnostics
					printf("Debug log cleared.\n");
				} else if (!strcasecmp(s_param[0], "s")) {
					unibusadapter->debug_snapshot(); // special diagnostics
					logger->dump();
				} else if (!strcasecmp(s_param[0], "f")) {
					logger->dump(logger->default_filepath);
				}
			} else if (!strcasecmp(s_opcode, "m") && n_fields == 2
					&& !strcasecmp(s_param[0], "i")) {
				// install (emulate) max UNIBUS memory
				memory_emulated = emulate_memory();
				show_help = true; // menu struct changed
			} else if (!strcasecmp(s_opcode, "m") && n_fields >= 2
					&& !strcasecmp(s_param[0], "f")) {
				//  clear UNIBUS memory
				bool timeout;
				uint16_t fillword = 0;
				if (n_fields == 3)
					parse_word(s_param[1], &fillword);
				membuffer->set_addr_range(emulated_memory_start_addr, emulated_memory_end_addr);
				membuffer->fill(fillword);
				// write buffer-> UNIBUS
				printf("Fill memory with %06o, writing UNIBUS memory[%06o:%06o]\n", fillword,
						emulated_memory_start_addr, emulated_memory_end_addr);
				unibus->mem_write(membuffer->data.words,
						emulated_memory_start_addr, emulated_memory_end_addr, &timeout);
				if (timeout)
					printf("Error writing UNIBUS memory!\n");
			} else if (!strcasecmp(s_opcode, "m") && n_fields == 2
					&& !strcasecmp(s_param[0], "d")) {
				// dump UNIBUS memory to disk
				const char * filename = "memory.dump";
				bool timeout;
				// 1. read UNIBUS memory
				uint32_t end_addr = unibus->test_sizer() - 2;
				printf("Reading UNIBUS memory[0:%06o] with DMA\n", end_addr);
				//  clear memory buffer, to be sure content changed
				membuffer->set_addr_range(0, end_addr);
				membuffer->fill(0);

				unibus->mem_read(membuffer->data.words, 0, end_addr,
						&timeout);
				if (timeout)
					printf("Error reading UNIBUS memory!\n");
				else {
					// 1. read UNIBUS memory
					printf("Saving to file \"%s\"\n", filename);
					membuffer->save_binary(filename, end_addr + 2);
				}
			} else if (!strcasecmp(s_opcode, "m") && n_fields == 3
					&& !strcasecmp(s_param[0], "ll")) {
				// m ll <filename>
				load_memory(fileformat_macro11_listing, s_param[1], "start");
			} else if (!strcasecmp(s_opcode, "m") && n_fields == 2
					&& !strcasecmp(s_param[0], "ll") && strlen(memory_filename)) {
				// m ll
				load_memory(fileformat_macro11_listing, memory_filename,
						"start");
			} else if (!strcasecmp(s_opcode, "m") && n_fields == 3
					&& !strcasecmp(s_param[0], "lp")) {
				// m lp <filename>
				load_memory(fileformat_papertape, s_param[1], NULL);
			} else if (!strcasecmp(s_opcode, "m") && n_fields == 2
					&& !strcasecmp(s_param[0], "lp") && strlen(memory_filename)) {
				// m lp
				load_memory(fileformat_papertape, memory_filename, NULL);
			} else if (!strcasecmp(s_opcode, "ld") && n_fields == 1) {
				unsigned n;
				list<device_c *>::iterator it;
				for (n = 0, it = device_c::mydevices.begin(); it != device_c::mydevices.end();
						++it)
					if ((*it)->enabled.value) {
						if (n == 0)
							cout << "Enabled devices:\n";
						n++;
						print_device(*it);
					}
				if (n == 0)
					cout << "No enabled devices.\n";

				for (n = 0, it = device_c::mydevices.begin(); it != device_c::mydevices.end();
						++it)
					if (!(*it)->enabled.value) {
						if (n == 0)
							cout << "Disabled devices:\n";
						n++;
						print_device(*it);
					}
				if (n == 0)
					cout << "No disabled devices.\n";
			} else if (!strcasecmp(s_opcode, "en") && n_fields == 2) {
				device_c *dev = device_c::find_by_name(s_param[0]);
				if (!dev) {
					cout << "Device \"" << s_param[0] << "\" not found.\n";
					show_help = true;
				} else
					dev->enabled.set(true);
			} else if (!strcasecmp(s_opcode, "dis") && n_fields == 2) {
				device_c *dev = device_c::find_by_name(s_param[0]);
				if (!dev) {
					cout << "Device \"" << s_param[0] << "\" not found.\n";
					show_help = true;
				} else
					dev->enabled.set(false);
			} else if (!strcasecmp(s_opcode, "sd") && n_fields == 2) {
				cur_device = device_c::find_by_name(s_param[0]);

				if (!cur_device) {
					cout << "Device \"" << s_param[0] << "\" not found.\n";
					show_help = true;
				} else {
					printf("Current device is \"%s\"\n", cur_device->name.value.c_str());
					// find base address of assoiated UNIBUS unibuscontroller
					if (cur_device != NULL && dynamic_cast<unibusdevice_c *>(cur_device))
						unibuscontroller = dynamic_cast<unibusdevice_c *>(cur_device);
					else if (cur_device->parent != NULL
							&& dynamic_cast<unibusdevice_c *>(cur_device->parent)) {
						unibuscontroller = dynamic_cast<unibusdevice_c *>(cur_device->parent);
						printf("Controller base address = %06o\n",
								unibuscontroller->base_addr.value);
					} else
						unibuscontroller = NULL; // no unibuscontroller found
					show_help = true;
				}
			} else if (cur_device && !strcasecmp(s_opcode, "p") && n_fields == 1) {
				cout << "Parameters of device " << cur_device->name.value << ":\n";
				print_params(cur_device, NULL);
			} else if (cur_device && !strcasecmp(s_opcode, "p") && n_fields == 2
					&& !strcasecmp(s_param[0], "panel")) {
				paneldriver->refresh_params(cur_device);
				// RL11.refresh_params_from_panel(); // all 4 drives
			} else if (cur_device && !strcasecmp(s_opcode, "p") && n_fields == 2) {
				// show selected
				string pn(s_param[0]);
				parameter_c *p = cur_device->param_by_name(pn);
				if (p == NULL)
					cout << "Device \"" << cur_device->name.value << "\" has no parameter \""
							<< pn << "\".\n";
				else
					print_params(cur_device, p);
			} else if (cur_device && !strcasecmp(s_opcode, "p") && n_fields == 3) {
				string pn(s_param[0]);
				parameter_c *p = cur_device->param_by_name(pn);
				if (p == NULL)
					cout << "Device \"" << cur_device->name.value << "\" has no parameter \""
							<< pn << "\".\n";
				else {
					string sval(s_param[1]);
					p->parse(sval);
					print_params(cur_device, p);
				}
			} else if (!strcasecmp(s_opcode, "d") && n_fields == 3) {
				uint32_t addr;
				uint16_t wordbuffer;
				unibusdevice_register_t *reg = NULL;
				if (unibuscontroller)
					reg = unibuscontroller->register_by_name(s_param[0]);
				if (reg) // register name given
					addr = reg->addr;
				else
					// interpret as 18 bit address
					parse_addr18(s_param[0], &addr);

				parse_word(s_param[1], &wordbuffer);
				bool timeout = !unibus->dma(true, UNIBUS_CONTROL_DATO, addr,
						&wordbuffer, 1);
				if (reg) {
					assert(
							reg
									== unibuscontroller->register_by_unibus_address(
											unibus->dma_request->unibus_end_addr));
					printf("DEPOSIT reg #%d \"%s\" %06o <- %06o\n", reg->index, reg->name,
							reg->addr, wordbuffer);
				} else
					printf("DEPOSIT %06o <- %06o\n", addr, wordbuffer);
				if (timeout)
					printf("Bus timeout at %06o.\n", mailbox->dma.cur_addr);
			} else if (!strcasecmp(s_opcode, "e") && n_fields <= 2) {
				bool timeout = false;
				uint32_t addr;
				unibusdevice_register_t *reg = NULL;
				if (n_fields == 2) { // single reg number or address given
					uint16_t wordbuffer; // exam single word
					if (unibuscontroller)
						reg = unibuscontroller->register_by_name(s_param[0]);
					if (reg)
						addr = reg->addr;
					else
						parse_addr18(s_param[0], &addr); // interpret as 18 bit address
					timeout = !unibus->dma(true, UNIBUS_CONTROL_DATI, addr,
							&wordbuffer, 1);
					printf("EXAM %06o -> %06o\n", addr, wordbuffer);
				} else if (n_fields == 1 && unibuscontroller) { // list all regs
					unsigned wordcount = 0; // default: no EXAM
					uint16_t wordbuffer[MAX_IOPAGE_REGISTERS_PER_DEVICE];
					addr = unibuscontroller->base_addr.value; // all device registers
					wordcount = unibuscontroller->register_count;
					if (wordcount) {
						unsigned i;
						timeout = !unibus->dma(true, UNIBUS_CONTROL_DATI,
								addr, wordbuffer, wordcount);
						for (i = 0; addr <= mailbox->dma.cur_addr; i++, addr += 2) {
							reg = unibuscontroller->register_by_unibus_address(addr);
							assert(reg);
							printf("EXAM reg #%d %s %06o -> %06o\n", reg->index, reg->name,
									reg->addr, wordbuffer[i]);
						}
					} else {
						timeout = false;
						printf("Device has no UNIBUS registers.\n");
					}
				} else {
					// no device: no "display all"
					show_help = true;
				}
				if (timeout)
					printf("Bus timeout at %06o.\n", mailbox->dma.cur_addr);
				// cur_addr now on last address in block
			} else if (DL11->enabled.value && !strcasecmp(s_opcode, "dl11")) {
				if ((n_fields == 3 || n_fields == 4) && !strcasecmp(s_param[0], "rcv")) {
					// dl11 rcv [<wait_ms>] <string>
					char buff[256];
					unsigned wait_ms;
					timeout_c timeout;
					char *s;
					if (n_fields == 3) {
						wait_ms = 0;
						s = s_param[1];
					} else {
						wait_ms = strtol(s_param[1], NULL, 10);
						s = s_param[2];
					}
					if (!str_decode_escapes(buff, sizeof(buff), s)) {
						printf("Error in escape sequences.\n");
						inputline.init();
						continue;
					}
					timeout.wait_ms(wait_ms);
					// let DL11 produce chars in 'buff'
					pthread_mutex_lock(&DL11->rs232adapter.mutex);
					dl11_rcv_stream.clear();
					dl11_rcv_stream.write(buff, strlen(buff)); // add endlessly to string
					// dl11_rcv_stream.str(buff);
					pthread_mutex_unlock(&DL11->rs232adapter.mutex);
//							printf("AAA %d\n", (int)dl11_rcv_stream.get()) ;
				} else if (n_fields == 4 && !strcasecmp(s_param[0], "wait")) {
					// dl11 wait <timeout_ms> <string>
					timeout_c timeout, timeout2;
					unsigned ms = strtol(s_param[1], NULL, 10);
					char buff[256];
					if (!str_decode_escapes(buff, sizeof(buff), s_param[2])) {
						printf("Error in escape sequences.\n");
						inputline.init();
						continue;
					}
					// while waiting echo to stdout, for diag
					DL11->rs232adapter.stream_xmt = &cout;
					DL11->rs232adapter.set_pattern(buff);
					timeout.start_ms(ms);
					while (!timeout.reached() && !DL11->rs232adapter.pattern_found)
						timeout2.wait_ms(1);
					DL11->rs232adapter.stream_xmt = NULL; // stop echo

					if (!DL11->rs232adapter.pattern_found) {
						printf(
								"\nPDP-11 did not xmt \"%s\" over DL11 within %u ms, aborting script\n",
								s_param[2], ms);
						inputline.init();
					}
				} else {
					printf("Unknown DL11 command \"%s\"!\n", s_choice);
					show_help = true;
				}
			} else {
				printf("Unknown command \"%s\"!\n", s_choice);
				show_help = true;
			}
		} catch (bad_parameter& e) {
			cout << "Error : " << e.what() << "\n";
		}
	} // ready

	if (with_emulated_CPU) {
		cpu->enabled.set(false);
		delete cpu;
	}

	m9312->enabled.set(false) ;
	delete m9312 ;


	LTC->enabled.set(false);
	delete LTC;
	DL11->enabled.set(false);
	delete DL11;

	RL11->enabled.set(false);
	delete RL11;

	RK11->enabled.set(false);
	delete RK11;

	UDA50->enabled.set(false);
	delete UDA50;

	//test_controller->enabled.set(false);
	//delete test_controller;

	demo_io->enabled.set(false);
	delete demo_io;

	unibusadapter->enabled.set(false);

	buslatches_output_enable(false);
	hardware_shutdown(); // stop PRU
}

