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
#define _BSD_SOURCE

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "jalv_config.h"
#include "jalv_internal.h"

#include "lv2/lv2plug.in/ns/extensions/ui/ui.h"

static int
print_usage(const char* name, bool error)
{
	FILE* const os = error ? stderr : stdout;
	fprintf(os, "Usage: %s [OPTION...] PLUGIN_URI\n", name);
	fprintf(os, "Run an LV2 plugin as a Jack application.\n");
	fprintf(os, "  -h           Display this help and exit\n");
	fprintf(os, "  -p           Print control output changes to stdout\n");
	fprintf(os, "  -c SYM=VAL   Set control value (e.g. \"vol=1.4\")\n");
	fprintf(os, "  -u UUID      UUID for Jack session restoration\n");
	fprintf(os, "  -l DIR       Load state from save directory\n");
	fprintf(os, "  -d           Dump plugin <=> UI communication\n");
	fprintf(os, "  -s           Show plugin UI if possible\n");
	fprintf(os, "  -t           Print trace messages from plugin\n");
	fprintf(os, "  -b SIZE      Buffer size for plugin <=> UI communication\n");
	fprintf(os, "  -n NAME      JACK client name\n");
	fprintf(os, "  -x           Exact JACK client name (exit if taken)\n");
	return error ? 1 : 0;
}

int
jalv_ui_resize(Jalv* jalv, int width, int height)
{
	return 0;
}

void
jalv_ui_port_event(Jalv*       jalv,
                   uint32_t    port_index,
                   uint32_t    buffer_size,
                   uint32_t    protocol,
                   const void* buffer)
{
}

int
jalv_init(int* argc, char*** argv, JalvOptions* opts)
{
	opts->controls    = (char**)malloc(sizeof(char*));
	opts->controls[0] = NULL;

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
jalv_native_ui_type(Jalv* jalv)
{
	return NULL;
}

static void
jalv_process_command(Jalv* jalv, const char* cmd)
{
	char  sym[64];
	float value;
	if (sscanf(cmd, "%[a-zA-Z0-9] = %f", sym, &value) == 2) {
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
			printf("%-*s = %f\n", jalv->longest_sym, sym, value);
		} else {
			fprintf(stderr, "error: no port `%s'\n", sym);
		}
	}
}

int
jalv_open_ui(Jalv* jalv)
{
	jalv->has_ui = jalv->opts.show_ui;

	const LV2UI_Idle_Interface* idle_iface = NULL;
	const LV2UI_Show_Interface* show_iface = NULL;
	if (jalv->ui && jalv->opts.show_ui) {
		jalv_ui_instantiate(jalv, jalv_native_ui_type(jalv), NULL);
		idle_iface = (const LV2UI_Idle_Interface*)
			suil_instance_extension_data(jalv->ui_instance, LV2_UI__idleInterface);
		show_iface = (LV2UI_Show_Interface*)
			suil_instance_extension_data(jalv->ui_instance, LV2_UI__showInterface);
	}

	if (show_iface && idle_iface) {
		show_iface->show(suil_instance_get_handle(jalv->ui_instance));

		// Drive idle interface until interrupted
		while (!zix_sem_try_wait(jalv->done)) {
			jalv_update(jalv);
			if (idle_iface->idle(suil_instance_get_handle(jalv->ui_instance))) {
				break;
			}
			usleep(33333);
		}

		show_iface->hide(suil_instance_get_handle(jalv->ui_instance));

	} else {
		// Primitive command prompt for setting control values
		while (!zix_sem_try_wait(jalv->done)) {
			char line[128];
			printf("> ");
			if (fgets(line, sizeof(line), stdin)) {
				jalv_process_command(jalv, line);
			} else {
				break;
			}
		}
	}

	// Caller waits on the done sem, so increment it again to exit
	zix_sem_post(jalv->done);

	return 0;
}

int
jalv_close_ui(Jalv* jalv)
{
	zix_sem_post(jalv->done);
	return 0;
}
