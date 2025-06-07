// Copyright 2016-2024 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "process_setup.h"

#include "jalv_config.h"
#include "log.h"
#include "lv2_evbuf.h"
#include "macros.h"
#include "mapper.h"
#include "nodes.h"
#include "process.h"
#include "query.h"
#include "settings.h"
#include "string_utils.h"
#include "types.h"
#include "urids.h"
#include "worker.h"

#include <lilv/lilv.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <zix/allocator.h>
#include <zix/ring.h>
#include <zix/sem.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

int
jalv_process_init(JalvProcess* const     proc,
                  const JalvURIDs* const urids,
                  JalvMapper* const      mapper,
                  const uint32_t         update_frames)
{
  proc->get_msg.atom.size  = sizeof(LV2_Atom_Object_Body);
  proc->get_msg.atom.type  = urids->atom_Object;
  proc->get_msg.body.id    = 0U;
  proc->get_msg.body.otype = urids->patch_Get;

  proc->instance         = NULL;
  proc->ui_to_plugin     = NULL;
  proc->plugin_to_ui     = NULL;
  proc->worker           = NULL;
  proc->state_worker     = NULL;
  proc->ports            = NULL;
  proc->process_msg_size = 1024U;
  proc->process_msg      = NULL;
  proc->run_state        = JALV_PAUSED;
  proc->control_in       = UINT32_MAX;
  proc->num_ports        = 0U;
  proc->pending_frames   = 0U;
  proc->update_frames    = update_frames;
  proc->position         = 0U;
  proc->bpm              = 120.0f;
  proc->rolling          = false;
  proc->has_ui           = false;

  zix_sem_init(&proc->paused, 0);
  lv2_atom_forge_init(&proc->forge, jalv_mapper_urid_map(mapper));

  return 0;
}

void
jalv_process_cleanup(JalvProcess* const proc)
{
  zix_sem_destroy(&proc->paused);
  jalv_worker_free(proc->worker);
  jalv_worker_free(proc->state_worker);
  zix_ring_free(proc->ui_to_plugin);
  zix_ring_free(proc->plugin_to_ui);
  zix_aligned_free(NULL, proc->process_msg);

  for (uint32_t i = 0U; i < proc->num_ports; ++i) {
    jalv_process_port_cleanup(&proc->ports[i]);
  }
}

void
jalv_process_activate(JalvProcess* const        proc,
                      const JalvURIDs* const    urids,
                      LilvInstance* const       instance,
                      const JalvSettings* const settings)
{
  proc->instance = instance;

  for (uint32_t i = 0U; i < proc->num_ports; ++i) {
    JalvProcessPort* const port = &proc->ports[i];
    if (port->type == TYPE_EVENT) {
      const size_t size =
        port->buf_size ? port->buf_size : settings->midi_buf_size;

      lv2_evbuf_free(port->evbuf);
      port->evbuf =
        lv2_evbuf_new(size, urids->atom_Chunk, urids->atom_Sequence);

      lv2_evbuf_reset(port->evbuf, port->flow == FLOW_INPUT);
      lilv_instance_connect_port(
        proc->instance, i, lv2_evbuf_get_buffer(port->evbuf));

      if (port->flow == FLOW_INPUT) {
        proc->process_msg_size = MAX(proc->process_msg_size, port->buf_size);
      }
    }
  }

  // Allocate UI<=>process communication rings and process receive buffer
  proc->ui_to_plugin = zix_ring_new(NULL, settings->ring_size);
  proc->plugin_to_ui = zix_ring_new(NULL, settings->ring_size);
  proc->process_msg  = zix_aligned_alloc(NULL, 8U, proc->process_msg_size);
  zix_ring_mlock(proc->ui_to_plugin);
  zix_ring_mlock(proc->plugin_to_ui);
  zix_ring_mlock(proc->process_msg);
}

void
jalv_process_deactivate(JalvProcess* const proc)
{
  zix_aligned_free(NULL, proc->process_msg);
  proc->process_msg = NULL;

  for (uint32_t i = 0U; i < proc->num_ports; ++i) {
    lv2_evbuf_free(proc->ports[i].evbuf);
    lilv_instance_connect_port(proc->instance, i, NULL);
    proc->ports[i].evbuf = NULL;
  }
}

