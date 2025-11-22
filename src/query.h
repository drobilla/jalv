// Copyright 2012-2024 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_QUERY_H
#define JALV_QUERY_H

#include "attributes.h"
#include "nodes.h"

#include <lilv/lilv.h>

#include <stdbool.h>

// Lilv query utilities
JALV_BEGIN_DECLS

/// Return whether a port has a given designation
bool
jalv_port_has_designation(const JalvNodes*  nodes,
                          const LilvPlugin* plugin,
                          const LilvPort*   port,
                          const LilvNode*   designation);

/// Return whether a UI is described as resizable
bool
jalv_ui_is_resizable(LilvWorld* world, const LilvUI* ui);

JALV_END_DECLS

#endif // JALV_QUERY_H
