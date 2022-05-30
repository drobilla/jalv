// Copyright 2007-2022 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_CONTROL_H
#define JALV_CONTROL_H

#include "jalv_internal.h"
#include "nodes.h"

#include "lilv/lilv.h"
#include "lv2/atom/forge.h"
#include "lv2/urid/urid.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Plugin control utilities

/// Order scale points by value
int
scale_point_cmp(const ScalePoint* a, const ScalePoint* b);

ControlID*
new_port_control(LilvWorld*        world,
                 const LilvPlugin* plugin,
                 const LilvPort*   port,
                 uint32_t          port_index,
                 float             sample_rate,
                 const JalvNodes*  nodes,
                 LV2_Atom_Forge*   forge);

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

#ifdef __cplusplus
} // extern "C"
#endif

#endif // JALV_CONTROL_H
