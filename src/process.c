// Copyright 2016-2025 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "process.h"

#include "comm.h"
#include "lv2_evbuf.h"
#include "types.h"
#include "worker.h"

#include <lilv/lilv.h>
#include <lv2/atom/atom.h>
#include <lv2/core/lv2.h>
#include <zix/ring.h>
#include <zix/sem.h>

#include <assert.h>
#include <stddef.h>

char*
jalv_process_strerror(const JalvProcessStatus pst)
{
  switch (pst) {
  case JALV_PROCESS_SUCCESS:
    return "Success";
  case JALV_PROCESS_SEND_UPDATES:
    return "Send updates";
  case JALV_PROCESS_BAD_HEADER:
    return "Failed to read header from UI ring";
  case JALV_PROCESS_BAD_CONTROL_VALUE:
    return "Failed to read control value from UI ring";
  case JALV_PROCESS_BAD_EVENT:
    return "Failed to read event from UI ring";
  case JALV_PROCESS_BAD_STATE_CHANGE:
    return "Failed to read run state change from UI ring";
  case JALV_PROCESS_BAD_MESSAGE_TYPE:
    return "Unknown message type received from UI ring";
  }

  return "Unknown error";
}

static JalvProcessStatus
apply_ui_events(JalvProcess* const proc, const uint32_t nframes)
{
  ZixRing* const    ring   = proc->ui_to_plugin;
  JalvMessageHeader header = {NO_MESSAGE, 0U};
  const size_t      space  = zix_ring_read_space(ring);
  for (size_t i = 0; i < space; i += sizeof(header) + header.size) {
    // Read message header (which includes the body size)
    if (zix_ring_read(ring, &header, sizeof(header)) != sizeof(header)) {
      return JALV_PROCESS_BAD_HEADER;
    }

    if (header.type == CONTROL_PORT_CHANGE) {
      assert(header.size == sizeof(JalvControlChange));
      JalvControlChange msg = {0U, 0.0f};
      if (zix_ring_read(ring, &msg, sizeof(msg)) != sizeof(msg)) {
        return JALV_PROCESS_BAD_CONTROL_VALUE;
      }

      assert(msg.port_index < proc->num_ports);
      proc->controls_buf[msg.port_index] = msg.value;

    } else if (header.type == EVENT_TRANSFER) {
      assert(header.size <= proc->process_msg_size);
      void* const body = proc->process_msg;
      if (zix_ring_read(ring, body, header.size) != header.size) {
        return JALV_PROCESS_BAD_EVENT;
      }

      const JalvEventTransfer* const msg = (const JalvEventTransfer*)body;
      assert(msg->port_index < proc->num_ports);
      JalvProcessPort* const port = &proc->ports[msg->port_index];
      LV2_Evbuf_Iterator     e    = lv2_evbuf_end(port->evbuf);
      const LV2_Atom* const  atom = &msg->atom;
      lv2_evbuf_write(
        &e, nframes, 0U, atom->type, atom->size, LV2_ATOM_BODY_CONST(atom));

    } else if (header.type == STATE_REQUEST) {
      JalvProcessPort* const port = &proc->ports[proc->control_in];
      assert(port->type == TYPE_EVENT);
      assert(port->flow == FLOW_INPUT);
      assert(port->evbuf);

      LV2_Evbuf_Iterator e = lv2_evbuf_end(port->evbuf);
      lv2_evbuf_write(&e,
                      nframes,
                      0U,
                      proc->get_msg.atom.type,
                      proc->get_msg.atom.size,
                      &proc->get_msg.body);

    } else if (header.type == RUN_STATE_CHANGE) {
      assert(header.size == sizeof(JalvRunStateChange));
      JalvRunStateChange msg = {JALV_RUNNING};
      if (zix_ring_read(ring, &msg, sizeof(msg)) != sizeof(msg)) {
        return JALV_PROCESS_BAD_STATE_CHANGE;
      }

      proc->run_state = msg.state;
      if (msg.state == JALV_PAUSED) {
        zix_sem_post(&proc->paused);
      }

    } else {
      return JALV_PROCESS_BAD_MESSAGE_TYPE;
    }
  }

  return 0;
}

JalvProcessStatus
jalv_run(JalvProcess* const proc, const uint32_t nframes)
{
  // Read and apply control change events from UI
  JalvProcessStatus pst = apply_ui_events(proc, nframes);

  // Run plugin for this cycle
  lilv_instance_run(proc->instance, nframes);

  // Process any worker replies and end the cycle
  LV2_Handle handle = lilv_instance_get_handle(proc->instance);
  jalv_worker_emit_responses(proc->state_worker, handle);
  jalv_worker_emit_responses(proc->worker, handle);
  jalv_worker_end_run(proc->worker);

  // Check if it's time to send updates to the UI
  proc->pending_frames += nframes;
  if (proc->update_frames && proc->pending_frames > proc->update_frames) {
    proc->pending_frames = 0U;
    pst                  = pst ? pst : JALV_PROCESS_SEND_UPDATES;
  }

  return pst;
}

int
jalv_bypass(JalvProcess* const proc, const uint32_t nframes)
{
  // Read and apply control change events from UI
  apply_ui_events(proc, nframes);
  return 0;
}
