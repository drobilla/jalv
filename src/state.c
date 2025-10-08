// Copyright 2007-2016 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "state.h"

#include "jalv_internal.h"
#include "log.h"
#include "nodes.h"
#include "port.h"

#include "lilv/lilv.h"
#include "lv2/atom/forge.h"
#include "lv2/core/lv2.h"
#include "lv2/state/state.h"
#include "lv2/urid/urid.h"
#include "lv2/presets/presets.h"
#include "zix/attributes.h"
#include "zix/sem.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char*
jalv_make_path(LV2_State_Make_Path_Handle handle, const char* path)
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

  LilvState* const state =
    lilv_state_new_from_instance(jalv->plugin,
                                 jalv->instance,
                                 &jalv->map,
                                 jalv->temp_dir,
                                 dir,
                                 dir,
                                 dir,
                                 get_port_value,
                                 jalv,
                                 LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE,
                                 NULL);

  lilv_state_save(
    jalv->world, &jalv->map, &jalv->unmap, state, NULL, dir, "state.ttl");

  lilv_state_free(state);

  free(jalv->save_dir);
  jalv->save_dir = NULL;
}

int
jalv_load_presets(Jalv* jalv, PresetSink sink, void* data)
{
  LilvNodes* presets =
    lilv_plugin_get_related(jalv->plugin, jalv->nodes.pset_Preset);
  LILV_FOREACH (nodes, i, presets) {
    const LilvNode* preset = lilv_nodes_get(presets, i);
    lilv_world_load_resource(jalv->world, preset);
    if (!sink) {
      continue;
    }

    LilvNodes* labels =
      lilv_world_find_nodes(jalv->world, preset, jalv->nodes.rdfs_label, NULL);
    if (labels) {
      const LilvNode* label = lilv_nodes_get_first(labels);
      sink(jalv, preset, label, data);
      lilv_nodes_free(labels);
    } else {
      jalv_log(JALV_LOG_WARNING,
               "Preset <%s> has no rdfs:label\n",
               lilv_node_as_string(lilv_nodes_get(presets, i)));
    }
  }
  lilv_nodes_free(presets);

  return 0;
}

int
jalv_unload_presets(Jalv* jalv)
{
  LilvNodes* presets =
    lilv_plugin_get_related(jalv->plugin, jalv->nodes.pset_Preset);
  LILV_FOREACH (nodes, i, presets) {
    const LilvNode* preset = lilv_nodes_get(presets, i);
    lilv_world_unload_resource(jalv->world, preset);
  }
  lilv_nodes_free(presets);

  return 0;
}

static void
set_port_fvalue(Jalv *jalv,
                 struct Port *port,
                 const ControlID *control,
                 float fvalue)
{
  if (jalv->play_state != JALV_RUNNING) {
    // Set value on port struct directly
    port->control = fvalue;
  } else {
    // Send value to plugin (as if from UI)
    jalv_write_control(jalv, jalv->ui_to_plugin, port->index, fvalue);
  }

  if (jalv->has_ui) {
    // Update UI (as if from plugin)
    jalv_write_control(jalv, jalv->plugin_to_ui, port->index, fvalue);
  }

  // Print control value to console
  jalv_print_control(jalv, control, fvalue);
}

static void
set_port_value(const char* port_symbol,
               void*       user_data,
               const void* value,
               uint32_t    ZIX_UNUSED(size),
               uint32_t    type)
{
  Jalv*        jalv = (Jalv*)user_data;
  //struct Port* port = jalv_port_by_symbol(jalv, port_symbol);
  //if (!port) {
  const ControlID* control = jalv_control_by_symbol(jalv, port_symbol);
  if (!control) {
    jalv_log(JALV_LOG_ERR, "Preset control `%s': doesn't exist!\n", port_symbol);
    return;
  }
  struct Port* port = &jalv->ports[control->index];

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
    jalv_log(JALV_LOG_ERR, "Preset control `%s': bad type <%s>!\n",
             port_symbol,
             jalv->unmap.unmap(jalv->unmap.handle, type));
    return;
  }


  set_port_fvalue(jalv, port, control, fvalue);

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
      NULL};

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
  LilvState* const state =
    lilv_state_new_from_instance(jalv->plugin,
                                 jalv->instance,
                                 &jalv->map,
                                 jalv->temp_dir,
                                 dir,
                                 dir,
                                 dir,
                                 get_port_value,
                                 jalv,
                                 LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE,
                                 NULL);

  if (label) {
    lilv_state_set_label(state, label);
  }

  int ret = lilv_state_save(
    jalv->world, &jalv->map, &jalv->unmap, state, uri, dir, filename);

  lilv_state_free(jalv->preset);
  jalv->preset = state;

  return ret;
}

int
jalv_save_bank_preset(Jalv*  jalv,
                 const char* dir,
                 const char* uri_bank,
                 const char* uri_preset,
                 const char* label_preset,
                 const char* filename)
{
	LilvState* const state = lilv_state_new_from_instance(
		jalv->plugin, jalv->instance, &jalv->map,
		jalv->temp_dir, dir, dir, dir,
		get_port_value, jalv,
		LV2_STATE_IS_POD|LV2_STATE_IS_PORTABLE, NULL);

	if (label_preset) {
		lilv_state_set_label(state, label_preset);
	}

	if (uri_bank) {
		const LV2_URID bank_key_urid = jalv->map.map(jalv, LV2_PRESETS__bank);
		const LV2_URID bank_urid = jalv->map.map(jalv, uri_bank);
		const LV2_URID atom_urid = jalv->map.map(jalv, LV2_ATOM__URID);
		lilv_state_set_metadata(state, bank_key_urid, &bank_urid, sizeof(bank_urid), atom_urid, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
	}

	int ret = lilv_state_save(
		jalv->world, &jalv->map, &jalv->unmap, state, uri_preset, dir, filename);

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

void
jalv_command_save_preset(Jalv* jalv, char* sym)
{
	char dir_preset[256];
	char fname_preset[256];
	char uri_preset[540];
	char *plugin_name = jalv_get_plugin_name(jalv);
	char *name_preset = sym;
	char *bank_uri = strtok_r(sym, ",", &name_preset);
	char *label_preset = strtok_r(NULL, ",", &name_preset);
	if (!label_preset) {
		label_preset = bank_uri;
		bank_uri = NULL;
	}
	jalv_fix_filename(plugin_name);
	sprintf(dir_preset, "%s/%s.presets.lv2/", jalv->opts.preset_path, plugin_name);
	sprintf(fname_preset, "%s.ttl", label_preset);
	jalv_fix_filename(fname_preset);
	if (bank_uri) {
		jalv_save_bank_preset(jalv, dir_preset, bank_uri, NULL, label_preset, fname_preset);
	} else {
		jalv_save_preset(jalv, dir_preset, NULL, label_preset, fname_preset);
	}
	//Generate preset uri
	sprintf(uri_preset, "file://%s%s", dir_preset, fname_preset);
	// Print feedback
	jalv_print_preset_str(uri_preset, label_preset);
	// Reload bundle into the world
	LilvNode* ldir = lilv_new_file_uri(jalv->world, NULL, dir_preset);
	lilv_world_unload_bundle(jalv->world, ldir);
	lilv_world_load_bundle(jalv->world, ldir);
	lilv_node_free(ldir);
	free(plugin_name);
}

void
jalv_command_load_preset(Jalv* jalv, char* sym)
{
	LilvNode* preset = lilv_new_uri(jalv->world, sym);
	lilv_world_load_resource(jalv->world, preset);
	jalv_apply_preset(jalv, preset);
	lilv_node_free(preset);
}
