// Copyright 2007-2024 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_BACKEND_H
#define JALV_BACKEND_H

#include "attributes.h"
#include "process.h"
#include "settings.h"
#include "types.h"
#include "urids.h"

#include <zix/attributes.h>
#include <zix/sem.h>

#include <stdbool.h>
#include <stdint.h>

// Interface that must be implemented by audio/MIDI backends
JALV_BEGIN_DECLS

/// Allocate a new uninitialized backend
ZIX_MALLOC_FUNC JalvBackend*
jalv_backend_allocate(void);

/// Free a backend allocated with jalv_backend_allocate()
void
jalv_backend_free(JalvBackend* backend);

/// Open the audio/MIDI system
int
jalv_backend_open(JalvBackend*     backend,
                  const JalvURIDs* urids,
                  JalvSettings*    settings,
                  JalvProcess*     process,
                  ZixSem*          done,
                  const char*      name,
                  bool             exact_name);

/// Close the audio/MIDI system
void
jalv_backend_close(JalvBackend* backend);

/// Activate the backend and start processing audio
void
jalv_backend_activate(JalvBackend* backend);

/// Deactivate the backend and stop processing audio
void
jalv_backend_deactivate(JalvBackend* backend);

/// Expose a port to the system (if applicable) and connect it to its buffer
void
jalv_backend_activate_port(JalvBackend* backend,
                           JalvProcess* process,
                           uint32_t     port_index);

/// Recompute latencies based on plugin port latencies if necessary
void
jalv_backend_recompute_latencies(JalvBackend* backend);

JALV_END_DECLS

#endif // JALV_BACKEND_H
