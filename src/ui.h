// Copyright 2007-2022 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_UI_H
#define JALV_UI_H

#include "jalv_internal.h"

#include "lilv/lilv.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Interface that must be implemented by UIs

int
jalv_init(int* argc, char*** argv, JalvOptions* opts);

const char*
jalv_native_ui_type(void);

bool
jalv_discover_ui(Jalv* jalv);

float
jalv_ui_refresh_rate(Jalv* jalv);

float
jalv_ui_scale_factor(Jalv* jalv);

int
jalv_open_ui(Jalv* jalv);

LilvNode*
jalv_select_plugin(Jalv* jalv);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // JALV_UI_H
