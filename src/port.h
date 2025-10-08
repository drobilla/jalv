// Copyright 2007-2022 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_PORT_H
#define JALV_PORT_H

#include "attributes.h"
#include "lv2_evbuf.h"

#include "lilv/lilv.h"

#include <stddef.h>
#include <stdint.h>

JALV_BEGIN_DECLS

enum PortFlow { FLOW_UNKNOWN, FLOW_INPUT, FLOW_OUTPUT };

enum PortType { TYPE_UNKNOWN, TYPE_CONTROL, TYPE_AUDIO, TYPE_EVENT, TYPE_CV };

struct Port {
  const LilvPort* lilv_port; ///< LV2 port
  enum PortType   type;      ///< Data type
  enum PortFlow   flow;      ///< Data flow direction
  void*           sys_port;  ///< For audio/MIDI ports, otherwise NULL
  LV2_Evbuf*      evbuf;     ///< For MIDI ports, otherwise NULL
  void*           widget;    ///< Control widget, if applicable
  size_t          buf_size;  ///< Custom buffer size, or 0
  uint32_t        index;     ///< Port index
  float           control;   ///< For control ports, otherwise 0.0f
  float           defval;    ///< For control ports, otherwise 0.0f
  bool            is_set;    ///< For control ports: is value set?
};

JALV_END_DECLS

#endif // JALV_PORT_H
