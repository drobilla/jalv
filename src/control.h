// Copyright 2007-2022 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_CONTROL_H
#define JALV_CONTROL_H

#include "attributes.h"
#include "nodes.h"

#include <lilv/lilv.h>
#include <lv2/atom/forge.h>
#include <lv2/urid/urid.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Support for plugin controls (control port or event-based)
JALV_BEGIN_DECLS

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
  ControlType type; ///< Type of control
  union {
    LV2_URID property; ///< Iff type == PROPERTY
    uint32_t index;    ///< Iff type == PORT
  } id;
  LilvNode*   node;           ///< Port or property
  LilvNode*   symbol;         ///< Symbol
  LilvNode*   label;          ///< Human readable label
  LilvNode*   group;          ///< Port/control group, or NULL
  void*       widget;         ///< Control Widget
  size_t      n_points;       ///< Number of scale points
  ScalePoint* points;         ///< Scale points
  LV2_URID    value_type;     ///< Type of control value
  float       min;            ///< Minimum value
  float       max;            ///< Maximum value
  float       def;            ///< Default value
  bool        is_toggle;      ///< Boolean (0 and 1 only)
  bool        is_integer;     ///< Integer values only
  bool        is_enumeration; ///< Point values only
  bool        is_logarithmic; ///< Logarithmic scale
  bool        is_writable;    ///< Writable (input)
  bool        is_readable;    ///< Readable (output)
  bool        is_hidden;      ///< Hidden from UI
} Control;

/// Set of plugin controls
typedef struct {
  size_t    n_controls;
  Control** controls;
} Controls;

/// Create a new ID for a control port
Control*
new_port_control(const LilvPlugin*     plugin,
                 const LilvPort*       port,
                 uint32_t              port_index,
                 float                 sample_rate,
                 const JalvNodes*      nodes,
                 const LV2_Atom_Forge* forge);

/// Create a new ID for a property-based parameter
Control*
new_property_control(LilvWorld*            world,
                     const LilvNode*       property,
                     const JalvNodes*      nodes,
                     LV2_URID_Map*         map,
                     const LV2_Atom_Forge* forge);

/// Free a control allocated with new_port_control() or new_property_control()
void
free_control(Control* control);

/// Add a control to the given controls set, reallocating as necessary
void
add_control(Controls* controls, Control* control);

/// Return a pointer to the control for the given control port, or null
Control*
get_port_control(const Controls* controls, uint32_t port_index);

/// Return a pointer to the control for the given property, or null
Control*
get_property_control(const Controls* controls, LV2_URID property);

JALV_END_DECLS

#endif // JALV_CONTROL_H
