// Copyright 2016-2024 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "process.h"

#include "comm.h"
#include "jalv_internal.h"
#include "log.h"
#include "lv2_evbuf.h"
#include "port.h"
#include "types.h"
#include "worker.h"

#include <lilv/lilv.h>
#include <lv2/atom/atom.h>
#include <lv2/core/lv2.h>
#include <zix/ring.h>
#include <zix/sem.h>

#include <assert.h>
#include <stddef.h>

static int
ring_error(const char* const message)
{
  jalv_log(JALV_LOG_ERR, "%s", message);
  return 1;
}

static int
apply_ui_events(Jalv* const jalv, const uint32_t nframes)
{
  if (!jalv->has_ui) {
    return 0;
  }

  ZixRing* const    ring   = jalv->ui_to_plugin;
  JalvMessageHeader header = {NO_MESSAGE, 0U};
  const size_t      space  = zix_ring_read_space(ring);
  for (size_t i = 0; i < space; i += sizeof(header) + header.size) {
    // Read message header (which includes the body size)
    if (zix_ring_read(ring, &header, sizeof(header)) != sizeof(header)) {
      return ring_error("Failed to read header from UI ring\n");
    }

    if (header.type == CONTROL_PORT_CHANGE) {
      assert(header.size == sizeof(JalvControlChange));
      JalvControlChange msg = {0U, 0.0f};
      if (zix_ring_read(ring, &msg, sizeof(msg)) != sizeof(msg)) {
        return ring_error("Failed to read control value from UI ring\n");
      }

      assert(msg.port_index < jalv->num_ports);
      jalv->controls_buf[msg.port_index] = msg.value;

    } else if (header.type == EVENT_TRANSFER) {
      assert(header.size <= jalv->msg_buf_size);
      void* const body = jalv->audio_msg;
      if (zix_ring_read(ring, body, header.size) != header.size) {
        return ring_error("Failed to read event from UI ring\n");
      }

      const JalvEventTransfer* const msg = (const JalvEventTransfer*)body;
      assert(msg->port_index < jalv->num_ports);
      JalvPort* const       port = &jalv->ports[msg->port_index];
      LV2_Evbuf_Iterator    e    = lv2_evbuf_end(port->evbuf);
      const LV2_Atom* const atom = &msg->atom;
      lv2_evbuf_write(
        &e, nframes, 0U, atom->type, atom->size, LV2_ATOM_BODY_CONST(atom));

    } else if (header.type == STATE_REQUEST) {
      JalvPort* const port = &jalv->ports[jalv->control_in];
      assert(port->type == TYPE_EVENT);
      assert(port->flow == FLOW_INPUT);
      assert(port->evbuf);

      LV2_Evbuf_Iterator    iter = lv2_evbuf_end(port->evbuf);
      const LV2_Atom_Object get  = {
        {sizeof(LV2_Atom_Object_Body), jalv->urids.atom_Object},
        {0U, jalv->urids.patch_Get},
      };

      lv2_evbuf_write(
        &iter, nframes, 0U, get.atom.type, get.atom.size, &get.body);

    } else if (header.type == RUN_STATE_CHANGE) {
      assert(header.size == sizeof(JalvRunStateChange));
      JalvRunStateChange msg = {JALV_RUNNING};
      if (zix_ring_read(ring, &msg, sizeof(msg)) != sizeof(msg)) {
        return ring_error("Failed to read run state change from UI ring\n");
      }

      jalv->run_state = msg.state;
      if (msg.state == JALV_PAUSED) {
        zix_sem_post(&jalv->paused);
      }

    } else {
      return ring_error("Unknown message type received from UI ring\n");
    }
  }

  return 0;
}

bool
jalv_run(Jalv* const jalv, const uint32_t nframes)
{
  // Read and apply control change events from UI
  apply_ui_events(jalv, nframes);

  // Run plugin for this cycle
  lilv_instance_run(jalv->instance, nframes);

  // Process any worker replies and end the cycle
  LV2_Handle handle = lilv_instance_get_handle(jalv->instance);
  jalv_worker_emit_responses(jalv->state_worker, handle);
  jalv_worker_emit_responses(jalv->worker, handle);
  jalv_worker_end_run(jalv->worker);

  // Check if it's time to send updates to the UI
  jalv->event_delta_t += nframes;
  bool     send_ui_updates = false;
  uint32_t update_frames   = (uint32_t)(jalv->sample_rate / jalv->ui_update_hz);
  if (jalv->has_ui && (jalv->event_delta_t > update_frames)) {
    send_ui_updates     = true;
    jalv->event_delta_t = 0U;
  }

  return send_ui_updates;
}

int
jalv_bypass(Jalv* const jalv, const uint32_t nframes)
{
  // Read and apply control change events from UI
  apply_ui_events(jalv, nframes);
  return 0;
}
