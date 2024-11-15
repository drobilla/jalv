// Copyright 2007-2022 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_UI_H
#define JALV_UI_H

#include "attributes.h"
#include "options.h"
#include "types.h"

#include "lilv/lilv.h"

#include <stdbool.h>

JALV_BEGIN_DECLS

// Interface that must be implemented by UIs

/// Command-line arguments passed to an executable
typedef struct {
  int*    argc; ///< Pointer to `argc` like in `main`
  char*** argv; ///< Pointer to `argv` like in `main`
} JalvFrontendArgs;

/// Consume command-line arguments and set `opts` accordingly
int
jalv_frontend_init(JalvFrontendArgs* args, JalvOptions* opts);

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
jalv_frontend_select_plugin(Jalv* jalv);

/// Open and run the frontend interface, signalling jalv.done when finished
int
jalv_frontend_open(Jalv* jalv);

/// Quit and close the frontend interface
int
jalv_frontend_close(Jalv* jalv);

/// Called when a port event (control change or other message) is sent to the UI
void
jalv_frontend_port_event(Jalv*       jalv,
                         uint32_t    port_index,
                         uint32_t    buffer_size,
                         uint32_t    protocol,
                         const void* buffer);

JALV_END_DECLS

#endif // JALV_UI_H
