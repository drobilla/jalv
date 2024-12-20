// Copyright 2007-2022 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "backend.h"

#include "frontend.h"
#include "jalv_config.h"
#include "jalv_internal.h"
#include "log.h"
#include "lv2_evbuf.h"
#include "nodes.h"
#include "options.h"
#include "port.h"
#include "types.h"
#include "urids.h"
#include "control.h"

#include "lilv/lilv.h"
#include "lv2/atom/atom.h"
#include "lv2/atom/forge.h"
#include "lv2/urid/urid.h"
#include "zix/sem.h"

#include <jack/jack.h>
#include <jack/midiport.h>
#include <jack/transport.h>
#include <jack/types.h>

#if USE_JACK_METADATA
#  include <jack/metadata.h>
#endif

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef __clang__
#  define REALTIME __attribute__((annotate("realtime")))
#else
#  define REALTIME
#endif

struct JalvBackendImpl {
  jack_client_t* client;             ///< Jack client
  bool           is_internal_client; ///< Running inside jackd
};

/// Internal Jack client initialization entry point
int
jack_initialize(jack_client_t* client, const char* load_init);

/// Internal Jack client finalization entry point
void
jack_finish(void* arg);

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


void print_hex_string(void *p, int size) {
	int i;
	uint32_t *c = (uint32_t *)p;
	for (i=0; i<size>>2; i++) {
	    printf("%d, ", c[i]);
	}
	printf("\n");
}

static double
get_atom_double(Jalv*       jalv,
                uint32_t    ZIX_UNUSED(size),
                LV2_URID    type,
                const void* body)
{
  if (type == jalv->forge.Int || type == jalv->forge.Bool) {
    return *(const int32_t*)body;
  }

  if (type == jalv->forge.Long) {
    return *(const int64_t*)body;
  }

  if (type == jalv->forge.Float) {
    return *(const float*)body;
  }

  if (type == jalv->forge.Double) {
    return *(const double*)body;
  }

  return NAN;
}

static int
patch_set_get(Jalv*                  jalv,
              const LV2_Atom_Object* obj,
              const LV2_Atom_URID**  property,
              const LV2_Atom**       value)
{
  lv2_atom_object_get(obj,
                      jalv->urids.patch_property,
                      (const LV2_Atom*)property,
                      jalv->urids.patch_value,
                      value,
                      0);
  if (!*property) {
    jalv_log(JALV_LOG_WARNING, "patch:Set message with no property\n");
    return 1;
  }

  if ((*property)->atom.type != jalv->forge.URID) {
    jalv_log(JALV_LOG_WARNING, "patch:Set property is not a URID\n");
    return 1;
  }

  return 0;
}

