/*
  Copyright 2007-2016 David Robillard <d@drobilla.net>

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

#include "jalv_internal.h"

#include "lilv/lilv.h"
#include "lv2/atom/forge.h"
#include "lv2/core/lv2.h"
#include "lv2/state/state.h"
#include "lv2/urid/urid.h"
#include "lv2/presets/presets.h"
#include "zix/common.h"
#include "zix/ring.h"
#include "zix/sem.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char*
jalv_make_path(LV2_State_Make_Path_Handle handle,
               const char*                path)
{
	Jalv* jalv = (Jalv*)handle;

	// Create in save directory if saving, otherwise use temp directory
	return jalv_strjoin(jalv->save_dir ? jalv->save_dir : jalv->temp_dir, path);
}

static const void*
get_port_value(const char* port_symbol,
               void*       user_data,
               uint32_t*   size,
               uint32_t*   type)
{
	Jalv*        jalv = (Jalv*)user_data;
	struct Port* port = jalv_port_by_symbol(jalv, port_symbol);
	if (port && port->flow == FLOW_INPUT && port->type == TYPE_CONTROL) {
		*size = sizeof(float);
		*type = jalv->forge.Float;
		return &port->control;
	}
	*size = *type = 0;
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

	lilv_state_save(jalv->world, &jalv->map, &jalv->unmap, state, NULL,
	                dir, "state.ttl");

	lilv_state_free(state);

	free(jalv->save_dir);
	jalv->save_dir = NULL;
}

int
jalv_load_presets(Jalv* jalv, PresetSink sink, void* data)
{
	LilvNodes* presets = lilv_plugin_get_related(jalv->plugin,
	                                             jalv->nodes.pset_Preset);
	LILV_FOREACH(nodes, i, presets) {
		const LilvNode* preset = lilv_nodes_get(presets, i);
		lilv_world_load_resource(jalv->world, preset);
		if (!sink) {
			continue;
		}

		LilvNodes* labels = lilv_world_find_nodes(
			jalv->world, preset, jalv->nodes.rdfs_label, NULL);
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

int
jalv_unload_presets(Jalv* jalv)
{
	LilvNodes* presets = lilv_plugin_get_related(jalv->plugin,
	                                             jalv->nodes.pset_Preset);
	LILV_FOREACH(nodes, i, presets) {
		const LilvNode* preset = lilv_nodes_get(presets, i);
		lilv_world_unload_resource(jalv->world, preset);
	}
	lilv_nodes_free(presets);

	return 0;
}

static void
set_port_value(const char* port_symbol,
               void*       user_data,
               const void* value,
               uint32_t    ZIX_UNUSED(size),
               uint32_t    type)
{
	Jalv*        jalv = (Jalv*)user_data;
	struct Port* port = jalv_port_by_symbol(jalv, port_symbol);
	if (!port) {
		fprintf(stderr, "error: Preset port `%s' is missing\n", port_symbol);
		return;
	}

	float fvalue = 0.0f;
	if (type == jalv->forge.Float) {
		fvalue = *(const float*)value;
	} else if (type == jalv->forge.Double) {
		fvalue = *(const double*)value;
	} else if (type == jalv->forge.Int) {
		fvalue = *(const int32_t*)value;
	} else if (type == jalv->forge.Long) {
		fvalue = *(const int64_t*)value;
	} else {
		fprintf(stderr, "error: Preset `%s' value has bad type <%s>\n",
		        port_symbol, jalv->unmap.unmap(jalv->unmap.handle, type));
		return;
	}

	if (jalv->play_state != JALV_RUNNING) {
		// Set value on port struct directly
		port->control = fvalue;
	} else {
		// Send value to running plugin
		jalv_ui_write(jalv, port->index, sizeof(fvalue), 0, &fvalue);
	}

	if (jalv->has_ui) {
		// Update UI
		char buf[sizeof(ControlChange) + sizeof(fvalue)];
		ControlChange* ev = (ControlChange*)buf;
		ev->index    = port->index;
		ev->protocol = 0;
		ev->size     = sizeof(fvalue);
		*(float*)ev->body = fvalue;
		zix_ring_write(jalv->plugin_events, buf, sizeof(buf));
	}
}

void
jalv_apply_state(Jalv* jalv, LilvState* state)
{
	bool must_pause = !jalv->safe_restore && jalv->play_state == JALV_RUNNING;
	if (state) {
		if (must_pause) {
			jalv->play_state = JALV_PAUSE_REQUESTED;
			zix_sem_wait(&jalv->paused);
		}

		const LV2_Feature* state_features[9] = {
			&jalv->features.map_feature,
			&jalv->features.unmap_feature,
			&jalv->features.make_path_feature,
			&jalv->features.state_sched_feature,
			&jalv->features.safe_restore_feature,
			&jalv->features.log_feature,
			&jalv->features.options_feature,
			NULL
		};

		lilv_state_restore(
			state, jalv->instance, set_port_value, jalv, 0, state_features);

		if (must_pause) {
			jalv->request_update = true;
			jalv->play_state     = JALV_RUNNING;
		}
	}
}

int
jalv_apply_preset(Jalv* jalv, const LilvNode* preset)
{
	lilv_state_free(jalv->preset);
	jalv->preset = lilv_state_new_from_world(jalv->world, &jalv->map, preset);
	jalv_apply_state(jalv, jalv->preset);
	return 0;
}

int
jalv_save_preset(Jalv*       jalv,
                 const char* dir,
                 const char* uri,
                 const char* label,
                 const char* filename)
{
	if(!label)
		return 1;

	const char* preset_name = NULL;
	char bank_name[1024];
	char full_dir[1024];
	char full_filename[1024];
	memset(bank_name, 0, sizeof(bank_name));

	for(int i = 0; i < strlen(label); ++i) {
		if(label[i] == '/') {
			preset_name = label + i + 1;
			break;
		}
		bank_name[i] = label[i];
	}

	if(!preset_name) {
		preset_name = label;
		bank_name[0] = '\0';
	}

	char* plugin_name = NULL;
	LilvNode* name = lilv_plugin_get_name(jalv->plugin);
	plugin_name = jalv_strdup(lilv_node_as_string(name));
	lilv_node_free(name);
	if(dir)
		if(strlen(bank_name))
			sprintf(full_dir, "%s/%s-%s", dir, plugin_name, bank_name);
		else
			sprintf(full_dir, "%s", dir);
	else
		if(strlen(bank_name))
			sprintf(full_dir, "./%s-%s", plugin_name, bank_name);
		else
			sprintf(full_dir, ".");

	if(filename)
		sprintf(full_filename, "%s", filename);
	else
		sprintf(full_filename, "%s.ttl", preset_name);

	LilvState* const state = lilv_state_new_from_instance(
		jalv->plugin, jalv->instance, &jalv->map,
		jalv->temp_dir, full_dir, full_dir, full_dir,
		get_port_value, jalv,
		LV2_STATE_IS_POD|LV2_STATE_IS_PORTABLE, NULL);

	lilv_state_set_label(state, preset_name);

	if(strlen(bank_name)) {
//		const LV2_URID atom_urid = jalv->map.map(jalv, LV2_ATOM__URID);
		const LV2_URID bank_urid = jalv->map.map(jalv, bank_name);
		const LV2_URID bank_key_urid = jalv->map.map(jalv, LV2_PRESETS__bank);
		lilv_state_set_metadata(state, bank_key_urid, &bank_urid, sizeof(bank_urid), atom_urid, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
		//!@todo Get the bank URI - we need to create the bank if it does not exist
	}

	int ret = lilv_state_save(
		jalv->world, &jalv->map, &jalv->unmap, state, uri, full_dir, full_filename);

	lilv_state_free(jalv->preset);
	jalv->preset = state;

	return ret;
}

int
jalv_delete_current_preset(Jalv* jalv)
{
	if (!jalv->preset) {
		return 1;
	}

	lilv_world_unload_resource(jalv->world, lilv_state_get_uri(jalv->preset));
	lilv_state_delete(jalv->world, jalv->preset);
	lilv_state_free(jalv->preset);
	jalv->preset = NULL;
	return 0;
}
