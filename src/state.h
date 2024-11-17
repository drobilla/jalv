// Copyright 2007-2022 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_STATE_H
#define JALV_STATE_H

#include "attributes.h"
#include "types.h"

#include "lilv/lilv.h"
#include "lv2/state/state.h"

// State and preset utilities
JALV_BEGIN_DECLS

typedef int (*PresetSink)(Jalv*           jalv,
                          const LilvNode* node,
                          const LilvNode* title,
                          void*           data);

int
jalv_load_presets(Jalv* jalv, PresetSink sink, void* data);

int
jalv_unload_presets(Jalv* jalv);

int
jalv_apply_preset(Jalv* jalv, const LilvNode* preset);

int
jalv_delete_current_preset(Jalv* jalv);

int
jalv_save_preset(Jalv*       jalv,
                 const char* dir,
                 const char* uri,
                 const char* label,
                 const char* filename);

void
jalv_save(Jalv* jalv, const char* dir);

char*
jalv_make_path(LV2_State_Make_Path_Handle handle, const char* path);

void
jalv_apply_state(Jalv* jalv, const LilvState* state);

JALV_END_DECLS

#endif // JALV_STATE_H
