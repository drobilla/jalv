// Copyright 2016-2024 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_PROCESS_SETUP_H
#define JALV_PROCESS_SETUP_H

#include "attributes.h"
#include "types.h"

// Code for setting up the realtime process thread (but that isn't used in it)
JALV_BEGIN_DECLS

/// Allocate appropriately-sized port buffers and connect the plugin to them
void
jalv_allocate_port_buffers(Jalv* jalv);

/// Clean up memory allocated by jalv_process_activate() and disconnect plugin
void
jalv_free_port_buffers(Jalv* jalv);

JALV_END_DECLS

#endif // JALV_PROCESS_SETUP_H
