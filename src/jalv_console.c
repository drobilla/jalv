/*
  Copyright 2007-2012 David Robillard <http://drobilla.net>

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

#include <string.h>
#include <stdbool.h>
#include <stdio.h>

#include "jalv_config.h"
#include "jalv_internal.h"

static int
print_usage(const char* name, bool error)
{
	FILE* const os = error ? stderr : stdout;
	fprintf(os, "Usage: %s [OPTION...] PLUGIN_URI\n", name);
	fprintf(os, "Run an LV2 plugin as a Jack application.\n");
	fprintf(os, "  -h           Display this help and exit\n");
	fprintf(os, "  -u UUID      UUID for Jack session restoration\n");
	fprintf(os, "  -l DIR       Load state from save directory\n");
	fprintf(os, "  -d DIR       Dump plugin <=> UI communication\n");
	fprintf(os, "  -b SIZE      Buffer size for plugin <=> UI communication\n");
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
	opts->controls    = malloc(sizeof(char*));
	opts->controls[0] = NULL;

	int n_controls = 0;
	int a          = 1;
	for (; a < *argc && (*argv)[a][0] == '-'; ++a) {
		if ((*argv)[a][1] == 'h') {
			return print_usage((*argv)[0], true);
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
			opts->controls = realloc(opts->controls,
			                         (++n_controls + 1) * sizeof(char*));
			opts->controls[n_controls - 1] = (*argv)[a];
			opts->controls[n_controls]     = NULL;
		} else if ((*argv)[a][1] == 'd') {
			opts->dump = true;
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

int
jalv_open_ui(Jalv* jalv)
{
#ifdef JALV_JACK_SESSION
	printf("\nPress Ctrl-C to quit: ");
	fflush(stdout);
#else
	printf("\nPress enter to quit: ");
	fflush(stdout);
	getc(stdin);
	zix_sem_post(jalv->done);
#endif
	printf("\n");

	return 0;
}

int
jalv_close_ui(Jalv* jalv)
{
	zix_sem_post(jalv->done);
	return 0;
}
