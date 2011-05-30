/*
  Copyright 2007-2011 David Robillard <http://drobilla.net>

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

#include <stdio.h>

#include "jalv-config.h"
#include "jalv_internal.h"

void
jalv_init(int* argc, char*** argv)
{
}

LilvNode*
jalv_native_ui_type(Jalv* jalv)
{
	return NULL;
}

int
jalv_open_ui(Jalv*         jalv,
             SuilInstance* instance)
{
#ifdef JALV_JACK_SESSION
	printf("\nPress Ctrl-C to quit: ");
	fflush(stdout);
	//g_cond_wait(exit_cond, exit_mutex);
#else
	printf("\nPress enter to quit: ");
	fflush(stdout);
	getc(stdin);
	sem_post(jalv->done);
#endif
	printf("\n");

	return 0;
}
