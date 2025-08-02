// Copyright 2007-2025 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "backend.h"

#include "comm.h"
#include "jack_impl.h"
#include "jalv_config.h"
#include "log.h"
#include "lv2_evbuf.h"
#include "process.h"
#include "process_setup.h"
#include "settings.h"
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
#  include <stdio.h>
#endif

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __clang__
#  define REALTIME __attribute__((annotate("realtime")))
#else
#  define REALTIME
#endif

typedef struct {
  jack_position_t        pos;
  jack_transport_state_t state;
  uint64_t               pos_buf[64];
  bool                   changed;
} TransportData;

/// Maximum supported latency in frames (at most 2^24 so all integers work)
static const float max_latency = 16777216.0f;

/// Jack buffer size callback
static int
buffer_size_cb(const jack_nframes_t nframes, void* const data)
{
  JalvBackend* const  backend  = (JalvBackend*)data;
  JalvSettings* const settings = backend->settings;
  JalvProcess* const  proc     = backend->process;

  settings->block_length = nframes;
#if USE_JACK_PORT_TYPE_GET_BUFFER_SIZE
  settings->midi_buf_size =
    jack_port_type_get_buffer_size(backend->client, JACK_DEFAULT_MIDI_TYPE);
#endif
  if (proc->run_state == JALV_RUNNING) {
    jalv_process_activate(proc, backend->urids, proc->instance, settings);
  }
  return 0;
}

/// Jack shutdown callback
static void
shutdown_cb(void* const data)
{
  JalvBackend* const backend = (JalvBackend*)data;
  zix_sem_post(backend->done);
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
  lv2_atom_forge_pop(forge, &frame);
}

static int
process_silent(JalvProcess* const proc, const jack_nframes_t nframes)
{
  for (uint32_t p = 0U; p < proc->num_ports; ++p) {
    const JalvProcessPort* const port  = &proc->ports[p];
    jack_port_t* const           jport = (jack_port_t*)proc->ports[p].sys_port;
    if (jport && port->flow == FLOW_OUTPUT) {
      void* const buf = jack_port_get_buffer(jport, nframes);
      if (port->type == TYPE_EVENT) {
        jack_midi_clear_buffer(buf);
      } else {
        memset(buf, '\0', nframes * sizeof(float));
      }
    }
  }

  return jalv_bypass(proc, nframes);
}

static void
process_transport(JalvPosition* const    transport,
                  TransportData* const   data,
                  LV2_Atom_Forge* const  forge,
                  const JalvURIDs* const urids,
                  jack_client_t* const   client,
                  const jack_nframes_t   nframes)
{
  data->state   = jack_transport_query(client, &data->pos);
  data->changed = false;

  // Get transport state and position
  jack_position_t              pos     = {0U};
  const jack_transport_state_t state   = jack_transport_query(client, &pos);
  const bool                   rolling = state == JackTransportRolling;
  const bool                   has_bbt = (pos.valid & JackPositionBBT);

  // If transport state is not as expected, then something has changed
  data->changed =
    (rolling != transport->rolling || pos.frame != transport->position ||
     (has_bbt && pos.beats_per_minute != transport->bpm));

  // Update transport state to expected values for next cycle
  transport->position = rolling ? pos.frame + nframes : pos.frame;
  transport->bpm      = has_bbt ? pos.beats_per_minute : transport->bpm;
  transport->rolling  = rolling;

  if (data->changed) {
    // Build an LV2 position object to report change to plugin
    lv2_atom_forge_set_buffer(
      forge, (uint8_t*)data->pos_buf, sizeof(data->pos_buf));
    forge_position(forge, urids, state, pos);
  }
}

