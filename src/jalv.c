/*
  Copyright 2007-2012 David Robillard <http://drobilla.net>

  Permission to use, copy, modify, and/or distribute this software for any
  purpose with or without fee is hereby granted, provided that the above
  copyright notice and this permission notice appear in all copies.

  THIS SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

#define _POSIX_C_SOURCE 200809L  /* for mkdtemp */
#define _DARWIN_C_SOURCE /* for mkdtemp on OSX */
#include <assert.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef _WIN32
#    include <io.h>  /* for _mktemp */
#endif

#include "jalv_config.h"
#include "jalv_internal.h"

#include <jack/jack.h>
#include <jack/midiport.h>
#ifdef JALV_JACK_SESSION
#    include <jack/session.h>
#endif

#include "lv2/lv2plug.in/ns/ext/atom/atom.h"
#include "lv2/lv2plug.in/ns/ext/buf-size/buf-size.h"
#include "lv2/lv2plug.in/ns/ext/event/event.h"
#include "lv2/lv2plug.in/ns/ext/presets/presets.h"
#include "lv2/lv2plug.in/ns/ext/time/time.h"
#include "lv2/lv2plug.in/ns/ext/uri-map/uri-map.h"
#include "lv2/lv2plug.in/ns/ext/urid/urid.h"
#include "lv2/lv2plug.in/ns/ext/worker/worker.h"
#include "lv2/lv2plug.in/ns/extensions/ui/ui.h"

#include "lilv/lilv.h"

#include "suil/suil.h"

#include "lv2_evbuf.h"
#include "worker.h"

#define NS_RDF "http://www.w3.org/1999/02/22-rdf-syntax-ns#"

#define USTR(str) ((const uint8_t*)str)

#ifndef MIN
#    define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

#ifndef MAX
#    define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

#ifdef __clang__
#    define REALTIME __attribute__((annotate("realtime")))
#else
#    define REALTIME
#endif

ZixSem exit_sem;  /**< Exit semaphore */

LV2_URID
map_uri(LV2_URID_Map_Handle handle,
        const char*         uri)
{
	return symap_map(((Jalv*)handle)->symap, uri);
}

const char*
unmap_uri(LV2_URID_Unmap_Handle handle,
          LV2_URID              urid)
{
	return symap_unmap(((Jalv*)handle)->symap, urid);
}

/**
   Map function for URI map extension.
*/
uint32_t
uri_to_id(LV2_URI_Map_Callback_Data callback_data,
          const char*               map,
          const char*               uri)
{
	return symap_map(((Jalv*)callback_data)->symap, uri);
}

#define NS_EXT "http://lv2plug.in/ns/ext/"

static LV2_URI_Map_Feature uri_map = { NULL, &uri_to_id };

static LV2_Feature uri_map_feature   = { NS_EXT "uri-map", &uri_map };
static LV2_Feature map_feature       = { NS_EXT "urid#map", NULL };
static LV2_Feature unmap_feature     = { NS_EXT "urid#unmap", NULL };
static LV2_Feature instance_feature  = { NS_EXT "instance-access", NULL };
static LV2_Feature make_path_feature = { LV2_STATE__makePath, NULL };
static LV2_Feature schedule_feature  = { LV2_WORKER__schedule, NULL };
static LV2_Feature log_feature       = { LV2_LOG__log, NULL };
static LV2_Feature buf_size_feature  = { LV2_BUF_SIZE__access, NULL };

const LV2_Feature* features[10] = {
	&uri_map_feature, &map_feature, &unmap_feature,
	&instance_feature,
	&make_path_feature,
	&schedule_feature,
	&log_feature,
	&buf_size_feature,
	NULL
};

/** Abort and exit on error */
static void
die(const char* msg)
{
	fprintf(stderr, "%s\n", msg);
	exit(EXIT_FAILURE);
}

/**
   Create a port structure from data description.  This is called before plugin
   and Jack instantiation.  The remaining instance-specific setup
   (e.g. buffers) is done later in expose_port().
*/
void
create_port(Jalv*    jalv,
            uint32_t port_index,
            float    default_value)
{
	struct Port* const port = &jalv->ports[port_index];

	port->lilv_port = lilv_plugin_get_port_by_index(jalv->plugin, port_index);
	port->jack_port = NULL;
	port->evbuf     = NULL;
	port->index     = port_index;
	port->control   = 0.0f;
	port->flow      = FLOW_UNKNOWN;

	/* Get the port symbol for console printing */
	const LilvNode* symbol = lilv_port_get_symbol(jalv->plugin,
	                                              port->lilv_port);

	const bool optional = lilv_port_has_property(jalv->plugin,
	                                             port->lilv_port,
	                                             jalv->nodes.lv2_connectionOptional);

	/* Set the port flow (input or output) */
	if (lilv_port_is_a(jalv->plugin, port->lilv_port, jalv->nodes.lv2_InputPort)) {
		port->flow = FLOW_INPUT;
	} else if (lilv_port_is_a(jalv->plugin, port->lilv_port,
	                          jalv->nodes.lv2_OutputPort)) {
		port->flow = FLOW_OUTPUT;
	} else if (!optional) {
		die("Mandatory port has unknown type (neither input nor output)");
	}

	/* Set control values */
	if (lilv_port_is_a(jalv->plugin, port->lilv_port, jalv->nodes.lv2_ControlPort)) {
		port->type    = TYPE_CONTROL;
		port->control = isnan(default_value) ? 0.0 : default_value;
	} else if (lilv_port_is_a(jalv->plugin, port->lilv_port,
	                          jalv->nodes.lv2_AudioPort)) {
		port->type = TYPE_AUDIO;
	} else if (lilv_port_is_a(jalv->plugin, port->lilv_port,
	                          jalv->nodes.ev_EventPort)) {
		port->type = TYPE_EVENT;
		port->old_api = true;
	} else if (lilv_port_is_a(jalv->plugin, port->lilv_port,
	                          jalv->nodes.atom_AtomPort)) {
		port->type = TYPE_EVENT;
		port->old_api = false;
	} else if (!optional) {
		die("Mandatory port has unknown data type");
	}

	const size_t sym_len = strlen(lilv_node_as_string(symbol));
	if (sym_len > jalv->longest_sym) {
		jalv->longest_sym = sym_len;
	}
}

