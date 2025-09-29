// Copyright 2007-2022 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "control.h"

#include "log.h"
#include "nodes.h"
#include "string_utils.h"

#include <lilv/lilv.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/urid/urid.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

/// Order scale points by value
static int
scale_point_cmp(const ScalePoint* a, const ScalePoint* b)
{
  if (a->value < b->value) {
    return -1;
  }

  if (a->value == b->value) {
    return 0;
  }

  return 1;
}

Control*
new_port_control(const LilvPlugin* const     plugin,
                 const LilvPort* const       port,
                 uint32_t                    port_index,
                 const float                 sample_rate,
                 const JalvNodes* const      nodes,
                 const LV2_Atom_Forge* const forge)
{
  Control* const id = (Control*)calloc(1, sizeof(Control));

  id->type        = PORT;
  id->id.index    = port_index;
  id->node        = lilv_node_duplicate(lilv_port_get_node(plugin, port));
  id->symbol      = lilv_node_duplicate(lilv_port_get_symbol(plugin, port));
  id->label       = lilv_port_get_name(plugin, port);
  id->group       = lilv_port_get(plugin, port, nodes->pg_group);
  id->value_type  = forge->Float;
  id->is_writable = lilv_port_is_a(plugin, port, nodes->lv2_InputPort);
  id->is_readable = lilv_port_is_a(plugin, port, nodes->lv2_OutputPort);
  id->is_toggle   = lilv_port_has_property(plugin, port, nodes->lv2_toggled);
  id->is_integer  = lilv_port_has_property(plugin, port, nodes->lv2_integer);

  id->is_enumeration =
    lilv_port_has_property(plugin, port, nodes->lv2_enumeration);

  id->is_logarithmic =
    lilv_port_has_property(plugin, port, nodes->pprops_logarithmic);

  // Set range and default
  LilvNode* def = NULL;
  LilvNode* min = NULL;
  LilvNode* max = NULL;
  lilv_port_get_range(plugin, port, &def, &min, &max);
  id->def = lilv_node_as_float(def);
  id->min = lilv_node_as_float(min);
  id->max = lilv_node_as_float(max);
  if (lilv_port_has_property(plugin, port, nodes->lv2_sampleRate)) {
    // Adjust range for lv2:sampleRate controls
    id->min *= sample_rate;
    id->max *= sample_rate;
  }
  lilv_node_free(max);
  lilv_node_free(min);
  lilv_node_free(def);

  // Get scale points
  LilvScalePoints* sp = lilv_port_get_scale_points(plugin, port);
  if (sp) {
    id->points =
      (ScalePoint*)malloc(lilv_scale_points_size(sp) * sizeof(ScalePoint));

    size_t np = 0;
    LILV_FOREACH (scale_points, s, sp) {
      const LilvScalePoint* p = lilv_scale_points_get(sp, s);
      if (lilv_node_is_float(lilv_scale_point_get_value(p)) ||
          lilv_node_is_int(lilv_scale_point_get_value(p))) {
        id->points[np].value =
          lilv_node_as_float(lilv_scale_point_get_value(p));
        id->points[np].label =
          jalv_strdup(lilv_node_as_string(lilv_scale_point_get_label(p)));
        ++np;
      }
      // TODO: Non-float scale points?
    }

    qsort(id->points,
          np,
          sizeof(ScalePoint),
          (int (*)(const void*, const void*))scale_point_cmp);

    id->n_points = np;
    lilv_scale_points_free(sp);
  }

  return id;
}

static bool
has_range(LilvWorld* const       world,
          const JalvNodes* const nodes,
          const LilvNode* const  subject,
          const char* const      range_uri)
{
  LilvNode*  range  = lilv_new_uri(world, range_uri);
  const bool result = lilv_world_ask(world, subject, nodes->rdfs_range, range);

  lilv_node_free(range);
  return result;
}

static float
get_float(LilvWorld* const      world,
          const LilvNode* const subject,
          const LilvNode* const predicate,
          const float           fallback)
{
  LilvNode* const node = lilv_world_get(world, subject, predicate, NULL);

  const float value = node ? lilv_node_as_float(node) : fallback;

  lilv_node_free(node);
  return value;
}

Control*
new_property_control(LilvWorld* const            world,
                     const LilvNode*             property,
                     const JalvNodes* const      nodes,
                     LV2_URID_Map* const         map,
                     const LV2_Atom_Forge* const forge)
{
  Control* const id = (Control*)calloc(1, sizeof(Control));

  id->type        = PROPERTY;
  id->id.property = map->map(map->handle, lilv_node_as_uri(property));
  id->node        = lilv_node_duplicate(property);
  id->symbol      = lilv_world_get_symbol(world, property);
  id->label       = lilv_world_get(world, property, nodes->rdfs_label, NULL);
  id->min         = get_float(world, property, nodes->lv2_minimum, 0.0f);
  id->max         = get_float(world, property, nodes->lv2_maximum, 1.0f);
  id->def         = get_float(world, property, nodes->lv2_default, 0.0f);

  const char* const types[] = {LV2_ATOM__Int,
                               LV2_ATOM__Long,
                               LV2_ATOM__Float,
                               LV2_ATOM__Double,
                               LV2_ATOM__Bool,
                               LV2_ATOM__String,
                               LV2_ATOM__Path,
                               NULL};

  for (const char* const* t = types; *t; ++t) {
    if (has_range(world, nodes, property, *t)) {
      id->value_type = map->map(map->handle, *t);
      break;
    }
  }

  id->is_toggle = (id->value_type == forge->Bool);
  id->is_integer =
    (id->value_type == forge->Int || id->value_type == forge->Long);

  if (!id->value_type) {
    jalv_log(JALV_LOG_WARNING,
             "Unknown value type for property <%s>\n",
             lilv_node_as_string(property));
  }

  return id;
}

void
free_control(Control* const control)
{
  for (size_t i = 0; i < control->n_points; ++i) {
    free(control->points[i].label);
  }

  free(control->points);
  lilv_node_free(control->node);
  lilv_node_free(control->symbol);
  lilv_node_free(control->label);
  lilv_node_free(control->group);
  free(control);
}

void
add_control(Controls* controls, Control* control)
{
  Control** const new_controls = (Control**)realloc(
    controls->controls, (controls->n_controls + 1) * sizeof(Control*));

  if (new_controls) {
    controls->controls                         = new_controls;
    controls->controls[controls->n_controls++] = control;
  }
}

Control*
get_property_control(const Controls* controls, LV2_URID property)
{
  for (size_t i = 0; i < controls->n_controls; ++i) {
    if (controls->controls[i]->type == PROPERTY &&
        controls->controls[i]->id.property == property) {
      return controls->controls[i];
    }
  }

  return NULL;
}
