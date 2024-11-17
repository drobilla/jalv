// Copyright 2016-2024 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_PROCESS_H
#define JALV_PROCESS_H

#include "attributes.h"
#include "types.h"

#include <stdbool.h>
#include <stdint.h>

// Code and data used in the realtime process thread
JALV_BEGIN_DECLS

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

/**
   Bypass the plugin for a block of frames.

   This is like jalv_run(), but doesn't actually run the plugin and only does
   the minimum necessary internal work for the cycle.

   @param jalv Application state.
   @param nframes Number of frames to bypass.
   @return Zero.
*/
int
jalv_bypass(Jalv* jalv, uint32_t nframes);

JALV_END_DECLS

#endif // JALV_PROCESS_H
