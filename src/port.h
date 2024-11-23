// Copyright 2007-2022 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_PORT_H
#define JALV_PORT_H

#include "attributes.h"
#include "lv2_evbuf.h"
#include "types.h"

#include <lilv/lilv.h>

#include <stddef.h>
#include <stdint.h>

// Application port state
JALV_BEGIN_DECLS

typedef struct {
  const LilvPort* lilv_port; ///< LV2 port
  PortType        type;      ///< Data type
  PortFlow        flow;      ///< Data flow direction
  void*           widget;    ///< Control widget, if applicable
  uint32_t        index;     ///< Port index
} JalvPort;

JALV_END_DECLS

#endif // JALV_PORT_H
