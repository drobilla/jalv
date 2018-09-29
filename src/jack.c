/*
  Copyright 2007-2016 David Robillard <http://drobilla.net>

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

#include <jack/jack.h>
#include <jack/midiport.h>
#ifdef JALV_JACK_SESSION
#    include <jack/session.h>
#endif
#ifdef HAVE_JACK_METADATA
#    include <jack/metadata.h>
#endif

#include "jalv_internal.h"
#include "worker.h"

struct JalvBackend {
	jack_client_t* client;  ///< Jack client
};

/** Jack buffer size callback. */
static int
jack_buffer_size_cb(jack_nframes_t nframes, void* data)
{
	Jalv* const jalv = (Jalv*)data;
	jalv->block_length = nframes;
	jalv->buf_size_set = true;
#ifdef HAVE_JACK_PORT_TYPE_GET_BUFFER_SIZE
	jalv->midi_buf_size = jack_port_type_get_buffer_size(
		jalv->backend->client, JACK_DEFAULT_MIDI_TYPE);
#endif
	jalv_allocate_port_buffers(jalv);
	return 0;
}

/** Jack shutdown callback. */
static void
jack_shutdown_cb(void* data)
{
	Jalv* const jalv = (Jalv*)data;
	jalv_close_ui(jalv);
	zix_sem_post(&jalv->done);
}

/** Jack process callback. */
static REALTIME int
jack_process_cb(jack_nframes_t nframes, void* data)
{
	Jalv* const    jalv   = (Jalv*)data;
	jack_client_t* client = jalv->backend->client;

	/* Get Jack transport position */
	jack_position_t pos;
	const bool rolling = (jack_transport_query(client, &pos)
	                      == JackTransportRolling);

	/* If transport state is not as expected, then something has changed */
	const bool xport_changed = (rolling != jalv->rolling ||
	                            pos.frame != jalv->position ||
	                            pos.beats_per_minute != jalv->bpm);

	uint8_t   pos_buf[256];
	LV2_Atom* lv2_pos = (LV2_Atom*)pos_buf;
	if (xport_changed) {
		/* Build an LV2 position object to report change to plugin */
		lv2_atom_forge_set_buffer(&jalv->forge, pos_buf, sizeof(pos_buf));
		LV2_Atom_Forge*      forge = &jalv->forge;
		LV2_Atom_Forge_Frame frame;
		lv2_atom_forge_object(forge, &frame, 0, jalv->urids.time_Position);
		lv2_atom_forge_key(forge, jalv->urids.time_frame);
		lv2_atom_forge_long(forge, pos.frame);
		lv2_atom_forge_key(forge, jalv->urids.time_speed);
		lv2_atom_forge_float(forge, rolling ? 1.0 : 0.0);
		if (pos.valid & JackPositionBBT) {
			lv2_atom_forge_key(forge, jalv->urids.time_barBeat);
			lv2_atom_forge_float(
				forge, pos.beat - 1 + (pos.tick / pos.ticks_per_beat));
			lv2_atom_forge_key(forge, jalv->urids.time_bar);
			lv2_atom_forge_long(forge, pos.bar - 1);
			lv2_atom_forge_key(forge, jalv->urids.time_beatUnit);
			lv2_atom_forge_int(forge, pos.beat_type);
			lv2_atom_forge_key(forge, jalv->urids.time_beatsPerBar);
			lv2_atom_forge_float(forge, pos.beats_per_bar);
			lv2_atom_forge_key(forge, jalv->urids.time_beatsPerMinute);
			lv2_atom_forge_float(forge, pos.beats_per_minute);
		}

		if (jalv->opts.dump) {
			char* str = sratom_to_turtle(
				jalv->sratom, &jalv->unmap, "time:", NULL, NULL,
				lv2_pos->type, lv2_pos->size, LV2_ATOM_BODY(lv2_pos));
			jalv_ansi_start(stdout, 36);
			printf("\n## Position ##\n%s\n", str);
			jalv_ansi_reset(stdout);
			free(str);
		}
	}

	/* Update transport state to expected values for next cycle */
	jalv->position = rolling ? pos.frame + nframes : pos.frame;
	jalv->bpm      = pos.beats_per_minute;
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

	/* Prepare port buffers */
	for (uint32_t p = 0; p < jalv->num_ports; ++p) {
		struct Port* port = &jalv->ports[p];
		if (port->type == TYPE_AUDIO && port->sys_port) {
			/* Connect plugin port directly to Jack port buffer */
			lilv_instance_connect_port(
				jalv->instance, p,
				jack_port_get_buffer(port->sys_port, nframes));
#ifdef HAVE_JACK_METADATA
		} else if (port->type == TYPE_CV && port->sys_port) {
			/* Connect plugin port directly to Jack port buffer */
			lilv_instance_connect_port(
				jalv->instance, p,
				jack_port_get_buffer(port->sys_port, nframes));
#endif
		} else if (port->type == TYPE_EVENT && port->flow == FLOW_INPUT) {
			lv2_evbuf_reset(port->evbuf, true);

			/* Write transport change event if applicable */
			LV2_Evbuf_Iterator iter = lv2_evbuf_begin(port->evbuf);
			if (xport_changed) {
				lv2_evbuf_write(&iter, 0, 0,
				                lv2_pos->type, lv2_pos->size,
				                (const uint8_t*)LV2_ATOM_BODY(lv2_pos));
			}

			if (jalv->request_update) {
				/* Plugin state has changed, request an update */
				const LV2_Atom_Object get = {
					{ sizeof(LV2_Atom_Object_Body), jalv->urids.atom_Object },
					{ 0, jalv->urids.patch_Get } };
				lv2_evbuf_write(&iter, 0, 0,
				                get.atom.type, get.atom.size,
				                (const uint8_t*)LV2_ATOM_BODY(&get));
			}

			if (port->sys_port) {
				/* Write Jack MIDI input */
				void* buf = jack_port_get_buffer(port->sys_port, nframes);
				for (uint32_t i = 0; i < jack_midi_get_event_count(buf); ++i) {
					jack_midi_event_t ev;
					jack_midi_event_get(&ev, buf, i);
					lv2_evbuf_write(&iter,
					                ev.time, 0,
					                jalv->urids.midi_MidiEvent,
					                ev.size, ev.buffer);
				}
			}
		} else if (port->type == TYPE_EVENT) {
			/* Clear event output for plugin to write to */
			lv2_evbuf_reset(port->evbuf, false);
		}
	}
	jalv->request_update = false;

	/* Run plugin for this cycle */
	const bool send_ui_updates = jalv_run(jalv, nframes);

	/* Deliver MIDI output and UI events */
	for (uint32_t p = 0; p < jalv->num_ports; ++p) {
		struct Port* const port = &jalv->ports[p];
		if (port->flow == FLOW_OUTPUT && port->type == TYPE_CONTROL &&
		    lilv_port_has_property(jalv->plugin, port->lilv_port,
		                           jalv->nodes.lv2_reportsLatency)) {
			if (jalv->plugin_latency != port->control) {
				jalv->plugin_latency = port->control;
				jack_recompute_total_latencies(client);
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
				uint32_t frames, subframes, type, size;
				uint8_t* body;
				lv2_evbuf_get(i, &frames, &subframes, &type, &size, &body);

				if (buf && type == jalv->urids.midi_MidiEvent) {
					// Write MIDI event to Jack output
					jack_midi_event_write(buf, frames, body, size);
				}

				if (jalv->has_ui) {
					// Forward event to UI
					jalv_send_to_ui(jalv, p, type, size, body);
				}
			}
		} else if (send_ui_updates &&
		           port->flow == FLOW_OUTPUT && port->type == TYPE_CONTROL) {
			char buf[sizeof(ControlChange) + sizeof(float)];
			ControlChange* ev = (ControlChange*)buf;
			ev->index    = p;
			ev->protocol = 0;
			ev->size     = sizeof(float);
			*(float*)ev->body = port->control;
			if (zix_ring_write(jalv->plugin_events, buf, sizeof(buf))
			    < sizeof(buf)) {
				fprintf(stderr, "Plugin => UI buffer overflow!\n");
			}
		}
	}

	return 0;
}

