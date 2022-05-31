// Copyright 2007-2016 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "state.h"

#include "control.h"
#include "jalv_internal.h"
#include "log.h"
#include "nodes.h"
#include "port.h"

#include "lilv/lilv.h"
#include "lv2/atom/forge.h"
#include "lv2/core/lv2.h"
#include "lv2/state/state.h"
#include "lv2/urid/urid.h"
#include "zix/common.h"
#include "zix/ring.h"
#include "zix/sem.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

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
set_port_value(const char* port_symbol,
               void*       user_data,
               const void* value,
               uint32_t    ZIX_UNUSED(size),
               uint32_t    type)
{
  Jalv*        jalv = (Jalv*)user_data;
  struct Port* port = jalv_port_by_symbol(jalv, port_symbol);
  if (!port) {
    jalv_log(JALV_LOG_ERR, "Preset port `%s' is missing\n", port_symbol);
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
    jalv_log(JALV_LOG_ERR,
             "Preset `%s' value has bad type <%s>\n",
             port_symbol,
             jalv->unmap.unmap(jalv->unmap.handle, type));
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
    char           buf[sizeof(ControlChange) + sizeof(fvalue)];
    ControlChange* ev = (ControlChange*)buf;
    ev->index         = port->index;
    ev->protocol      = 0;
    ev->size          = sizeof(fvalue);
    *(float*)(ev + 1) = fvalue;
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
