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

#include "lv2/lv2plug.in/ns/ext/persist/persist.h"

#include "jalv_internal.h"

static int
store_callback(void*       host_data,
               uint32_t    key,
               const void* value,
               size_t      size,
               uint32_t    type,
               uint32_t    flags)
{
	Jalv* jalv = (Jalv*)host_data;
	const char* key_uri = symap_unmap(jalv->symap, key);
	if (key_uri) {
		printf("STORE %s\n", key_uri);
	} else {
		fprintf(stderr, "error: Failed to unmap URI ID %u\n", key);
		return 1;
	}
	return 0;
}

static const void*
retrieve_callback(void*     host_data,
                  uint32_t  key,
                  size_t*   size,
                  uint32_t* type,
                  uint32_t* flags)
{
	//Jalv* jalv = (Jalv*)host_data;
	printf("RETRIEVE %d\n", key);
	return 0;
}


void
jalv_save(Jalv* jalv, const char* dir)
{
	printf("SAVE %s\n", dir);
	const LV2_Persist* persist = (const LV2_Persist*)
		lilv_instance_get_extension_data(jalv->instance,
		                                 "http://lv2plug.in/ns/ext/persist");

	if (persist) {
		persist->save(lilv_instance_get_handle(jalv->instance),
		              store_callback,
		              jalv);
	}
}

void
jalv_restore(Jalv* jalv, const char* dir)
{
	printf("RESTORE %s\n", dir);
	const LV2_Persist* persist = (const LV2_Persist*)
		lilv_instance_get_extension_data(jalv->instance,
		                                 "http://lv2plug.in/ns/ext/persist");
	if (persist) {
		persist->restore(lilv_instance_get_handle(jalv->instance),
		                 retrieve_callback,
		                 jalv);
	}
}