/**
   Create port structures from data (via create_port()) for all ports.
*/
void
jalv_create_ports(Jalv* jalv)
{
	jalv->num_ports = lilv_plugin_get_num_ports(jalv->plugin);
	jalv->ports     = calloc((size_t)jalv->num_ports, sizeof(struct Port));
	float* default_values = calloc(lilv_plugin_get_num_ports(jalv->plugin),
	                               sizeof(float));
	lilv_plugin_get_port_ranges_float(jalv->plugin, NULL, NULL, default_values);

	for (uint32_t i = 0; i < jalv->num_ports; ++i) {
		create_port(jalv, i, default_values[i]);
	}

	free(default_values);
}

/**
   Allocate port buffers (only necessary for MIDI).
*/
void
jalv_allocate_port_buffers(Jalv* jalv)
{
	for (uint32_t i = 0; i < jalv->num_ports; ++i) {
		struct Port* const port = &jalv->ports[i];
		switch (port->type) {
		case TYPE_EVENT:
			lv2_evbuf_free(port->evbuf);
			port->evbuf = lv2_evbuf_new(
				jalv->midi_buf_size,
				port->old_api ? LV2_EVBUF_EVENT : LV2_EVBUF_ATOM,
				jalv->map.map(jalv->map.handle,
				              lilv_node_as_string(jalv->nodes.atom_Chunk)),
				jalv->map.map(jalv->map.handle,
				              lilv_node_as_string(jalv->nodes.atom_Sequence)));
			lilv_instance_connect_port(
				jalv->instance, i, lv2_evbuf_get_buffer(port->evbuf));
		default: break;
		}
	}
}

/**
   Get a port structure by symbol.

   TODO: Build an index to make this faster, currently O(n) which may be
   a problem when restoring the state of plugins with many ports.
*/
struct Port*
jalv_port_by_symbol(Jalv* jalv, const char* sym)
{
	for (uint32_t i = 0; i < jalv->num_ports; ++i) {
		struct Port* const port     = &jalv->ports[i];
		const LilvNode*    port_sym = lilv_port_get_symbol(jalv->plugin,
		                                                   port->lilv_port);

		if (!strcmp(lilv_node_as_string(port_sym), sym)) {
			return port;
		}
	}

	return NULL;
}

/**
   Expose a port to Jack (if applicable) and connect it to its buffer.
*/
void
activate_port(Jalv*    jalv,
              uint32_t port_index)
{
	struct Port* const port = &jalv->ports[port_index];

	/* Get the port symbol for console printing */
	const LilvNode* symbol = lilv_port_get_symbol(jalv->plugin,
	                                              port->lilv_port);
	const char* symbol_str = lilv_node_as_string(symbol);

	/* Connect unsupported ports to NULL (known to be optional by this point) */
	if (port->flow == FLOW_UNKNOWN || port->type == TYPE_UNKNOWN) {
		lilv_instance_connect_port(jalv->instance, port_index, NULL);
		return;
	}

	/* Build Jack flags for port */
	enum JackPortFlags jack_flags = (port->flow == FLOW_INPUT)
		? JackPortIsInput
		: JackPortIsOutput;

	/* Connect the port based on its type */
	switch (port->type) {
	case TYPE_CONTROL:
		printf("%-*s = %f\n", jalv->longest_sym, symbol_str,
		       jalv->ports[port_index].control);
		lilv_instance_connect_port(jalv->instance, port_index, &port->control);
		break;
	case TYPE_AUDIO:
		port->jack_port = jack_port_register(
			jalv->jack_client, symbol_str,
			JACK_DEFAULT_AUDIO_TYPE, jack_flags, 0);
		break;
	case TYPE_EVENT:
		port->jack_port = jack_port_register(
			jalv->jack_client, symbol_str,
			JACK_DEFAULT_MIDI_TYPE, jack_flags, 0);
		break;
	default:
		break;
	}
}

/** Jack buffer size callback. */
int
jack_buffer_size_cb(jack_nframes_t nframes, void* data)
{
	Jalv* const jalv = (Jalv*)data;
	jalv->block_length = nframes;
	jalv->buf_size_set = true;
#ifdef jack_port_type_get_buffer_size
	jalv->midi_buf_size = jack_port_type_get_buffer_size(
		jalv->jack_client, JACK_DEFAULT_MIDI_TYPE);
#endif
	jalv_allocate_port_buffers(jalv);
	return 0;
}