/** Calculate latency assuming all ports depend on each other. */
static void
jack_latency_cb(jack_latency_callback_mode_t mode, void* data)
{
	Jalv* const         jalv = (Jalv*)data;
	const enum PortFlow flow = ((mode == JackCaptureLatency)
	                            ? FLOW_INPUT : FLOW_OUTPUT);

	/* First calculate the min/max latency of all feeding ports */
	uint32_t             ports_found = 0;
	jack_latency_range_t range       = { UINT32_MAX, 0 };
	for (uint32_t p = 0; p < jalv->num_ports; ++p) {
		struct Port* port = &jalv->ports[p];
		if (port->sys_port && port->flow == flow) {
			jack_latency_range_t r;
			jack_port_get_latency_range(port->sys_port, mode, &r);
			if (r.min < range.min) { range.min = r.min; }
			if (r.max > range.max) { range.max = r.max; }
			++ports_found;
		}
	}

	if (ports_found == 0) {
		range.min = 0;
	}

	/* Add the plugin's own latency */
	range.min += jalv->plugin_latency;
	range.max += jalv->plugin_latency;

	/* Tell Jack about it */
	for (uint32_t p = 0; p < jalv->num_ports; ++p) {
		struct Port* port = &jalv->ports[p];
		if (port->sys_port && port->flow == flow) {
			jack_port_set_latency_range(port->sys_port, mode, &range);
		}
	}
}

#ifdef JALV_JACK_SESSION
static void
jack_session_cb(jack_session_event_t* event, void* arg)
{
	Jalv* const jalv = (Jalv*)arg;

	#define MAX_CMD_LEN 256
	event->command_line = (char*)malloc(MAX_CMD_LEN);
	snprintf(event->command_line, MAX_CMD_LEN, "%s -u %s -l \"${SESSION_DIR}\"",
	         jalv->prog_name,
	         event->client_uuid);

	switch (event->type) {
	case JackSessionSave:
	case JackSessionSaveTemplate:
		jalv_save(jalv, event->session_dir);
		break;
	case JackSessionSaveAndQuit:
		jalv_save(jalv, event->session_dir);
		jalv_close_ui(jalv);
		break;
	}

	jack_session_reply(jalv->backend->client, event);
	jack_session_event_free(event);
}
#endif /* JALV_JACK_SESSION */

