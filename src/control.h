// Copyright 2007-2022 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_CONTROL_H
#define JALV_CONTROL_H

#include "attributes.h"
#include "nodes.h"

#include "lilv/lilv.h"
#include "lv2/atom/forge.h"
#include "lv2/urid/urid.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

JALV_BEGIN_DECLS

// Plugin control utilities

/// Type of plugin control
typedef enum {
  PORT,    ///< Control port
  PROPERTY ///< Property (set via atom message)
} ControlType;

// "Interesting" value in a control's value range
typedef struct {
  float value;
  char* label;
} ScalePoint;

/// Plugin control
typedef struct {
  ControlType     type;           ///< Type of control
  LilvNode*       node;           ///< Port or property
  LilvNode*       symbol;         ///< Symbol
  LilvNode*       label;          ///< Human readable label
  LV2_Atom_Forge* forge;          ///< Forge (for URIDs)
  LV2_URID        property;       ///< Iff type == PROPERTY
  uint32_t        index;          ///< Iff type == PORT
  LilvNode*       group;          ///< Port/control group, or NULL
  void*           widget;         ///< Control Widget
  size_t          n_points;       ///< Number of scale points
  ScalePoint*     points;         ///< Scale points
  LV2_URID        value_type;     ///< Type of control value
  LilvNode*       min;            ///< Minimum value
  LilvNode*       max;            ///< Maximum value
  LilvNode*       def;            ///< Default value
  bool            is_toggle;      ///< Boolean (0 and 1 only)
  bool            is_integer;     ///< Integer values only
  bool            is_enumeration; ///< Point values only
  bool            is_logarithmic; ///< Logarithmic scale
  bool            is_writable;    ///< Writable (input)
  bool            is_readable;    ///< Readable (output)
} ControlID;

/// Set of plugin controls
typedef struct {
  size_t      n_controls;
  ControlID** controls;
} Controls;

/// Control change event, sent through ring buffers for UI updates
typedef struct {
  uint32_t index;
  uint32_t protocol;
  uint32_t size;
  // Followed immediately by size bytes of data
} ControlChange;

/// Create a new ID for a control port
ControlID*
new_port_control(LilvWorld*        world,
                 const LilvPlugin* plugin,
                 const LilvPort*   port,
                 uint32_t          port_index,
                 float             sample_rate,
                 const JalvNodes*  nodes,
                 LV2_Atom_Forge*   forge);

/// Create a new ID for a property-based parameter
ControlID*
new_property_control(LilvWorld*       world,
                     const LilvNode*  property,
                     const JalvNodes* nodes,
                     LV2_URID_Map*    map,
                     LV2_Atom_Forge*  forge);

void
add_control(Controls* controls, ControlID* control);

ControlID*
get_property_control(const Controls* controls, LV2_URID property);

JALV_END_DECLS

#endif // JALV_CONTROL_H