int
jalv_process_port_init(JalvProcessPort* const  port,
                       const JalvNodes* const  nodes,
                       const LilvPlugin* const lilv_plugin,
                       const LilvPort* const   lilv_port)
{
  const LilvNode* const symbol = lilv_port_get_symbol(lilv_plugin, lilv_port);

  port->type            = TYPE_UNKNOWN;
  port->flow            = FLOW_UNKNOWN;
  port->sys_port        = NULL;
  port->evbuf           = NULL;
  port->buf_size        = 0U;
  port->reports_latency = false;

  const bool optional = lilv_port_has_property(
    lilv_plugin, lilv_port, nodes->lv2_connectionOptional);

  // Set port flow (input or output)
  if (lilv_port_is_a(lilv_plugin, lilv_port, nodes->lv2_InputPort)) {
    port->flow = FLOW_INPUT;
  } else if (lilv_port_is_a(lilv_plugin, lilv_port, nodes->lv2_OutputPort)) {
    port->flow = FLOW_OUTPUT;
  } else if (!optional) {
    jalv_log(JALV_LOG_ERR,
             "Mandatory port \"%s\" is neither input nor output\n",
             lilv_node_as_string(symbol));
    return 1;
  }

  // Set port type
  if (lilv_port_is_a(lilv_plugin, lilv_port, nodes->lv2_ControlPort)) {
    port->type = TYPE_CONTROL;
  } else if (lilv_port_is_a(lilv_plugin, lilv_port, nodes->lv2_AudioPort)) {
    port->type = TYPE_AUDIO;
#if USE_JACK_METADATA
  } else if (lilv_port_is_a(lilv_plugin, lilv_port, nodes->lv2_CVPort)) {
    port->type = TYPE_CV;
#endif
  } else if (lilv_port_is_a(lilv_plugin, lilv_port, nodes->atom_AtomPort)) {
    port->type = TYPE_EVENT;
  } else if (!optional) {
    jalv_log(JALV_LOG_ERR,
             "Mandatory port \"%s\" has unknown data type\n",
             lilv_node_as_string(symbol));
    return 1;
  }

  // Set symbol and label
  LilvNode* const name = lilv_port_get_name(lilv_plugin, lilv_port);
  port->symbol = symbol ? jalv_strdup(lilv_node_as_string(symbol)) : NULL;
  port->label  = name ? jalv_strdup(lilv_node_as_string(name)) : NULL;
  lilv_node_free(name);

  // Set buffer size
  LilvNode* const min_size =
    lilv_port_get(lilv_plugin, lilv_port, nodes->rsz_minimumSize);
  if (min_size && lilv_node_is_int(min_size)) {
    port->buf_size = (uint32_t)MAX(lilv_node_as_int(min_size), 0);
  }
  lilv_node_free(min_size);

  // Set primary flag for designated control port
  if (port->type == TYPE_EVENT &&
      jalv_port_has_designation(
        nodes, lilv_plugin, lilv_port, nodes->lv2_control)) {
    port->is_primary = true;
  }

  // Set reports_latency flag
  if (port->flow == FLOW_OUTPUT && port->type == TYPE_CONTROL &&
      (lilv_port_has_property(
         lilv_plugin, lilv_port, nodes->lv2_reportsLatency) ||
       jalv_port_has_designation(
         nodes, lilv_plugin, lilv_port, nodes->lv2_latency))) {
    port->reports_latency = true;
  }

  // Set supports_midi flag
  port->supports_midi =
    lilv_port_supports_event(lilv_plugin, lilv_port, nodes->midi_MidiEvent);

  return 0;
}

void
jalv_process_port_cleanup(JalvProcessPort* const port)
{
  if (port) {
    if (port->evbuf) {
      lv2_evbuf_free(port->evbuf);
    }
    free(port->label);
    free(port->symbol);
  }
}
