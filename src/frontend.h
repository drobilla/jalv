// Copyright 2007-2025 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_FRONTEND_H
#define JALV_FRONTEND_H

#include "attributes.h"
#include "control.h"
#include "types.h"

#include <lilv/lilv.h>

#include <stdbool.h>
#include <stdint.h>

// Interface that must be implemented by UIs
JALV_BEGIN_DECLS

/// Arbitrary return code for successful early exit (for --help and so on)
#define JALV_EARLY_EXIT_STATUS (-431)

/// Allocate and prepare application
int
jalv_frontend_init(Jalv* jalv);

/// Return the URI of the "native" LV2 UI type
const char*
jalv_frontend_ui_type(void);

/// Return true if an interactive frontend is available
bool
jalv_frontend_discover(const Jalv* jalv);

/// Return the ideal refresh rate of the frontend in Hz
float
jalv_frontend_refresh_rate(const Jalv* jalv);

/// Return the scale factor of the frontend (for example 2.0 for double sized)
float
jalv_frontend_scale_factor(const Jalv* jalv);

/// Attempt to get a plugin URI selection from the user
LilvNode*
jalv_frontend_select_plugin(LilvWorld* world);

/// Open and run the frontend interface, signalling jalv.done when finished
int
jalv_frontend_run(Jalv* jalv);

/// Quit and close the frontend interface
int
jalv_frontend_close(Jalv* jalv);

/// Called when a control value change is sent to the UI
void
jalv_frontend_set_control(const Jalv*    jalv,
                          const Control* control,
                          uint32_t       value_size,
                          uint32_t       value_type,
                          const void*    value_body);

JALV_END_DECLS

#endif // JALV_FRONTEND_H