static jack_client_t*
jack_create_client(Jalv* jalv)
{
	jack_client_t* client = NULL;

	/* Determine the name of the JACK client */
	char* jack_name = NULL;
	if (jalv->opts.name) {
		/* Name given on command line */
		jack_name = jalv_strdup(jalv->opts.name);
	} else {
		/* Use plugin name */
		LilvNode* name = lilv_plugin_get_name(jalv->plugin);
		jack_name = jalv_strdup(lilv_node_as_string(name));
		lilv_node_free(name);
	}

	/* Truncate client name to suit JACK if necessary */
	if (strlen(jack_name) >= (unsigned)jack_client_name_size() - 1) {
		jack_name[jack_client_name_size() - 1] = '\0';
	}

	/* Connect to JACK */
#ifdef JALV_JACK_SESSION
	if (jalv->opts.uuid) {
		client = jack_client_open(
			jack_name,
			(jack_options_t)(JackSessionID |
			                 (jalv->opts.name_exact ? JackUseExactName : 0)),
			NULL,
			jalv->opts.uuid);
	}
#endif

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

	printf("JACK Name:    %s\n", jack_get_client_name(client));

	/* Set audio engine properties */
	jalv->sample_rate   = jack_get_sample_rate(client);
	jalv->block_length  = jack_get_buffer_size(client);
	jalv->midi_buf_size = 4096;
#ifdef HAVE_JACK_PORT_TYPE_GET_BUFFER_SIZE
	jalv->midi_buf_size = jack_port_type_get_buffer_size(
		client, JACK_DEFAULT_MIDI_TYPE);
#endif

	/* Set JACK callbacks */
	void* const arg = (void*)jalv;
	jack_set_process_callback(client, &jack_process_cb, arg);
	jack_set_buffer_size_callback(client, &jack_buffer_size_cb, arg);
	jack_on_shutdown(client, &jack_shutdown_cb, arg);
	jack_set_latency_callback(client, &jack_latency_cb, arg);
#ifdef JALV_JACK_SESSION
	jack_set_session_callback(client, &jack_session_cb, arg);
#endif

	/* Allocate and return opaque backend */
	JalvBackend* backend = (JalvBackend*)calloc(1, sizeof(JalvBackend));
	backend->client = client;
	return backend;
}

void
jalv_backend_close(Jalv* jalv)
{
	if (jalv->backend) {
		jack_client_close(jalv->backend->client);
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
	if (jalv->backend) {
		jack_deactivate(jalv->backend->client);
	}
}

void
jalv_backend_activate_port(Jalv* jalv, uint32_t port_index)
{
	jack_client_t*     client = jalv->backend->client;
	struct Port* const port   = &jalv->ports[port_index];

	const LilvNode* sym = lilv_port_get_symbol(jalv->plugin, port->lilv_port);

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
		lilv_instance_connect_port(jalv->instance, port_index, &port->control);
		break;
	case TYPE_AUDIO:
		port->sys_port = jack_port_register(
			client, lilv_node_as_string(sym),
			JACK_DEFAULT_AUDIO_TYPE, jack_flags, 0);
		break;
#ifdef HAVE_JACK_METADATA
	case TYPE_CV:
		port->sys_port = jack_port_register(
			client, lilv_node_as_string(sym),
			JACK_DEFAULT_AUDIO_TYPE, jack_flags, 0);
		if (port->sys_port) {
			jack_set_property(client, jack_port_uuid(port->sys_port),
			                  "http://jackaudio.org/metadata/signal-type", "CV",
			                  "text/plain");
		}
		break;
#endif
	case TYPE_EVENT:
		if (lilv_port_supports_event(
			    jalv->plugin, port->lilv_port, jalv->nodes.midi_MidiEvent)) {
			port->sys_port = jack_port_register(
				client, lilv_node_as_string(sym),
				JACK_DEFAULT_MIDI_TYPE, jack_flags, 0);
		}
		break;
	default:
		break;
	}

#ifdef HAVE_JACK_METADATA
	if (port->sys_port) {
		// Set port order to index
		char index_str[16];
		snprintf(index_str, sizeof(index_str), "%d", port_index);
		jack_set_property(client, jack_port_uuid(port->sys_port),
		                  "http://jackaudio.org/metadata/order", index_str,
		                  "http://www.w3.org/2001/XMLSchema#integer");

		// Set port pretty name to label
		LilvNode* name = lilv_port_get_name(jalv->plugin, port->lilv_port);
		jack_set_property(client, jack_port_uuid(port->sys_port),
		                  JACK_METADATA_PRETTY_NAME, lilv_node_as_string(name),
		                  "text/plain");
		lilv_node_free(name);
	}
#endif
}
