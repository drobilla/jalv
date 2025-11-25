// Copyright 2015-2025 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_PATCH_H
#define JALV_PATCH_H

#include "attributes.h"
#include "types.h"

#include <lv2/atom/atom.h>
#include <lv2/urid/urid.h>

// Utility functions for getting values from LV2 patch messages
JALV_BEGIN_DECLS

typedef void (*PropertyChangedFunc)(LV2_URID        key,
                                    const LV2_Atom* value,
                                    void*           user_data);

/// Get the property and value from a patch:Set message
int
patch_set_get(Jalv*                  jalv,
              const LV2_Atom_Object* obj,
              const LV2_Atom_URID**  property,
              const LV2_Atom**       value);

/// Get the object body from a patch:Put message
int
patch_put_get(Jalv*                   jalv,
              const LV2_Atom_Object*  obj,
              const LV2_Atom_Object** body);

/// Call a callback for every property changed in a Set or Put message
int
patch_changed_properties(Jalv*                  jalv,
                         const LV2_Atom_Object* obj,
                         PropertyChangedFunc    property_changed,
                         void*                  property_changed_user_data);

JALV_END_DECLS

#endif // JALV_PATCH_H
