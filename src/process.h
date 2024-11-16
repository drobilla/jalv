// Copyright 2016-2024 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_PROCESS_H
#define JALV_PROCESS_H

#include "attributes.h"
#include "lv2_evbuf.h"
#include "types.h"
#include "urids.h"

#include <stdbool.h>
#include <stdint.h>

JALV_BEGIN_DECLS

// Code and data used in the realtime process thread

/**
   Write a patch:Get message to an event buffer.

   This is used to request an update of plugin state when it has changed or the
   UI needs updating for whatever reason.
*/
int
jalv_write_get_message(LV2_Evbuf_Iterator* iter, const JalvURIDs* urids);

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