// Prepare a port before running the plugin for a block
static void
pre_process_port(JalvProcess* const         proc,
                 const JalvURIDs* const     urids,
                 const TransportData* const xport,
                 JalvProcessPort* const     port,
                 const uint32_t             index,
                 const jack_nframes_t       nframes)
{
  if (port->sys_port && (port->type == TYPE_AUDIO || port->type == TYPE_CV)) {
    // Connect plugin port directly to Jack port buffer
    lilv_instance_connect_port(
      proc->instance, index, jack_port_get_buffer(port->sys_port, nframes));
  } else if (port->type == TYPE_EVENT && port->flow == FLOW_INPUT) {
    lv2_evbuf_reset(port->evbuf, true);
    LV2_Evbuf_Iterator iter = lv2_evbuf_begin(port->evbuf);

    if (port->supports_pos && xport->changed) {
      // Write new transport position
      LV2_Atom* const pos = (LV2_Atom*)xport->pos_buf;
      lv2_evbuf_write(&iter, 0, 0, pos->type, pos->size, LV2_ATOM_BODY(pos));
    }

    if (port->sys_port) {
      // Write Jack MIDI input
      void* buf = jack_port_get_buffer(port->sys_port, nframes);
      for (uint32_t i = 0; i < jack_midi_get_event_count(buf); ++i) {
        jack_midi_event_t ev;
        jack_midi_event_get(&ev, buf, i);
        lv2_evbuf_write(
          &iter, ev.time, 0, urids->midi_MidiEvent, ev.size, ev.buffer);
      }
    }
  } else if (port->type == TYPE_EVENT) {
    // Clear event output for plugin to write to
    lv2_evbuf_reset(port->evbuf, false);
  } else if (port->type == TYPE_CONTROL && port->flow == FLOW_INPUT) {
    if (xport->changed && port->is_bpm &&
        (xport->pos.valid & JackPositionBBT)) {
      proc->controls_buf[index] = proc->transport.bpm;
      jalv_write_control(proc->plugin_to_ui, index, proc->transport.bpm);
    }
  }
}

// Process port output after running the plugin for a block
static void
post_process_output_port(JalvProcess* const     proc,
                         const JalvURIDs* const urids,
                         JalvProcessPort* const port,
                         const uint32_t         index,
                         const jack_nframes_t   nframes,
                         const bool             send_updates)
{
  assert(port->flow == FLOW_OUTPUT);

  if (port->type == TYPE_CONTROL && port->reports_latency) {
    // Get the latency in frames from the control output truncated to integer
    const float    value = proc->controls_buf[index];
    const uint32_t frames =
      (value >= 0.0f && value <= max_latency) ? (uint32_t)value : 0U;

    if (proc->plugin_latency != frames) {
      // Update the cached value and notify the UI if the latency changed
      proc->plugin_latency = frames;

      const JalvLatencyChange body   = {frames};
      const JalvMessageHeader header = {LATENCY_CHANGE, sizeof(body)};
      jalv_write_split_message(
        proc->plugin_to_ui, &header, sizeof(header), &body, sizeof(body));
    }
  } else if (port->type == TYPE_EVENT) {
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

      if (buf && type == urids->midi_MidiEvent) {
        // Write MIDI event to Jack output
        jack_midi_event_write(buf, frames, body, size);
      }

      if (proc->has_ui) {
        // Forward event to UI
        jalv_write_event(proc->plugin_to_ui, index, size, type, body);
      }
    }
  } else if (send_updates && port->type == TYPE_CONTROL) {
    jalv_write_control(proc->plugin_to_ui, index, proc->controls_buf[index]);
  }
}

/// Jack process callback
static REALTIME int
process_cb(const jack_nframes_t nframes, void* const data)
{
  JalvBackend* const     backend = (JalvBackend*)data;
  const JalvURIDs* const urids   = backend->urids;
  JalvProcess* const     proc    = backend->process;
  TransportData          xport   = {{0U}, 0, {0U}, false};

  // If execution is paused, emit silence and return
  if (proc->run_state == JALV_PAUSED) {
    return process_silent(proc, nframes);
  }

  // Process and update transport data
  process_transport(
    &proc->transport, &xport, &proc->forge, urids, backend->client, nframes);

  // Prepare port buffers
  for (uint32_t p = 0; p < proc->num_ports; ++p) {
    pre_process_port(proc, urids, &xport, &proc->ports[p], p, nframes);
  }

  // Run plugin for this cycle
  const JalvProcessStatus pst = jalv_run(proc, nframes);

  // Deliver MIDI output and UI events
  for (uint32_t p = 0; p < proc->num_ports; ++p) {
    if (proc->ports[p].flow == FLOW_OUTPUT) {
      post_process_output_port(proc,
                               urids,
                               &proc->ports[p],
                               p,
                               nframes,
                               pst == JALV_PROCESS_SEND_UPDATES);
    }
  }

  return 0;
}

