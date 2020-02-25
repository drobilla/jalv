/*
  Copyright 2007-2016 David Robillard <http://drobilla.net>

  Permission to use, copy, modify, and/or distribute this software for any
  purpose with or without fee is hereby granted, provided that the above
  copyright notice and this permission notice appear in all copies.

  THIS SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE   600
#define _BSD_SOURCE     1
#define _DEFAULT_SOURCE 1

#include "jalv_config.h"
#include "jalv_internal.h"

#include "lv2/ui/ui.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int
print_usage(const char* name, bool error)
{
	FILE* const os = error ? stderr : stdout;
	fprintf(os, "Usage: %s [OPTION...] PLUGIN_URI\n", name);
	fprintf(os, "Run an LV2 plugin as a Jack application.\n");
	fprintf(os, "  -b SIZE      Buffer size for plugin <=> UI communication\n");
	fprintf(os, "  -c SYM=VAL   Set control value (e.g. \"vol=1.4\")\n");
	fprintf(os, "  -d           Dump plugin <=> UI communication\n");
	fprintf(os, "  -h           Display this help and exit\n");
	fprintf(os, "  -l DIR       Load state from save directory\n");
	fprintf(os, "  -n NAME      JACK client name\n");
	fprintf(os, "  -p           Print control output changes to stdout\n");
	fprintf(os, "  -s           Show plugin UI if possible\n");
	fprintf(os, "  -t           Print trace messages from plugin\n");
	fprintf(os, "  -u UUID      UUID for Jack session restoration\n");
	fprintf(os, "  -x           Exact JACK client name (exit if taken)\n");
	return error ? 1 : 0;
}

void
jalv_ui_port_event(ZIX_UNUSED Jalv*       jalv,
                   ZIX_UNUSED uint32_t    port_index,
                   ZIX_UNUSED uint32_t    buffer_size,
                   ZIX_UNUSED uint32_t    protocol,
                   ZIX_UNUSED const void* buffer)
{
}

int
jalv_init(int* argc, char*** argv, JalvOptions* opts)
{
	int n_controls = 0;
	int a          = 1;
	for (; a < *argc && (*argv)[a][0] == '-'; ++a) {
		if ((*argv)[a][1] == 'h') {
			return print_usage((*argv)[0], true);
		} else if ((*argv)[a][1] == 's') {
			opts->show_ui = true;
		} else if ((*argv)[a][1] == 'p') {
			opts->print_controls = true;
		} else if ((*argv)[a][1] == 'u') {
			if (++a == *argc) {
				fprintf(stderr, "Missing argument for -u\n");
				return 1;
			}
			opts->uuid = jalv_strdup((*argv)[a]);
		} else if ((*argv)[a][1] == 'l') {
			if (++a == *argc) {
				fprintf(stderr, "Missing argument for -l\n");
				return 1;
			}
			opts->load = jalv_strdup((*argv)[a]);
		} else if ((*argv)[a][1] == 'b') {
			if (++a == *argc) {
				fprintf(stderr, "Missing argument for -b\n");
				return 1;
			}
			opts->buffer_size = atoi((*argv)[a]);
		} else if ((*argv)[a][1] == 'c') {
			if (++a == *argc) {
				fprintf(stderr, "Missing argument for -c\n");
				return 1;
			}
			opts->controls = (char**)realloc(
				opts->controls, (++n_controls + 1) * sizeof(char*));
			opts->controls[n_controls - 1] = (*argv)[a];
			opts->controls[n_controls]     = NULL;
		} else if ((*argv)[a][1] == 'i') {
			opts->non_interactive = true;
		} else if ((*argv)[a][1] == 'd') {
			opts->dump = true;
		} else if ((*argv)[a][1] == 't') {
			opts->trace = true;
		} else if ((*argv)[a][1] == 'n') {
			if (++a == *argc) {
				fprintf(stderr, "Missing argument for -n\n");
				return 1;
			}
			free(opts->name);
			opts->name = jalv_strdup((*argv)[a]);
		} else if ((*argv)[a][1] == 'x') {
			opts->name_exact = 1;
		} else {
			fprintf(stderr, "Unknown option %s\n", (*argv)[a]);
			return print_usage((*argv)[0], true);
		}
	}

	return 0;
}

const char*
jalv_native_ui_type(void)
{
	return NULL;
}

static void
jalv_print_controls(Jalv* jalv, bool writable, bool readable)
{
	for (size_t i = 0; i < jalv->controls.n_controls; ++i) {
		ControlID* const control = jalv->controls.controls[i];
		if ((control->is_writable && writable) ||
		    (control->is_readable && readable)) {
			struct Port* const port = &jalv->ports[control->index];
			printf("%s = %f\n",
			       lilv_node_as_string(control->symbol),
			       port->control);
		}
	}
}

static int
jalv_print_preset(Jalv*           jalv,
                  const LilvNode* node,
                  const LilvNode* title,
                  void*           data)
{
	printf("%s (%s)\n", lilv_node_as_string(node), lilv_node_as_string(title));
	return 0;
}

static void
jalv_process_command(Jalv* jalv, const char* cmd)
{
	char     sym[64];
	uint32_t index;
	float    value;
	if (!strncmp(cmd, "help", 4)) {
		fprintf(stderr,
		        "Commands:\n"
		        "  help              Display this help message\n"
		        "  controls          Print settable control values\n"
		        "  monitors          Print output control values\n"
		        "  presets           Print available presets\n"
		        "  preset URI        Set preset\n"
		        "  set INDEX VALUE   Set control value by port index\n"
		        "  set SYMBOL VALUE  Set control value by symbol\n"
		        "  SYMBOL = VALUE    Set control value by symbol\n");
	} else if (strcmp(cmd, "presets\n") == 0) {
		jalv_unload_presets(jalv);
		jalv_load_presets(jalv, jalv_print_preset, NULL);
	} else if (sscanf(cmd, "preset %[a-zA-Z0-9_:/-.#]\n", sym) == 1) {
		LilvNode* preset = lilv_new_uri(jalv->world, sym);
		jalv_apply_preset(jalv, preset);
		lilv_node_free(preset);
		jalv_print_controls(jalv, true, false);
	} else if (strcmp(cmd, "controls\n") == 0) {
		jalv_print_controls(jalv, true, false);
	} else if (strcmp(cmd, "monitors\n") == 0) {
		jalv_print_controls(jalv, false, true);
	} else if (sscanf(cmd, "set %u %f", &index, &value) == 2) {
		if (index < jalv->num_ports) {
			jalv->ports[index].control = value;
			jalv_print_control(jalv, &jalv->ports[index], value);
		} else {
			fprintf(stderr, "error: port index out of range\n");
		}
	} else if (sscanf(cmd, "set %[a-zA-Z0-9_] %f", sym, &value) == 2 ||
	           sscanf(cmd, "%[a-zA-Z0-9_] = %f", sym, &value) == 2) {
		struct Port* port = NULL;
		for (uint32_t i = 0; i < jalv->num_ports; ++i) {
			struct Port* p = &jalv->ports[i];
			const LilvNode* s = lilv_port_get_symbol(jalv->plugin, p->lilv_port);
			if (!strcmp(lilv_node_as_string(s), sym)) {
				port = p;
				break;
			}
		}
		if (port) {
			port->control = value;
			jalv_print_control(jalv, port, value);
		} else {
			fprintf(stderr, "error: no control named `%s'\n", sym);
		}
	} else {
		fprintf(stderr, "error: invalid command (try `help')\n");
	}
}

bool
jalv_discover_ui(Jalv* jalv)
{
	return jalv->opts.show_ui;
}

static bool
jalv_run_custom_ui(Jalv* jalv)
{
#ifdef HAVE_SUIL
	const LV2UI_Idle_Interface* idle_iface = NULL;
	const LV2UI_Show_Interface* show_iface = NULL;
	if (jalv->ui && jalv->opts.show_ui) {
		jalv_ui_instantiate(jalv, jalv_native_ui_type(), NULL);
		idle_iface = (const LV2UI_Idle_Interface*)
			suil_instance_extension_data(jalv->ui_instance, LV2_UI__idleInterface);
		show_iface = (const LV2UI_Show_Interface*)
			suil_instance_extension_data(jalv->ui_instance, LV2_UI__showInterface);
	}

	if (show_iface && idle_iface) {
		show_iface->show(suil_instance_get_handle(jalv->ui_instance));

		// Drive idle interface until interrupted
		while (!zix_sem_try_wait(&jalv->done)) {
			jalv_update(jalv);
			if (idle_iface->idle(suil_instance_get_handle(jalv->ui_instance))) {
				break;
			}
			usleep(33333);
		}

		show_iface->hide(suil_instance_get_handle(jalv->ui_instance));
		return true;
	}
#endif

	return false;
}

float
jalv_ui_refresh_rate(Jalv* jalv)
{
	return 30.0f;
}

int
jalv_open_ui(Jalv* jalv)
{
	if (!jalv_run_custom_ui(jalv) && !jalv->opts.non_interactive) {
		// Primitive command prompt for setting control values
		while (!zix_sem_try_wait(&jalv->done)) {
			char line[128];
			printf("> ");
			if (fgets(line, sizeof(line), stdin)) {
				jalv_process_command(jalv, line);
			} else {
				break;
			}
		}
	} else {
		zix_sem_wait(&jalv->done);
	}

	// Caller waits on the done sem, so increment it again to exit
	zix_sem_post(&jalv->done);

	return 0;
}

int
jalv_close_ui(Jalv* jalv)
{
	zix_sem_post(&jalv->done);
	return 0;
}