/** Jack process callback. */
REALTIME int
jack_process_cb(jack_nframes_t nframes, void* data)
{
	Jalv* const jalv = (Jalv*)data;

	/* Get Jack transport position */
	jack_position_t pos;
	const bool rolling = (jack_transport_query(jalv->jack_client, &pos)
	                      == JackTransportRolling);

	/* If transport state is not as expected, then something has changed */
	const bool xport_changed = (rolling != jalv->rolling ||
	                            pos.frame != jalv->position);

	uint8_t   pos_buf[256];
	LV2_Atom* lv2_pos = (LV2_Atom*)pos_buf;
	if (xport_changed) {
		/* Build an LV2 position object to report change to plugin */
		lv2_atom_forge_set_buffer(&jalv->forge, pos_buf, sizeof(pos_buf));
		LV2_Atom_Forge*      forge = &jalv->forge;
		LV2_Atom_Forge_Frame frame;
		lv2_atom_forge_blank(forge, &frame, 1, jalv->urids.time_Position);
		lv2_atom_forge_property_head(forge, jalv->urids.time_frame, 0);
		lv2_atom_forge_long(forge, pos.frame);
		lv2_atom_forge_property_head(forge, jalv->urids.time_speed, 0);
		lv2_atom_forge_float(forge, rolling ? 1.0 : 0.0);
		if (pos.valid & JackPositionBBT) {
			lv2_atom_forge_property_head(forge, jalv->urids.time_barBeat, 0);
			lv2_atom_forge_float(
				forge, pos.beat - 1 + (pos.tick / pos.ticks_per_beat));
			lv2_atom_forge_property_head(forge, jalv->urids.time_bar, 0);
			lv2_atom_forge_float(forge, pos.bar - 1);
			lv2_atom_forge_property_head(forge, jalv->urids.time_beatUnit, 0);
			lv2_atom_forge_float(forge, pos.beat_type);
			lv2_atom_forge_property_head(forge, jalv->urids.time_beatsPerBar, 0);
			lv2_atom_forge_float(forge, pos.beats_per_bar);
			lv2_atom_forge_property_head(forge, jalv->urids.time_beatsPerMinute, 0);
			lv2_atom_forge_float(forge, pos.beats_per_minute);
		}

		if (jalv->opts.dump) {
			char* str = sratom_to_turtle(
				jalv->sratom, &jalv->unmap, "time:", NULL, NULL,
				lv2_pos->type, lv2_pos->size, LV2_ATOM_BODY(lv2_pos));
			printf("\n## Position\n%s\n", str);
			free(str);
		}
	}

	/* Update transport state to expected values for next cycle */
	jalv->position = rolling ? pos.frame + nframes : pos.frame;
	jalv->rolling  = rolling;

	switch (jalv->play_state) {
	case JALV_PAUSE_REQUESTED:
		jalv->play_state = JALV_PAUSED;
		zix_sem_post(&jalv->paused);
		break;
	case JALV_PAUSED:
		for (uint32_t p = 0; p < jalv->num_ports; ++p) {
			jack_port_t* jport = jalv->ports[p].jack_port;
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

	/* Prepare port buffers */
	for (uint32_t p = 0; p < jalv->num_ports; ++p) {
		struct Port* port = &jalv->ports[p];
		if (!port->jack_port)
			continue;

		if (port->type == TYPE_AUDIO) {
			/* Connect plugin port directly to Jack port buffer */
			lilv_instance_connect_port(
				jalv->instance, p,
				jack_port_get_buffer(port->jack_port, nframes));

		} else if (port->type == TYPE_EVENT && port->flow == FLOW_INPUT) {
			lv2_evbuf_reset(port->evbuf, true);

			/* Write transport change event if applicable */
			LV2_Evbuf_Iterator iter = lv2_evbuf_begin(port->evbuf);
			if (xport_changed) {
				lv2_evbuf_write(
					&iter, 0, 0,
					lv2_pos->type, lv2_pos->size, LV2_ATOM_BODY(lv2_pos));
			}

			/* Write Jack MIDI input */
			void* buf = jack_port_get_buffer(port->jack_port, nframes);
			for (uint32_t i = 0; i < jack_midi_get_event_count(buf); ++i) {
				jack_midi_event_t ev;
				jack_midi_event_get(&ev, buf, i);
				lv2_evbuf_write(&iter,
				                ev.time, 0,
				                jalv->midi_event_id,
				                ev.size, ev.buffer);
			}
		} else if (port->type == TYPE_EVENT) {
			/* Clear event output for plugin to write to */
			lv2_evbuf_reset(port->evbuf, false);
		}
	}

	/* Read and apply control change events from UI */
	if (jalv->has_ui) {
		ControlChange ev;
		const size_t  space = jack_ringbuffer_read_space(jalv->ui_events);
		for (size_t i = 0; i < space; i += sizeof(ev) + ev.size) {
			jack_ringbuffer_read(jalv->ui_events, (char*)&ev, sizeof(ev));
			char body[ev.size];
			if (jack_ringbuffer_read(jalv->ui_events, body, ev.size) != ev.size) {
				fprintf(stderr, "error: Error reading from UI ring buffer\n");
				break;
			}
			assert(ev.index < jalv->num_ports);
			struct Port* const port = &jalv->ports[ev.index];
			if (ev.protocol == 0) {
				assert(ev.size == sizeof(float));
				port->control = *(float*)body;
			} else if (ev.protocol == jalv->urids.atom_eventTransfer) {
				LV2_Evbuf_Iterator    i    = lv2_evbuf_end(port->evbuf);
				const LV2_Atom* const atom = (const LV2_Atom*)body;
				lv2_evbuf_write(&i, nframes, 0,
				                atom->type, atom->size, LV2_ATOM_BODY(atom));
			} else {
				fprintf(stderr, "error: Unknown control change protocol %d\n",
				        ev.protocol);
			}
		}
	}

	/* Run plugin for this cycle */
	lilv_instance_run(jalv->instance, nframes);

	/* Process any replies from the worker. */
	jalv_worker_emit_responses(jalv, &jalv->worker);

	/* Notify the plugin the run() cycle is finished */
	if (jalv->worker.iface && jalv->worker.iface->end_run) {
		jalv->worker.iface->end_run(jalv->instance->lv2_handle);
	}

	/* Check if it's time to send updates to the UI */
	jalv->event_delta_t += nframes;
	bool           send_ui_updates = false;
	jack_nframes_t update_frames   = jalv->sample_rate / jalv->ui_update_hz;
	if (jalv->has_ui && (jalv->event_delta_t > update_frames)) {
		send_ui_updates = true;
		jalv->event_delta_t = 0;
	}

	/* Deliver MIDI output and UI events */
	for (uint32_t p = 0; p < jalv->num_ports; ++p) {
		struct Port* const port = &jalv->ports[p];
		if (port->jack_port && port->flow == FLOW_OUTPUT
		    && port->type == TYPE_EVENT) {
			void* buf = jack_port_get_buffer(port->jack_port, nframes);
			jack_midi_clear_buffer(buf);

			for (LV2_Evbuf_Iterator i = lv2_evbuf_begin(port->evbuf);
			     lv2_evbuf_is_valid(i);
			     i = lv2_evbuf_next(i)) {
				uint32_t frames, subframes, type, size;
				uint8_t* data;
				lv2_evbuf_get(i, &frames, &subframes, &type, &size, &data);
				if (type == jalv->midi_event_id) {
					jack_midi_event_write(buf, frames, data, size);
				}

				/* TODO: Be more disciminate about what to send */
				if (jalv->has_ui && !port->old_api) {
					char buf[sizeof(ControlChange) + sizeof(LV2_Atom)];
					ControlChange* ev = (ControlChange*)buf;
					ev->index    = p;
					ev->protocol = jalv->urids.atom_eventTransfer;
					ev->size     = sizeof(LV2_Atom) + size;
					LV2_Atom* atom = (LV2_Atom*)ev->body;
					atom->type = type;
					atom->size = size;
					if (jack_ringbuffer_write_space(jalv->plugin_events)
					    < sizeof(buf) + size) {
						fprintf(stderr, "Plugin => UI buffer overflow!\n");
						break;
					}
					jack_ringbuffer_write(jalv->plugin_events, buf, sizeof(buf));
					/* TODO: race, ensure reader handles this correctly */
					jack_ringbuffer_write(jalv->plugin_events, (void*)data, size);
				}
			}
		} else if (send_ui_updates
		           && port->flow != FLOW_INPUT
		           && port->type == TYPE_CONTROL) {
			char buf[sizeof(ControlChange) + sizeof(float)];
			ControlChange* ev = (ControlChange*)buf;
			ev->index    = p;
			ev->protocol = 0;
			ev->size     = sizeof(float);
			*(float*)ev->body = port->control;
			if (jack_ringbuffer_write(jalv->plugin_events, buf, sizeof(buf))
			    < sizeof(buf)) {
				fprintf(stderr, "Plugin => UI buffer overflow!\n");
			}
		}
	}

	return 0;
}

#ifdef JALV_JACK_SESSION
void
jack_session_cb(jack_session_event_t* event, void* arg)
{
	Jalv* const jalv = (Jalv*)arg;

	#define MAX_CMD_LEN 256
	event->command_line = malloc(MAX_CMD_LEN);
	snprintf(event->command_line, MAX_CMD_LEN, "%s -u %s -l '%s'",
	         jalv->prog_name,
	         event->client_uuid,
	         event->session_dir);

	switch (event->type) {
	case JackSessionSave:
	case JackSessionSaveTemplate:
		jalv_save(jalv, event->session_dir);
		break;
	case JackSessionSaveAndQuit:
		jalv_save(jalv, event->session_dir);
		zix_sem_post(&exit_sem);
		break;
	}

	jack_session_reply(jalv->jack_client, event);
	jack_session_event_free(event);
}
#endif /* JALV_JACK_SESSION */

bool
jalv_ui_is_resizable(Jalv* jalv)
{
	if (!jalv->ui) {
		return false;
	}

	const LilvNode* s   = lilv_ui_get_uri(jalv->ui);
	LilvNode*       p   = lilv_new_uri(jalv->world, LV2_CORE__optionalFeature);
	LilvNode*       fs  = lilv_new_uri(jalv->world, LV2_UI__fixedSize);
	LilvNode*       nrs = lilv_new_uri(jalv->world, LV2_UI__noUserResize);

	LilvNodes* fs_matches = lilv_world_find_nodes(jalv->world, s, p, fs);
	LilvNodes* nrs_matches = lilv_world_find_nodes(jalv->world, s, p, nrs);

	lilv_nodes_free(nrs_matches);
	lilv_nodes_free(fs_matches);
	lilv_node_free(nrs);
	lilv_node_free(fs);
	lilv_node_free(p);

	return !fs_matches && !nrs_matches;
}

void
jalv_ui_write(SuilController controller,
              uint32_t       port_index,
              uint32_t       buffer_size,
              uint32_t       protocol,
              const void*    buffer)
{
	Jalv* const jalv = (Jalv*)controller;
	if (!jalv->has_ui) {
		return;
	}

	if (protocol != 0 && protocol != jalv->urids.atom_eventTransfer) {
		fprintf(stderr, "UI write with unsupported protocol %d (%s)\n",
		        protocol, symap_unmap(jalv->symap, protocol));
		return;
	}

	if (port_index >= jalv->num_ports) {
		fprintf(stderr, "UI write to out of range port index %d\n",
		        port_index);
		return;
	}

	if (jalv->opts.dump && protocol == jalv->urids.atom_eventTransfer) {
		SerdNode s = serd_node_from_string(SERD_BLANK, USTR("msg"));
		SerdNode p = serd_node_from_string(SERD_URI, USTR(NS_RDF "value"));

		const LV2_Atom* atom = (const LV2_Atom*)buffer;
		char*           str  = sratom_to_turtle(
			jalv->sratom, &jalv->unmap, "jalv:", &s, &p,
			atom->type, atom->size, LV2_ATOM_BODY(atom));
		printf("\n## UI => Plugin (%u bytes) ##\n%s\n", atom->size, str);
		free(str);
	}

	char buf[sizeof(ControlChange) + buffer_size];
	ControlChange* ev = (ControlChange*)buf;
	ev->index    = port_index;
	ev->protocol = protocol;
	ev->size     = buffer_size;
	memcpy(ev->body, buffer, buffer_size);
	jack_ringbuffer_write(jalv->ui_events, buf, sizeof(buf));
}

bool
jalv_emit_ui_events(Jalv* jalv)
{
	ControlChange ev;
	const size_t  space = jack_ringbuffer_read_space(jalv->plugin_events);
	for (size_t i = 0; i < space; i += sizeof(ev) + ev.size) {
		jack_ringbuffer_read(jalv->plugin_events, (char*)&ev, sizeof(ev));
		char buf[ev.size];
		jack_ringbuffer_read(jalv->plugin_events, buf, ev.size);

		if (jalv->opts.dump && ev.protocol == jalv->urids.atom_eventTransfer) {
			SerdNode  s    = serd_node_from_string(SERD_BLANK, USTR("msg"));
			SerdNode  p    = serd_node_from_string(SERD_URI, USTR(NS_RDF "value"));
			LV2_Atom* atom = (LV2_Atom*)buf;
			char*     str  = sratom_to_turtle(
				jalv->sratom, &jalv->unmap, "jalv:", &s, &p,
				atom->type, atom->size, LV2_ATOM_BODY(atom));
			printf("\n## Plugin => UI (%u bytes) ##\n%s\n", atom->size, str);
			free(str);
		}

		if (jalv->ui_instance) {
			suil_instance_port_event(jalv->ui_instance, ev.index,
			                         ev.size, ev.protocol, buf);
		} else {
			jalv_ui_port_event(jalv, ev.index, ev.size, ev.protocol, buf);
		}
	}

	return true;
}

static LV2_Buf_Size_Status
jalv_get_sample_count(LV2_Buf_Size_Access_Handle handle,
                      uint32_t*                  min,
                      uint32_t*                  max,
                      uint32_t*                  multiple_of,
                      uint32_t*                  power_of)
{
	Jalv* jalv = (Jalv*)handle;
	// TODO: Is this actually guaranteed with Jack2?
	*min         = jalv->block_length;
	*max         = jalv->block_length;
	*multiple_of = 1;
	*power_of    = 0;
	if (!(jalv->block_length & (jalv->block_length - 1))) {
		// Block length is a power of 2
		*power_of    = 2;
		*multiple_of = 2;
	}
	return 0;
}

static size_t
jalv_get_buf_size(LV2_Buf_Size_Access_Handle handle,
                  LV2_URID                   type,
                  LV2_URID                   subtype)
{
	Jalv* jalv = (Jalv*)handle;
	if (!jalv->buf_size_set) {
		fprintf(stderr, "Buffer size requested but it is not yet known.\n");
		return 0;
	}
	return 0;
}

static void
signal_handler(int ignored)
{
	zix_sem_post(&exit_sem);
}

int
main(int argc, char** argv)
{
	Jalv jalv;
	memset(&jalv, '\0', sizeof(Jalv));
	jalv.prog_name     = argv[0];
	jalv.block_length  = 4096;  // Should be set by jack_buffer_size_cb
	jalv.midi_buf_size = 1024;  // Should be set by jack_buffer_size_cb
	jalv.play_state    = JALV_PAUSED;

	if (jalv_init(&argc, &argv, &jalv.opts)) {
		return EXIT_FAILURE;
	}

	if (jalv.opts.uuid) {
		printf("UUID: %s\n", jalv.opts.uuid);
	}

	jalv.symap = symap_new();
	uri_map.callback_data = &jalv;

	jalv.map.handle  = &jalv;
	jalv.map.map     = map_uri;
	map_feature.data = &jalv.map;

	jalv.unmap.handle  = &jalv;
	jalv.unmap.unmap   = unmap_uri;
	unmap_feature.data = &jalv.unmap;

	lv2_atom_forge_init(&jalv.forge, &jalv.map);

	jalv.sratom = sratom_new(&jalv.map);

	jalv.midi_event_id = uri_to_id(
		&jalv, "http://lv2plug.in/ns/ext/event", LV2_MIDI__MidiEvent);

	jalv.urids.atom_eventTransfer  = symap_map(jalv.symap, LV2_ATOM__eventTransfer);
	jalv.urids.log_Trace           = symap_map(jalv.symap, LV2_LOG__Trace);
	jalv.urids.midi_MidiEvent      = symap_map(jalv.symap, LV2_MIDI__MidiEvent);
	jalv.urids.time_Position       = symap_map(jalv.symap, LV2_TIME__Position);
	jalv.urids.time_bar            = symap_map(jalv.symap, LV2_TIME__bar);
	jalv.urids.time_barBeat        = symap_map(jalv.symap, LV2_TIME__barBeat);
	jalv.urids.time_beatUnit       = symap_map(jalv.symap, LV2_TIME__beatUnit);
	jalv.urids.time_beatsPerBar    = symap_map(jalv.symap, LV2_TIME__beatsPerBar);
	jalv.urids.time_beatsPerMinute = symap_map(jalv.symap, LV2_TIME__beatsPerMinute);
	jalv.urids.time_frame          = symap_map(jalv.symap, LV2_TIME__frame);
	jalv.urids.time_speed          = symap_map(jalv.symap, LV2_TIME__speed);

#ifdef _WIN32
	jalv.temp_dir = jalv_strdup("jalvXXXXXX");
	_mktemp(jalv.temp_dir);
#else
	char* template = jalv_strdup("/tmp/jalv-XXXXXX");
	jalv.temp_dir = jalv_strjoin(mkdtemp(template), "/");
	free(template);
#endif

	LV2_State_Make_Path make_path = { &jalv, jalv_make_path };
	make_path_feature.data = &make_path;

	LV2_Worker_Schedule schedule = { &jalv, jalv_worker_schedule };
	schedule_feature.data = &schedule;

	LV2_Log_Log log = { &jalv, jalv_printf, jalv_vprintf };
	log_feature.data = &log;

	LV2_Buf_Size_Access access = { &jalv, sizeof(LV2_Buf_Size_Access),
	                               jalv_get_sample_count, jalv_get_buf_size };
	buf_size_feature.data = &access;

	zix_sem_init(&exit_sem, 0);
	jalv.done = &exit_sem;

	zix_sem_init(&jalv.paused, 0);
	zix_sem_init(&jalv.worker.sem, 0);

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	/* Find all installed plugins */
	LilvWorld* world = lilv_world_new();
	lilv_world_load_all(world);
	jalv.world = world;
	const LilvPlugins* plugins = lilv_world_get_all_plugins(world);

	/* Cache URIs for concepts we'll use */
	jalv.nodes.atom_AtomPort          = lilv_new_uri(world, LV2_ATOM__AtomPort);
	jalv.nodes.atom_Chunk             = lilv_new_uri(world, LV2_ATOM__Chunk);
	jalv.nodes.atom_Sequence          = lilv_new_uri(world, LV2_ATOM__Sequence);
	jalv.nodes.ev_EventPort           = lilv_new_uri(world, LV2_EVENT__EventPort);
	jalv.nodes.lv2_AudioPort          = lilv_new_uri(world, LV2_CORE__AudioPort);
	jalv.nodes.lv2_ControlPort        = lilv_new_uri(world, LV2_CORE__ControlPort);
	jalv.nodes.lv2_InputPort          = lilv_new_uri(world, LV2_CORE__InputPort);
	jalv.nodes.lv2_OutputPort         = lilv_new_uri(world, LV2_CORE__OutputPort);
	jalv.nodes.lv2_connectionOptional = lilv_new_uri(world, LV2_CORE__connectionOptional);
	jalv.nodes.midi_MidiEvent         = lilv_new_uri(world, LV2_MIDI__MidiEvent);
	jalv.nodes.pset_Preset            = lilv_new_uri(world, LV2_PRESETS__Preset);
	jalv.nodes.rdfs_label             = lilv_new_uri(world, LILV_NS_RDFS "label");
	jalv.nodes.work_interface         = lilv_new_uri(world, LV2_WORKER__interface);
	jalv.nodes.work_schedule          = lilv_new_uri(world, LV2_WORKER__schedule);
	jalv.nodes.end                    = NULL;

	/* Get plugin URI from loaded state or command line */
	LilvState* state      = NULL;
	LilvNode*  plugin_uri = NULL;
	if (jalv.opts.load) {
		struct stat info;
		stat(jalv.opts.load, &info);
		if (S_ISDIR(info.st_mode)) {
			char* path = jalv_strjoin(jalv.opts.load, "/state.ttl");
			state = lilv_state_new_from_file(jalv.world, &jalv.map, NULL, path);
			free(path);
		} else {
			state = lilv_state_new_from_file(jalv.world, &jalv.map, NULL,
			                                 jalv.opts.load);
		}
		if (!state) {
			fprintf(stderr, "Failed to load state from %s\n", jalv.opts.load);
			return EXIT_FAILURE;
		}
		plugin_uri = lilv_node_duplicate(lilv_state_get_plugin_uri(state));
	} else if (argc > 1) {
		plugin_uri = lilv_new_uri(world, argv[argc - 1]);
	} else {
		fprintf(stderr, "Missing plugin URI parameter\n");
		return EXIT_FAILURE;
	}

	/* Find plugin */
	printf("Plugin:       %s\n", lilv_node_as_string(plugin_uri));
	jalv.plugin = lilv_plugins_get_by_uri(plugins, plugin_uri);
	lilv_node_free(plugin_uri);
	if (!jalv.plugin) {
		fprintf(stderr, "Failed to find plugin\n");
		lilv_world_free(world);
		return EXIT_FAILURE;
	}

	/* Get a plugin UI */
	LilvNode*       native_ui_type = jalv_native_ui_type(&jalv);
	const LilvNode* ui_type        = NULL;
	jalv.ui = NULL;
	if (!jalv.opts.generic_ui && native_ui_type) {
		jalv.uis = lilv_plugin_get_uis(jalv.plugin);
		LILV_FOREACH(uis, u, jalv.uis) {
			const LilvUI* this_ui = lilv_uis_get(jalv.uis, u);
			if (lilv_ui_is_supported(
				    this_ui, suil_ui_supported, native_ui_type, &ui_type)) {
				// TODO: Multiple UI support
				jalv.ui = this_ui;
				break;
			}
		}
	}

	/* Create ringbuffers for UI if necessary */
	if (jalv.ui) {
		fprintf(stderr, "UI:           %s\n",
		        lilv_node_as_uri(lilv_ui_get_uri(jalv.ui)));
	} else {
		fprintf(stderr, "No appropriate UI found\n");
	}

	/* Create port structures (jalv.ports) */
	jalv_create_ports(&jalv);

	/* Get the plugin's name */
	LilvNode*   name     = lilv_plugin_get_name(jalv.plugin);
	const char* name_str = lilv_node_as_string(name);

	/* Truncate plugin name to suit JACK (if necessary) */
	char* jack_name = NULL;
	if (strlen(name_str) >= (unsigned)jack_client_name_size() - 1) {
		jack_name = calloc(jack_client_name_size(), 1);
		strncpy(jack_name, name_str, jack_client_name_size() - 1);
	} else {
		jack_name = jalv_strdup(name_str);
	}

	/* Connect to JACK */
	printf("JACK Name:    %s\n", jack_name);
#ifdef JALV_JACK_SESSION
	if (jalv.opts.uuid) {
		jalv.jack_client = jack_client_open(jack_name, JackSessionID, NULL,
		                                    jalv.opts.uuid);
	}
#endif

	if (!jalv.jack_client) {
		jalv.jack_client = jack_client_open(jack_name, JackNullOption, NULL);
	}

	free(jack_name);
	lilv_node_free(name);

	if (!jalv.jack_client)
		die("Failed to connect to JACK.\n");

	jalv.block_length = jack_get_buffer_size(jalv.jack_client);
#ifdef HAVE_JACK_PORT_TYPE_GET_BUFFER_SIZE
	jalv.midi_buf_size = jack_port_type_get_buffer_size(
		jalv.jack_client, JACK_DEFAULT_MIDI_TYPE);
#else
	jalv.midi_buf_size = 4096;
	fprintf(stderr, "warning: No jack_port_type_get_buffer_size.\n");
#endif
	printf("MIDI buffers: %zu bytes\n", jalv.midi_buf_size);

	if (jalv.opts.buffer_size == 0) {
		/* The UI ring is fed by plugin output ports (usually one), and the UI
		   updates roughly once per cycle.  The ring size is a few times the
		   size of the MIDI output to give the UI a chance to keep up.  The UI
		   should be able to keep up with 4 cycles, and tests show this works
		   for me, but this value might need increasing to avoid overflows.
		*/
		jalv.opts.buffer_size = jalv.midi_buf_size * 4;
	}

	/* Calculate theoretical UI update frequency. */
	jalv.sample_rate  = jack_get_sample_rate(jalv.jack_client);
	jalv.ui_update_hz = (double)jalv.sample_rate / jalv.midi_buf_size * 2.0;

	/* The UI can only go so fast, clamp to reasonable limits */
	jalv.ui_update_hz     = MIN(60, jalv.ui_update_hz);
	jalv.opts.buffer_size = MAX(4096, jalv.opts.buffer_size);
	fprintf(stderr, "Comm buffers: %d bytes\n", jalv.opts.buffer_size);
	fprintf(stderr, "Update rate:  %d Hz\n", jalv.ui_update_hz);

	/* Create Plugin <=> UI communication buffers */
	jalv.ui_events     = jack_ringbuffer_create(jalv.opts.buffer_size);
	jalv.plugin_events = jack_ringbuffer_create(jalv.opts.buffer_size);
	jack_ringbuffer_mlock(jalv.ui_events);
	jack_ringbuffer_mlock(jalv.plugin_events);

	/* Instantiate the plugin */
	jalv.instance = lilv_plugin_instantiate(
		jalv.plugin, jalv.sample_rate, features);
	if (!jalv.instance) {
		die("Failed to instantiate plugin.\n");
	}

	fprintf(stderr, "\n");
	if (!jalv.buf_size_set) {
		jalv_allocate_port_buffers(&jalv);
	}

	/* Create thread and ringbuffers for worker if necessary */
	if (lilv_plugin_has_feature(jalv.plugin, jalv.nodes.work_schedule)
	    && lilv_plugin_has_extension_data(jalv.plugin, jalv.nodes.work_interface)) {
		jalv_worker_init(
			&jalv, &jalv.worker,
			(LV2_Worker_Interface*)lilv_instance_get_extension_data(
				jalv.instance, LV2_WORKER__interface));
	}

	/* Apply loaded state to plugin instance if necessary */
	if (state) {
		jalv_apply_state(&jalv, state);
	}

	/* Set instance for instance-access extension */
	instance_feature.data = lilv_instance_get_handle(jalv.instance);

	/* Set Jack callbacks */
	jack_set_process_callback(jalv.jack_client,
	                          &jack_process_cb, (void*)(&jalv));
	jack_set_buffer_size_callback(jalv.jack_client,
	                              &jack_buffer_size_cb, (void*)(&jalv));
#ifdef JALV_JACK_SESSION
	jack_set_session_callback(jalv.jack_client,
	                          &jack_session_cb, (void*)(&jalv));
#endif

	/* Create Jack ports and connect plugin ports to buffers */
	for (uint32_t i = 0; i < jalv.num_ports; ++i) {
		activate_port(&jalv, i);
	}

	/* Activate plugin */
	lilv_instance_activate(jalv.instance);

	/* Activate Jack */
	jack_activate(jalv.jack_client);
	jalv.sample_rate = jack_get_sample_rate(jalv.jack_client);
	jalv.play_state  = JALV_RUNNING;

	SuilHost* ui_host = NULL;
	if (jalv.ui) {
		/* Instantiate UI */
		ui_host = suil_host_new(jalv_ui_write, NULL, NULL, NULL);

		jalv.has_ui = true;
		jalv.ui_instance = suil_instance_new(
			ui_host,
			&jalv,
			lilv_node_as_uri(native_ui_type),
			lilv_node_as_uri(lilv_plugin_get_uri(jalv.plugin)),
			lilv_node_as_uri(lilv_ui_get_uri(jalv.ui)),
			lilv_node_as_uri(ui_type),
			lilv_uri_to_path(lilv_node_as_uri(lilv_ui_get_bundle_uri(jalv.ui))),
			lilv_uri_to_path(lilv_node_as_uri(lilv_ui_get_binary_uri(jalv.ui))),
			features);

		if (!jalv.ui_instance) {
			die("Failed to instantiate plugin.\n");
		}

		/* Set initial control values for UI */
		for (uint32_t i = 0; i < jalv.num_ports; ++i) {
			if (jalv.ports[i].type == TYPE_CONTROL) {
				suil_instance_port_event(jalv.ui_instance, i,
				                         sizeof(float), 0,
				                         &jalv.ports[i].control);
			}
		}
	}

	/* Run UI (or prompt at console) */
	jalv_open_ui(&jalv, jalv.ui_instance);

	/* Wait for finish signal from UI or signal handler */
	zix_sem_wait(&exit_sem);
	jalv.exit = true;

	fprintf(stderr, "Exiting...\n");

	/* Terminate the worker */
	jalv_worker_finish(&jalv.worker);

	/* Deactivate JACK */
	jack_deactivate(jalv.jack_client);
	for (uint32_t i = 0; i < jalv.num_ports; ++i) {
		if (jalv.ports[i].evbuf) {
			lv2_evbuf_free(jalv.ports[i].evbuf);
		}
	}
	jack_client_close(jalv.jack_client);

	/* Deactivate plugin */
	suil_instance_free(jalv.ui_instance);
	lilv_instance_deactivate(jalv.instance);
	lilv_instance_free(jalv.instance);

	/* Clean up */
	free(jalv.ports);
	jack_ringbuffer_free(jalv.ui_events);
	jack_ringbuffer_free(jalv.plugin_events);
	lilv_node_free(native_ui_type);
	for (LilvNode** n = (LilvNode**)&jalv.nodes; *n; ++n) {
		lilv_node_free(*n);
	}
	symap_free(jalv.symap);
	suil_host_free(ui_host);
	sratom_free(jalv.sratom);
	lilv_uis_free(jalv.uis);
	lilv_world_free(world);

	zix_sem_destroy(&exit_sem);

	remove(jalv.temp_dir);
	free(jalv.temp_dir);

	return 0;
}
