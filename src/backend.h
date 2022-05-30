// Copyright 2007-2022 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_BACKEND_H
#define JALV_BACKEND_H

#include "jalv_internal.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Audio/MIDI I/O backend interface

JalvBackend*
jalv_backend_init(Jalv* jalv);

void
jalv_backend_activate(Jalv* jalv);

void
jalv_backend_deactivate(Jalv* jalv);

void
jalv_backend_close(Jalv* jalv);

/// Expose a port to the system (if applicable) and connect it to its buffer
void
jalv_backend_activate_port(Jalv* jalv, uint32_t port_index);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // JALV_BACKEND_H
