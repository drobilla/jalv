// Copyright 2007-2024 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "backend.h"

#include "comm.h"
#include "frontend.h"
#include "jack_impl.h"
#include "jalv.h"
#include "jalv_config.h"
#include "log.h"
#include "lv2_evbuf.h"
#include "port.h"
#include "process.h"
#include "string_utils.h"
#include "types.h"
#include "urids.h"

#include <lilv/lilv.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/urid/urid.h>
#include <zix/sem.h>

#include <jack/jack.h>
#include <jack/midiport.h>
#include <jack/transport.h>
#include <jack/types.h>

#if USE_JACK_METADATA
#  include <jack/metadata.h>
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __clang__
#  define REALTIME __attribute__((annotate("realtime")))
#else
#  define REALTIME
#endif

/// Maximum supported latency in frames (at most 2^24 so all integers work)
static const float max_latency = 16777216.0f;

/// Jack buffer size callback
static int
jack_buffer_size_cb(jack_nframes_t nframes, void* data)
{
  Jalv* const jalv   = (Jalv*)data;
  jalv->block_length = nframes;
  jalv->buf_size_set = true;
#if USE_JACK_PORT_TYPE_GET_BUFFER_SIZE
  jalv->midi_buf_size = jack_port_type_get_buffer_size(jalv->backend->client,
                                                       JACK_DEFAULT_MIDI_TYPE);
#endif
  jalv_allocate_port_buffers(jalv);
  return 0;
}

/// Jack shutdown callback
static void
jack_shutdown_cb(void* data)
{
  Jalv* const jalv = (Jalv*)data;
  jalv_frontend_close(jalv);
  zix_sem_post(&jalv->done);
}

static void
forge_position(LV2_Atom_Forge* const        forge,
               const JalvURIDs* const       urids,
               const jack_transport_state_t state,
               const jack_position_t        pos)
{
  LV2_Atom_Forge_Frame frame;
  lv2_atom_forge_object(forge, &frame, 0, urids->time_Position);
  lv2_atom_forge_key(forge, urids->time_frame);
  lv2_atom_forge_long(forge, pos.frame);
  lv2_atom_forge_key(forge, urids->time_speed);
  lv2_atom_forge_float(forge, (state == JackTransportRolling) ? 1.0 : 0.0);
  if ((pos.valid & JackPositionBBT)) {
    lv2_atom_forge_key(forge, urids->time_barBeat);
    lv2_atom_forge_float(forge, pos.beat - 1 + (pos.tick / pos.ticks_per_beat));
    lv2_atom_forge_key(forge, urids->time_bar);
    lv2_atom_forge_long(forge, pos.bar - 1);
    lv2_atom_forge_key(forge, urids->time_beatUnit);
    lv2_atom_forge_int(forge, pos.beat_type);
    lv2_atom_forge_key(forge, urids->time_beatsPerBar);
    lv2_atom_forge_float(forge, pos.beats_per_bar);
    lv2_atom_forge_key(forge, urids->time_beatsPerMinute);
    lv2_atom_forge_float(forge, pos.beats_per_minute);
  }
}

static int
process_silent(Jalv* const jalv, const jack_nframes_t nframes)
{
  for (uint32_t p = 0U; p < jalv->num_ports; ++p) {
    JalvPort* const    port  = &jalv->ports[p];
    jack_port_t* const jport = (jack_port_t*)jalv->ports[p].sys_port;
    if (jport && port->flow == FLOW_OUTPUT) {
      void* const buf = jack_port_get_buffer(jport, nframes);
      if (port->type == TYPE_EVENT) {
        jack_midi_clear_buffer(buf);
      } else {
        memset(buf, '\0', nframes * sizeof(float));
      }
    }
  }

  return jalv_bypass(jalv, nframes);
}

static bool
process_transport(Jalv* const                  jalv,
                  const jack_transport_state_t state,
                  const jack_position_t        pos,
                  const jack_nframes_t         nframes)
{
  // If transport state is not as expected, then something has changed
  const bool rolling = state == JackTransportRolling;
  const bool has_bbt = (pos.valid & JackPositionBBT);
  const bool xport_changed =
    (rolling != jalv->rolling || pos.frame != jalv->position ||
     (has_bbt && pos.beats_per_minute != jalv->bpm));

  // Update transport state to expected values for next cycle
  jalv->position = rolling ? pos.frame + nframes : pos.frame;
  jalv->bpm      = has_bbt ? pos.beats_per_minute : jalv->bpm;
  jalv->rolling  = rolling;

  return xport_changed;
}

