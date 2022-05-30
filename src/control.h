// Copyright 2007-2022 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_CONTROL_H
#define JALV_CONTROL_H

#include "jalv_internal.h"

#include "lilv/lilv.h"
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
new_port_control(Jalv* jalv, uint32_t index);

ControlID*
new_property_control(Jalv* jalv, const LilvNode* property);

void
add_control(Controls* controls, ControlID* control);

ControlID*
get_property_control(const Controls* controls, LV2_URID property);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // JALV_CONTROL_H
