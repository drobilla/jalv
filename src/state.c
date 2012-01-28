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

#define _POSIX_C_SOURCE 200112L /* for fileno */
#define _BSD_SOURCE 1 /* for lockf */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef HAVE_LV2_STATE
#    include "lv2/lv2plug.in/ns/ext/state/state.h"
#endif

#include "lilv/lilv.h"

#include "jalv_config.h"
#include "jalv_internal.h"

#define NS_ATOM  "http://lv2plug.in/ns/ext/atom#"
#define NS_JALV  "http://drobilla.net/ns/jalv#"
#define NS_LV2   "http://lv2plug.in/ns/lv2core#"
#define NS_PSET  "http://lv2plug.in/ns/ext/presets#"
#define NS_RDF   "http://www.w3.org/1999/02/22-rdf-syntax-ns#"
#define NS_RDFS  "http://www.w3.org/2000/01/rdf-schema#"
#define NS_STATE "http://lv2plug.in/ns/ext/state#"
#define NS_XSD   "http://www.w3.org/2001/XMLSchema#"

#define USTR(s) ((const uint8_t*)s)

char*
jalv_make_path(LV2_State_Make_Path_Handle handle,
               const char*                path)
{
	Jalv* jalv = (Jalv*)handle;

	// Create in save directory if saving, otherwise use temp directory
	const char* dir = (jalv->save_dir) ? jalv->save_dir : jalv->temp_dir;

	char* fullpath = jalv_strjoin(dir, path);
	fprintf(stderr, "MAKE PATH `%s' => `%s'\n", path, fullpath);

	return fullpath;
}

LilvNode*
get_port_value(const char* port_symbol,
               void*       user_data)
{
	Jalv*        jalv = (Jalv*)user_data;
	struct Port* port = jalv_port_by_symbol(jalv, port_symbol);
	if (port && port->flow == FLOW_INPUT && port->type == TYPE_CONTROL) {
		return lilv_new_float(jalv->world, port->control);
	}
	return NULL;
}

void
jalv_save(Jalv* jalv, const char* dir)
{
	jalv->save_dir = jalv_strjoin(dir, "/");

	LilvState* const state = lilv_state_new_from_instance(
		jalv->plugin, jalv->instance, &jalv->map,
		jalv->temp_dir, dir, dir, dir,
		get_port_value, jalv,
		LV2_STATE_IS_POD|LV2_STATE_IS_PORTABLE, NULL);

	lilv_state_save(jalv->world, &jalv->unmap, state, NULL,
	                dir, "state.ttl", NULL);

	lilv_state_free(state);

	free(jalv->save_dir);
	jalv->save_dir = NULL;
}

int
jalv_load_presets(Jalv* jalv, PresetSink sink, void* data)
{
	LilvNodes* presets = lilv_plugin_get_related(jalv->plugin,
	                                             jalv->preset_class);
	LILV_FOREACH(nodes, i, presets) {
		const LilvNode* preset = lilv_nodes_get(presets, i);
		lilv_world_load_resource(jalv->world, preset);
		LilvNodes* labels = lilv_world_find_nodes(
			jalv->world, preset, jalv->label_pred, NULL);
		if (labels) {
			const LilvNode* label = lilv_nodes_get_first(labels);
			sink(jalv, preset, label, data);
			lilv_nodes_free(labels);
		} else {
			fprintf(stderr, "Preset <%s> has no rdfs:label\n",
			        lilv_node_as_string(lilv_nodes_get(presets, i)));
		}
	}
	lilv_nodes_free(presets);

	return 0;
}

static void
set_port_value(const char*     port_symbol,
               const LilvNode* value,
               void*           user_data)
{
	Jalv*        jalv = (Jalv*)user_data;
	struct Port* port = jalv_port_by_symbol(jalv, port_symbol);
	if (!port) {
		fprintf(stderr, "error: Preset port `%s' is missing\n", port_symbol);
		return;
	}

	if (!lilv_node_is_float(value) && !lilv_node_is_int(value)) {
		fprintf(stderr, "error: Preset port `%s' value is not a number\n",
		        port_symbol);
		return;
	}

	const float fvalue = lilv_node_as_float(value);

	// Send value to plugin
	jalv_ui_write(jalv, port->index, sizeof(fvalue), 0, &fvalue);

	// Update UI
	if (jalv->ui) {
		char buf[sizeof(ControlChange) + sizeof(fvalue)];
		ControlChange* ev = (ControlChange*)buf;
		ev->index    = port->index;
		ev->protocol = 0;
		ev->size     = sizeof(fvalue);
		*(float*)ev->body = fvalue;
		jack_ringbuffer_write(jalv->plugin_events, buf, sizeof(buf));
	}
}

void
jalv_apply_state(Jalv* jalv, LilvState* state)
{
	if (state) {
		const bool must_pause = (jalv->play_state == JALV_RUNNING);
		if (must_pause) {
			jalv->play_state = JALV_PAUSE_REQUESTED;
			sem_wait(&jalv->paused);
		}

		lilv_state_restore(
			state, jalv->instance, set_port_value, jalv, 0, NULL);

		if (must_pause) {
			jalv->play_state = JALV_RUNNING;
		}
	}
}

int
jalv_apply_preset(Jalv* jalv, const LilvNode* preset)
{
	LilvState* state = lilv_state_new_from_world(
		jalv->world, &jalv->map, preset);
	jalv_apply_state(jalv, state);
	lilv_state_free(state);
	return 0;
}

int
jalv_save_preset(Jalv* jalv, const char* dir, const char* uri, const char* label)
{
	LilvState* const state = lilv_state_new_from_instance(
		jalv->plugin, jalv->instance, &jalv->map,
		jalv->temp_dir, dir, dir, dir,
		get_port_value, jalv,
		LV2_STATE_IS_POD|LV2_STATE_IS_PORTABLE, NULL);

	if (label) {
		lilv_state_set_label(state, label);
	}

	int ret = lilv_state_save(jalv->world, &jalv->unmap, state,
	                          uri, dir, "state.ttl", NULL);
	lilv_state_free(state);

	return ret;
}