/// Jack process callback
static REALTIME int
jack_process_cb(jack_nframes_t nframes, void* data)
{
  Jalv* const          jalv        = (Jalv*)data;
  jack_client_t* const client      = jalv->backend->client;
  uint64_t             pos_buf[64] = {0U};
  LV2_Atom* const      lv2_pos     = (LV2_Atom*)pos_buf;

  // If execution is paused, emit silence and return
  if (jalv->run_state == JALV_PAUSED) {
    return process_silent(jalv, nframes);
  }

  // Get transport state and position
  jack_position_t              pos   = {0U};
  const jack_transport_state_t state = jack_transport_query(client, &pos);

  // Check if transport is discontinuous from last time and update state
  const bool xport_changed = process_transport(jalv, state, pos, nframes);
  if (xport_changed) {
    // Build an LV2 position object to report change to plugin
    lv2_atom_forge_set_buffer(&jalv->forge, (uint8_t*)pos_buf, sizeof(pos_buf));
    forge_position(&jalv->forge, &jalv->urids, state, pos);
  }

  // Prepare port buffers
  for (uint32_t p = 0; p < jalv->num_ports; ++p) {
    JalvPort* const port = &jalv->ports[p];
    if (port->sys_port && (port->type == TYPE_AUDIO || port->type == TYPE_CV)) {
      // Connect plugin port directly to Jack port buffer
      lilv_instance_connect_port(
        jalv->instance, p, jack_port_get_buffer(port->sys_port, nframes));
    } else if (port->type == TYPE_EVENT && port->flow == FLOW_INPUT) {
      lv2_evbuf_reset(port->evbuf, true);
      LV2_Evbuf_Iterator iter = lv2_evbuf_begin(port->evbuf);

      if (port->is_primary && xport_changed) {
        // Write new transport position
        lv2_evbuf_write(
          &iter, 0, 0, lv2_pos->type, lv2_pos->size, LV2_ATOM_BODY(lv2_pos));
      }

      if (port->sys_port) {
        // Write Jack MIDI input
        void* buf = jack_port_get_buffer(port->sys_port, nframes);
        for (uint32_t i = 0; i < jack_midi_get_event_count(buf); ++i) {
          jack_midi_event_t ev;
          jack_midi_event_get(&ev, buf, i);
          lv2_evbuf_write(
            &iter, ev.time, 0, jalv->urids.midi_MidiEvent, ev.size, ev.buffer);
        }
      }
    } else if (port->type == TYPE_EVENT) {
      // Clear event output for plugin to write to
      lv2_evbuf_reset(port->evbuf, false);
    }
  }

  // Run plugin for this cycle
  const bool send_ui_updates = jalv_run(jalv, nframes);

  // Deliver MIDI output and UI events
  for (uint32_t p = 0; p < jalv->num_ports; ++p) {
    JalvPort* const port = &jalv->ports[p];
    if (port->flow == FLOW_OUTPUT && port->type == TYPE_CONTROL &&
        port->reports_latency) {
      // Get the latency in frames from the control output truncated to integer
      const float    value = jalv->controls_buf[p];
      const uint32_t frames =
        (value >= 0.0f && value <= max_latency) ? (uint32_t)value : 0U;

      if (jalv->plugin_latency != frames) {
        // Update the cached value and notify the UI if the latency changed
        jalv->plugin_latency = frames;

        const JalvLatencyChange body   = {frames};
        const JalvMessageHeader header = {LATENCY_CHANGE, sizeof(body)};
        jalv_write_split_message(
          jalv->plugin_to_ui, &header, sizeof(header), &body, sizeof(body));
      }
    } else if (port->flow == FLOW_OUTPUT && port->type == TYPE_EVENT) {
      void* buf = NULL;
      if (port->sys_port) {
        buf = jack_port_get_buffer(port->sys_port, nframes);
        jack_midi_clear_buffer(buf);
      }

      for (LV2_Evbuf_Iterator i = lv2_evbuf_begin(port->evbuf);
           lv2_evbuf_is_valid(i);
           i = lv2_evbuf_next(i)) {
        // Get event from LV2 buffer
        uint32_t frames    = 0;
        uint32_t subframes = 0;
        LV2_URID type      = 0;
        uint32_t size      = 0;
        void*    body      = NULL;
        lv2_evbuf_get(i, &frames, &subframes, &type, &size, &body);

        if (buf && type == jalv->urids.midi_MidiEvent) {
          // Write MIDI event to Jack output
          jack_midi_event_write(buf, frames, body, size);
        }

        if (jalv->has_ui) {
          // Forward event to UI
          jalv_write_event(jalv->plugin_to_ui, p, size, type, body);
        }
      }
    } else if (send_ui_updates && port->flow == FLOW_OUTPUT &&
               port->type == TYPE_CONTROL) {
      jalv_write_control(jalv->plugin_to_ui, p, jalv->controls_buf[p]);
    }
  }

  return 0;
}