/// Jack process callback
static REALTIME int
jack_process_cb(jack_nframes_t nframes, void* data)
{
  Jalv* const    jalv   = (Jalv*)data;
  jack_client_t* client = jalv->backend->client;

  // Get Jack transport position
  jack_position_t pos;
  const bool      rolling =
    (jack_transport_query(client, &pos) == JackTransportRolling);

  // If transport state is not as expected, then something has changed
  const bool has_bbt = (pos.valid & JackPositionBBT);
  const bool xport_changed =
    (rolling != jalv->rolling || pos.frame != jalv->position ||
    (has_bbt && (float)pos.beats_per_minute != jalv->bpm));

  uint8_t   pos_buf[256];
  LV2_Atom* lv2_pos = (LV2_Atom*)pos_buf;
  if (xport_changed) {
    // Build an LV2 position object to report change to plugin
    lv2_atom_forge_set_buffer(&jalv->forge, pos_buf, sizeof(pos_buf));
    LV2_Atom_Forge*      forge = &jalv->forge;
    LV2_Atom_Forge_Frame frame;
    lv2_atom_forge_object(forge, &frame, 0, jalv->urids.time_Position);
    lv2_atom_forge_key(forge, jalv->urids.time_frame);
    lv2_atom_forge_long(forge, pos.frame);
    lv2_atom_forge_key(forge, jalv->urids.time_speed);
    lv2_atom_forge_float(forge, rolling ? 1.0 : 0.0);
    if (has_bbt) {
      lv2_atom_forge_key(forge, jalv->urids.time_barBeat);
      lv2_atom_forge_float(forge,
                           pos.beat - 1 + (pos.tick / pos.ticks_per_beat));
      lv2_atom_forge_key(forge, jalv->urids.time_bar);
      lv2_atom_forge_long(forge, pos.bar - 1);
      lv2_atom_forge_key(forge, jalv->urids.time_beatUnit);
      lv2_atom_forge_int(forge, pos.beat_type);
      lv2_atom_forge_key(forge, jalv->urids.time_beatsPerBar);
      lv2_atom_forge_float(forge, pos.beats_per_bar);
      lv2_atom_forge_key(forge, jalv->urids.time_beatsPerMinute);
      lv2_atom_forge_float(forge, pos.beats_per_minute);
    }

    jalv_dump_atom(jalv, stdout, "Position", lv2_pos, 32);
  }

  // Update transport state to expected values for next cycle
  jalv->position = rolling ? pos.frame + nframes : pos.frame;
  jalv->bpm      = has_bbt ? pos.beats_per_minute : jalv->bpm;
  jalv->rolling  = rolling;

  switch (jalv->play_state) {
  case JALV_PAUSE_REQUESTED:
    jalv->play_state = JALV_PAUSED;
    zix_sem_post(&jalv->paused);
    break;
  case JALV_PAUSED:
    for (uint32_t p = 0; p < jalv->num_ports; ++p) {
      jack_port_t* jport = jalv->ports[p].sys_port;
      if (jport && jalv->ports[p].flow == FLOW_OUTPUT) {
        void* buf = jack_port_get_buffer(jport, nframes);
        if (jalv->ports[p].type == TYPE_EVENT) {
          jack_midi_clear_buffer(buf);
        } else {
          memset(buf, '\0', nframes * sizeof(float));
        }
      }
    }
    return 0;
  default:
    break;
  }

  // Prepare port buffers
  for (uint32_t p = 0; p < jalv->num_ports; ++p) {
    struct Port* port = &jalv->ports[p];
    if (port->type == TYPE_AUDIO && port->sys_port) {
      // Connect plugin port directly to Jack port buffer
      lilv_instance_connect_port(
        jalv->instance, p, jack_port_get_buffer(port->sys_port, nframes));
#if USE_JACK_METADATA
    } else if (port->type == TYPE_CV && port->sys_port) {
      // Connect plugin port directly to Jack port buffer
      lilv_instance_connect_port(
        jalv->instance, p, jack_port_get_buffer(port->sys_port, nframes));
#endif
    } else if (port->type == TYPE_EVENT && port->flow == FLOW_INPUT) {
      lv2_evbuf_reset(port->evbuf, true);

      // Write transport change event if applicable
      LV2_Evbuf_Iterator iter = lv2_evbuf_begin(port->evbuf);
      if (xport_changed) {
        lv2_evbuf_write(
          &iter, 0, 0, lv2_pos->type, lv2_pos->size, LV2_ATOM_BODY(lv2_pos));
      }

      if (jalv->request_update) {
        // Plugin state has changed, request an update
        const LV2_Atom_Object get = {
          {sizeof(LV2_Atom_Object_Body), jalv->urids.atom_Object},
          {0, jalv->urids.patch_Get}};
        lv2_evbuf_write(
          &iter, 0, 0, get.atom.type, get.atom.size, LV2_ATOM_BODY_CONST(&get));
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
    } else if (jalv->bpm_port_index == port->index && port->flow == FLOW_INPUT && port->type == TYPE_CONTROL) {
      // Send BPM value to designated control port, if any
      if (xport_changed && has_bbt) {
        port->control = jalv->bpm;
        //jalv_print_control(jalv, port, jalv->bpm);
      }
    }
  }
  jalv->request_update = false;

  // Run plugin for this cycle
  const bool send_ui_updates = jalv_run(jalv, nframes);

  // Deliver MIDI output and UI events
  for (uint32_t p = 0; p < jalv->num_ports; ++p) {
    struct Port* const port = &jalv->ports[p];
    if (port->flow != FLOW_OUTPUT) continue;
    if (port->type == TYPE_CONTROL) {
      if (lilv_port_has_property(
          jalv->plugin, port->lilv_port, jalv->nodes.lv2_reportsLatency)) {
        if (jalv->plugin_latency != port->control) {
          jalv->plugin_latency = port->control;
          jack_recompute_total_latencies(client);
        }
      } else if (send_ui_updates) {
        jalv_write_control(jalv, jalv->plugin_to_ui, p, port->control);
        //printf("JACK => WRITE CONTROL port=%d, value=%f\n", p, port->control);
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

        if (buf && type == jalv->urids.midi_MidiEvent) {
          // Write MIDI event to Jack output
          jack_midi_event_write(buf, frames, body, size);
        }
		else if (type == jalv->urids.atom_Object) {
		  const LV2_Atom_Object_Body* obj = (const LV2_Atom_Object_Body*)body;
  		  // Get the atom object containing the atom object body =>
		  const LV2_Atom* atom = (const LV2_Atom*)body;
		  atom--;
	      if (obj->otype == jalv->urids.patch_Set) {
	        //printf("PATCH SET => id=%d , type=%d\n", obj->id, obj->otype);
      		const LV2_Atom_URID* property = NULL;
      		const LV2_Atom*      value    = NULL;
      		if (!patch_set_get(jalv, (const LV2_Atom_Object*)atom, &property, &value)) {
      		  LV2_URID key = property->body;
      		  const double fvalue = get_atom_double(jalv, value->size, value->type, value + 1);
			  //printf("GOT PATCH SET! key=%d => %f\n", key, fvalue);
              ControlID* control = get_property_control(&jalv->controls, key);
              if (control) {
                control->fval =  (float)fvalue;
                // Print property parameter value
                jalv_print_control(jalv, control, fvalue);
                //printf("%s = %f\n", lilv_node_as_string(control->symbol), fvalue);
			  }
      		}
      	  /*} else if (obj->otype == jalv->urids.patch_Put) {
      	    printf("PATCH PUT => size=%d, id=%d , type=%d\n", size, obj->id, obj->otype);
      	    //print_hex_string(body, size);
      	  } else {
      	    printf("ATOM OBJECT => size=%d, id=%d , otype=%d\n", size, obj->id, obj->otype);
		    //print_hex_string(body, size);
      	  */
      	  }
    	}
        if (jalv->has_ui) {
          // Forward event to UI
          jalv_write_event(jalv, jalv->plugin_to_ui, p, size, type, body);
        }
      }
    }
  }
  return 0;
}

/// Calculate latency assuming all ports depend on each other
static void
jack_latency_cb(jack_latency_callback_mode_t mode, void* data)
{
  Jalv* const         jalv = (Jalv*)data;
  const enum PortFlow flow =
    ((mode == JackCaptureLatency) ? FLOW_INPUT : FLOW_OUTPUT);

  // First calculate the min/max latency of all feeding ports
  uint32_t             ports_found = 0;
  jack_latency_range_t range       = {UINT32_MAX, 0};
  for (uint32_t p = 0; p < jalv->num_ports; ++p) {
    struct Port* port = &jalv->ports[p];
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
    struct Port* port = &jalv->ports[p];
    if (port->sys_port && port->flow == flow) {
      jack_port_set_latency_range(port->sys_port, mode, &range);
    }
  }
}

static jack_client_t*
jack_create_client(Jalv* jalv)
{
  jack_client_t* client = NULL;

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
  if (!client) {
    client = jack_client_open(
      jack_name,
      (jalv->opts.name_exact ? JackUseExactName : JackNullOption),
      NULL);
  }

  free(jack_name);

  return client;
}

JalvBackend*
jalv_backend_init(Jalv* jalv)
{
  jack_client_t* const client =
    jalv->backend ? jalv->backend->client : jack_create_client(jalv);

  if (!client) {
    return NULL;
  }

  jalv_log(JALV_LOG_INFO, "JACK Name:    %s\n", jack_get_client_name(client));

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

  if (jalv->backend) {
    /* Internal JACK client, jalv->backend->is_internal_client was already set
       in jack_initialize() when allocating the backend. */
    return jalv->backend;
  }

  // External JACK client, allocate and return opaque backend
  JalvBackend* backend        = (JalvBackend*)calloc(1, sizeof(JalvBackend));
  backend->client             = client;
  backend->is_internal_client = false;
  return backend;
}

void
jalv_backend_close(Jalv* jalv)
{
  if (jalv->backend) {
    if (!jalv->backend->is_internal_client) {
      jack_client_close(jalv->backend->client);
    }

    free(jalv->backend);
    jalv->backend = NULL;
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
  if (jalv->backend && !jalv->backend->is_internal_client) {
    jack_deactivate(jalv->backend->client);
  }
}

void
jalv_backend_activate_port(Jalv* jalv, uint32_t port_index)
{
  jack_client_t*     client = jalv->backend->client;
  struct Port* const port   = &jalv->ports[port_index];

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
  case TYPE_CONTROL:
    lilv_instance_connect_port(jalv->instance, port_index, &port->control);
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
  default:
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

int
jack_initialize(jack_client_t* const client, const char* const load_init)
{
  const size_t args_len = strlen(load_init);
  if (args_len > JACK_LOAD_INIT_LIMIT) {
    jalv_log(JALV_LOG_ERR, "Too many arguments given\n");
    return -1;
  }

  Jalv* const jalv = (Jalv*)calloc(1, sizeof(Jalv));
  if (!jalv) {
    return -1;
  }

  if (!(jalv->backend = (JalvBackend*)calloc(1, sizeof(JalvBackend)))) {
    free(jalv);
    return -1;
  }

  jalv->backend->client             = client;
  jalv->backend->is_internal_client = true;

  // Build full command line with "program" name for building argv
  const size_t cmd_len = strlen("jalv ") + args_len;
  char* const  cmd     = (char*)calloc(cmd_len + 1, 1);
  memcpy(cmd, "jalv ", strlen("jalv ") + 1);
  memcpy(cmd + 5, load_init, args_len + 1);

  // Build argv
  int    argc = 0;
  char** argv = NULL;
  char*  tok  = cmd;
  for (size_t i = 0; i <= cmd_len; ++i) {
    if (isspace(cmd[i]) || !cmd[i]) {
      argv           = (char**)realloc(argv, sizeof(char*) * ++argc);
      cmd[i]         = '\0';
      argv[argc - 1] = tok;
      tok            = cmd + i + 1;
    }
  }

  const int err = jalv_open(jalv, &argc, &argv);
  if (err) {
    jalv_backend_close(jalv);
    free(jalv);
  }

  free(argv);
  free(cmd);
  return err;
}

void
jack_finish(void* const arg)
{
  Jalv* const jalv = (Jalv*)arg;
  if (jalv) {
    if (jalv_close(jalv)) {
      jalv_log(JALV_LOG_ERR, "Failed to close Jalv\n");
    }

    free(jalv);
  }
}
