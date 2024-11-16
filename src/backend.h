// Copyright 2007-2022 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_BACKEND_H
#define JALV_BACKEND_H

#include "attributes.h"
#include "types.h"

#include <stdint.h>

JALV_BEGIN_DECLS

// Interface that must be implemented by audio/MIDI backends

/// Initialize the audio and MIDI systems
JalvBackend*
jalv_backend_init(Jalv* jalv);

/// Activate the backend and start processing audio
void
jalv_backend_activate(Jalv* jalv);

/// Deactivate the backend and stop processing audio
void
jalv_backend_deactivate(Jalv* jalv);

/// Close the backend
void
jalv_backend_close(Jalv* jalv);

/// Expose a port to the system (if applicable) and connect it to its buffer
void
jalv_backend_activate_port(Jalv* jalv, uint32_t port_index);

/// Recompute latencies based on plugin port latencies if necessary
void
jalv_backend_recompute_latencies(Jalv* jalv);

JALV_END_DECLS

#endif // JALV_BACKEND_H