/// Calculate latency assuming all ports depend on each other
static void
jack_latency_cb(const jack_latency_callback_mode_t mode, void* const data)
{
  const Jalv* const jalv = (const Jalv*)data;
  const PortFlow    flow =
    ((mode == JackCaptureLatency) ? FLOW_INPUT : FLOW_OUTPUT);

  // First calculate the min/max latency of all feeding ports
  uint32_t             ports_found = 0;
  jack_latency_range_t range       = {UINT32_MAX, 0};
  for (uint32_t p = 0; p < jalv->num_ports; ++p) {
    JalvPort* const port = &jalv->ports[p];
    if (port->sys_port && port->flow == flow) {
      jack_latency_range_t r;
      jack_port_get_latency_range(port->sys_port, mode, &r);
      if (r.min < range.min) {
        range.min = r.min;
      }
      if (r.max > range.max) {
        range.max = r.max;
      }
      ++ports_found;
    }
  }

  if (ports_found == 0) {
    range.min = 0;
  }

  // Add the plugin's own latency
  range.min += jalv->plugin_latency;
  range.max += jalv->plugin_latency;

  // Tell Jack about it
  for (uint32_t p = 0; p < jalv->num_ports; ++p) {
    const JalvPort* const port = &jalv->ports[p];
    if (port->sys_port && port->flow == flow) {
      jack_port_set_latency_range(port->sys_port, mode, &range);
    }
  }
}

static jack_client_t*
jack_create_client(Jalv* jalv)
{
  // Determine the name of the JACK client
  char* jack_name = NULL;
  if (jalv->opts.name) {
    // Name given on command line
    jack_name = jalv_strdup(jalv->opts.name);
  } else {
    // Use plugin name
    LilvNode* name = lilv_plugin_get_name(jalv->plugin);
    jack_name      = jalv_strdup(lilv_node_as_string(name));
    lilv_node_free(name);
  }

  // Truncate client name to suit JACK if necessary
  if (strlen(jack_name) >= (unsigned)jack_client_name_size() - 1) {
    jack_name[jack_client_name_size() - 1] = '\0';
  }

  // Connect to JACK
  jack_client_t* const client = jack_client_open(
    jack_name,
    (jalv->opts.name_exact ? JackUseExactName : JackNullOption),
    NULL);

  free(jack_name);

  return client;
}

JalvBackend*
jalv_backend_allocate(void)
{
  return (JalvBackend*)calloc(1, sizeof(JalvBackend));
}

void
jalv_backend_free(JalvBackend* const backend)
{
  free(backend);
}