/// Jack latency callback
static void
latency_cb(const jack_latency_callback_mode_t mode, void* const data)
{
  // Calculate latency assuming all ports depend on each other

  const JalvBackend* const backend = (JalvBackend*)data;
  const JalvProcess* const proc    = backend->process;
  const PortFlow           flow =
    ((mode == JackCaptureLatency) ? FLOW_INPUT : FLOW_OUTPUT);

  // First calculate the min/max latency of all feeding ports
  uint32_t             ports_found = 0;
  jack_latency_range_t range       = {UINT32_MAX, 0};
  for (uint32_t p = 0; p < proc->num_ports; ++p) {
    JalvProcessPort* const port = &proc->ports[p];
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
  range.min += proc->plugin_latency;
  range.max += proc->plugin_latency;

  // Tell Jack about it
  for (uint32_t p = 0; p < proc->num_ports; ++p) {
    const JalvProcessPort* const port = &proc->ports[p];
    if (port->sys_port && port->flow == flow) {
      jack_port_set_latency_range(port->sys_port, mode, &range);
    }
  }
}

static jack_client_t*
create_client(const char* const name, const bool exact_name)
{
  char* const jack_name = jalv_strdup(name);

  // Truncate client name to suit JACK if necessary
  if (strlen(jack_name) >= (unsigned)jack_client_name_size() - 1) {
    jack_name[jack_client_name_size() - 1] = '\0';
  }

  // Connect to JACK
  jack_client_t* const client = jack_client_open(
    jack_name, (exact_name ? JackUseExactName : JackNullOption), NULL);

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
jalv_backend_open(JalvBackend* const     backend,
                  const JalvURIDs* const urids,
                  JalvSettings* const    settings,
                  JalvProcess* const     process,
                  ZixSem* const          done,
                  const char* const      name,
                  const bool             exact_name)
{
  jack_client_t* const client =
    backend->client ? backend->client : create_client(name, exact_name);

  if (!client) {
    return 1;
  }

  jalv_log(JALV_LOG_INFO, "JACK name:    %s\n", jack_get_client_name(client));

  // Set audio engine properties
  settings->sample_rate   = (float)jack_get_sample_rate(client);
  settings->block_length  = jack_get_buffer_size(client);
  settings->midi_buf_size = 4096;
#if USE_JACK_PORT_TYPE_GET_BUFFER_SIZE
  settings->midi_buf_size =
    jack_port_type_get_buffer_size(client, JACK_DEFAULT_MIDI_TYPE);
#endif

  // Set JACK callbacks
  void* const arg = (void*)backend;
  jack_set_process_callback(client, &process_cb, arg);
  jack_set_buffer_size_callback(client, &buffer_size_cb, arg);
  jack_on_shutdown(client, &shutdown_cb, arg);
  jack_set_latency_callback(client, &latency_cb, arg);

  backend->urids              = urids;
  backend->settings           = settings;
  backend->process            = process;
  backend->done               = done;
  backend->client             = client;
  backend->is_internal_client = false;
  return 0;
}

void
jalv_backend_close(JalvBackend* const backend)
{
  if (backend && backend->client && !backend->is_internal_client) {
    jack_client_close(backend->client);
  }
}

void
jalv_backend_activate(JalvBackend* const backend)
{
  jack_activate(backend->client);
}

void
jalv_backend_deactivate(JalvBackend* const backend)
{
  if (!backend->is_internal_client && backend->client) {
    jack_deactivate(backend->client);
  }
}

void
jalv_backend_activate_port(JalvBackend* const backend,
                           JalvProcess* const proc,
                           const uint32_t     port_index)
{
  jack_client_t* const   client = backend->client;
  JalvProcessPort* const port   = &proc->ports[port_index];

  // Connect unsupported ports to NULL (known to be optional by this point)
  if (port->flow == FLOW_UNKNOWN || port->type == TYPE_UNKNOWN) {
    lilv_instance_connect_port(proc->instance, port_index, NULL);
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
      proc->instance, port_index, &proc->controls_buf[port_index]);
    break;
  case TYPE_AUDIO:
    port->sys_port = jack_port_register(
      client, port->symbol, JACK_DEFAULT_AUDIO_TYPE, jack_flags, 0);
    break;
#if USE_JACK_METADATA
  case TYPE_CV:
    port->sys_port = jack_port_register(
      client, port->symbol, JACK_DEFAULT_AUDIO_TYPE, jack_flags, 0);
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
    if (port->supports_midi) {
      port->sys_port = jack_port_register(
        client, port->symbol, JACK_DEFAULT_MIDI_TYPE, jack_flags, 0);
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
    if (port->label) {
      jack_set_property(client,
                        jack_port_uuid(port->sys_port),
                        JACK_METADATA_PRETTY_NAME,
                        port->label,
                        "text/plain");
    }
  }
#endif
}

void
jalv_backend_recompute_latencies(JalvBackend* const backend)
{
  jack_recompute_total_latencies(backend->client);
}
