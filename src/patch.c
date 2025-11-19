// Copyright 2015-2025 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "patch.h"

#include "jalv.h"
#include "log.h"
#include "types.h"

#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/atom/util.h>

int
patch_set_get(Jalv*                  jalv,
              const LV2_Atom_Object* obj,
              const LV2_Atom_URID**  property,
              const LV2_Atom**       value)
{
  lv2_atom_object_get(obj,
                      jalv->urids.patch_property,
                      (const LV2_Atom*)property,
                      jalv->urids.patch_value,
                      value,
                      0);
  if (!*property) {
    jalv_log(JALV_LOG_WARNING, "patch:Set message with no property\n");
    return 1;
  }

  if ((*property)->atom.type != jalv->forge.URID) {
    jalv_log(JALV_LOG_WARNING, "patch:Set property is not a URID\n");
    return 1;
  }

  return 0;
}

int
patch_put_get(Jalv*                   jalv,
              const LV2_Atom_Object*  obj,
              const LV2_Atom_Object** body)
{
  lv2_atom_object_get(obj, jalv->urids.patch_body, (const LV2_Atom*)body, 0);
  if (!*body) {
    jalv_log(JALV_LOG_WARNING, "patch:Put message with no body\n");
    return 1;
  }

  if (!lv2_atom_forge_is_object_type(&jalv->forge, (*body)->atom.type)) {
    jalv_log(JALV_LOG_WARNING, "patch:Put body is not an object\n");
    return 1;
  }

  return 0;
}