int
jalv_backend_open(Jalv* jalv)
{
  jack_client_t* const client =
    jalv->backend->client ? jalv->backend->client : jack_create_client(jalv);

  if (!client) {
    return 1;
  }

  jalv_log(JALV_LOG_INFO, "JACK name:    %s\n", jack_get_client_name(client));

  // Set audio engine properties
  jalv->sample_rate   = (float)jack_get_sample_rate(client);
  jalv->block_length  = jack_get_buffer_size(client);
  jalv->midi_buf_size = 4096;
#if USE_JACK_PORT_TYPE_GET_BUFFER_SIZE
  jalv->midi_buf_size =
    jack_port_type_get_buffer_size(client, JACK_DEFAULT_MIDI_TYPE);
#endif

  // Set JACK callbacks
  void* const arg = (void*)jalv;
  jack_set_process_callback(client, &jack_process_cb, arg);
  jack_set_buffer_size_callback(client, &jack_buffer_size_cb, arg);
  jack_on_shutdown(client, &jack_shutdown_cb, arg);
  jack_set_latency_callback(client, &jack_latency_cb, arg);

  jalv->backend->client             = client;
  jalv->backend->is_internal_client = false;
  return 0;
}

void
jalv_backend_close(Jalv* jalv)
{
  if (jalv->backend && jalv->backend->client &&
      !jalv->backend->is_internal_client) {
    jack_client_close(jalv->backend->client);
  }
}

void
jalv_backend_activate(Jalv* jalv)
{
  jack_activate(jalv->backend->client);
}

void
jalv_backend_deactivate(Jalv* jalv)
{
  if (!jalv->backend->is_internal_client && jalv->backend->client) {
    jack_deactivate(jalv->backend->client);
  }
}

void
jalv_backend_activate_port(Jalv* jalv, uint32_t port_index)
{
  jack_client_t*  client = jalv->backend->client;
  JalvPort* const port   = &jalv->ports[port_index];

  const LilvNode* sym = lilv_port_get_symbol(jalv->plugin, port->lilv_port);

  // Connect unsupported ports to NULL (known to be optional by this point)
  if (port->flow == FLOW_UNKNOWN || port->type == TYPE_UNKNOWN) {
    lilv_instance_connect_port(jalv->instance, port_index, NULL);
    return;
  }

  // Build Jack flags for port
  enum JackPortFlags jack_flags =
    (port->flow == FLOW_INPUT) ? JackPortIsInput : JackPortIsOutput;

  // Connect the port based on its type
  switch (port->type) {
  case TYPE_UNKNOWN:
    break;
  case TYPE_CONTROL:
    lilv_instance_connect_port(
      jalv->instance, port_index, &jalv->controls_buf[port_index]);
    break;
  case TYPE_AUDIO:
    port->sys_port = jack_port_register(
      client, lilv_node_as_string(sym), JACK_DEFAULT_AUDIO_TYPE, jack_flags, 0);
    break;
#if USE_JACK_METADATA
  case TYPE_CV:
    port->sys_port = jack_port_register(
      client, lilv_node_as_string(sym), JACK_DEFAULT_AUDIO_TYPE, jack_flags, 0);
    if (port->sys_port) {
      jack_set_property(client,
                        jack_port_uuid(port->sys_port),
                        "http://jackaudio.org/metadata/signal-type",
                        "CV",
                        "text/plain");
    }
    break;
#endif
  case TYPE_EVENT:
    if (lilv_port_supports_event(
          jalv->plugin, port->lilv_port, jalv->nodes.midi_MidiEvent)) {
      port->sys_port = jack_port_register(client,
                                          lilv_node_as_string(sym),
                                          JACK_DEFAULT_MIDI_TYPE,
                                          jack_flags,
                                          0);
    }
    break;
  }

#if USE_JACK_METADATA
  if (port->sys_port) {
    // Set port order to index
    char index_str[16];
    snprintf(index_str, sizeof(index_str), "%u", port_index);
    jack_set_property(client,
                      jack_port_uuid(port->sys_port),
                      "http://jackaudio.org/metadata/order",
                      index_str,
                      "http://www.w3.org/2001/XMLSchema#integer");

    // Set port pretty name to label
    LilvNode* name = lilv_port_get_name(jalv->plugin, port->lilv_port);
    jack_set_property(client,
                      jack_port_uuid(port->sys_port),
                      JACK_METADATA_PRETTY_NAME,
                      lilv_node_as_string(name),
                      "text/plain");
    lilv_node_free(name);
  }
#endif
}

void
jalv_backend_recompute_latencies(Jalv* const jalv)
{
  jack_recompute_total_latencies(jalv->backend->client);
}
