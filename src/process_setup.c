// Copyright 2016-2024 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "process_setup.h"

#include "jalv.h"
#include "lv2_evbuf.h"
#include "port.h"
#include "types.h"
#include "urids.h"

#include <lilv/lilv.h>

#include <stddef.h>
#include <stdint.h>

void
jalv_allocate_port_buffers(Jalv* const jalv)
{
  const JalvURIDs* const urids = &jalv->urids;

  for (uint32_t i = 0; i < jalv->num_ports; ++i) {
    JalvPort* const port = &jalv->ports[i];
    if (port->type == TYPE_EVENT) {
      const size_t size =
        port->buf_size ? port->buf_size : jalv->settings.midi_buf_size;

      lv2_evbuf_free(port->evbuf);
      port->evbuf =
        lv2_evbuf_new(size, urids->atom_Chunk, urids->atom_Sequence);

      lv2_evbuf_reset(port->evbuf, port->flow == FLOW_INPUT);
      lilv_instance_connect_port(
        jalv->instance, i, lv2_evbuf_get_buffer(port->evbuf));
    }
  }
}

void
jalv_free_port_buffers(Jalv* const jalv)
{
  for (uint32_t i = 0; i < jalv->num_ports; ++i) {
    lv2_evbuf_free(jalv->ports[i].evbuf);
    lilv_instance_connect_port(jalv->instance, i, NULL);
  }
}
