// Copyright 2016-2024 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_PROCESS_H
#define JALV_PROCESS_H

#include "attributes.h"
#include "types.h"

#include <stdbool.h>
#include <stdint.h>

JALV_BEGIN_DECLS

// Code and data used in the realtime process thread

/**
   Run the plugin for a block of frames.

   Applies any pending messages from the UI, runs the plugin instance, and
   processes any worker replies.

   @param jalv Application state.
   @param nframes Number of frames to process.
   @return Whether output value updates should be sent to the UI now.
*/
bool
jalv_run(Jalv* jalv, uint32_t nframes);

JALV_END_DECLS

#endif // JALV_PROCESS_H