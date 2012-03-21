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

#include "jalv_config.h"
#include "jalv_internal.h"

#include <jack/jack.h>
#include <jack/midiport.h>
#ifdef JALV_JACK_SESSION
#    include <jack/session.h>
#endif

#include "lv2/lv2plug.in/ns/ext/atom/atom.h"
#include "lv2/lv2plug.in/ns/ext/time/time.h"
#include "lv2/lv2plug.in/ns/ext/uri-map/uri-map.h"
#include "lv2/lv2plug.in/ns/ext/urid/urid.h"

#include "lilv/lilv.h"

#include "suil/suil.h"

#include "lv2_evbuf.h"

#define NS_ATOM "http://lv2plug.in/ns/ext/atom#"
#define NS_MIDI "http://lv2plug.in/ns/ext/midi#"
#define NS_PSET "http://lv2plug.in/ns/ext/presets#"
#define NS_RDF  "http://www.w3.org/1999/02/22-rdf-syntax-ns#"
#define NS_UI   "http://lv2plug.in/ns/extensions/ui#"

#define USTR(str) ((const uint8_t*)str)

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

const LV2_Feature* features[7] = {
	&uri_map_feature, &map_feature, &unmap_feature,
	&instance_feature,
	&make_path_feature,
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
create_port(Jalv*    host,
            uint32_t port_index,
            float    default_value)
{
	struct Port* const port = &host->ports[port_index];

	port->lilv_port = lilv_plugin_get_port_by_index(host->plugin, port_index);
	port->jack_port = NULL;
	port->evbuf     = NULL;
	port->index     = port_index;
	port->control   = 0.0f;
	port->flow      = FLOW_UNKNOWN;

	/* Get the port symbol for console printing */
	const LilvNode* symbol = lilv_port_get_symbol(host->plugin,
	                                              port->lilv_port);

	const bool optional = lilv_port_has_property(host->plugin,
	                                             port->lilv_port,
	                                             host->optional);

	/* Set the port flow (input or output) */
	if (lilv_port_is_a(host->plugin, port->lilv_port, host->input_class)) {
		port->flow = FLOW_INPUT;
	} else if (lilv_port_is_a(host->plugin, port->lilv_port,
	                          host->output_class)) {
		port->flow = FLOW_OUTPUT;
	} else if (!optional) {
		die("Mandatory port has unknown type (neither input nor output)");
	}

	/* Set control values */
	if (lilv_port_is_a(host->plugin, port->lilv_port, host->control_class)) {
		port->type    = TYPE_CONTROL;
		port->control = isnan(default_value) ? 0.0 : default_value;
	} else if (lilv_port_is_a(host->plugin, port->lilv_port,
	                          host->audio_class)) {
		port->type = TYPE_AUDIO;
	} else if (lilv_port_is_a(host->plugin, port->lilv_port,
	                          host->event_class)) {
		port->type = TYPE_EVENT;
		port->old_api = true;
	} else if (lilv_port_is_a(host->plugin, port->lilv_port,
	                          host->msg_port_class)) {
		port->type = TYPE_EVENT;
		port->old_api = false;
	} else if (!optional) {
		die("Mandatory port has unknown data type");
	}

	const size_t sym_len = strlen(lilv_node_as_string(symbol));
	if (sym_len > host->longest_sym) {
		host->longest_sym = sym_len;
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
				              lilv_node_as_string(jalv->chunk_class)),
				jalv->map.map(jalv->map.handle,
				              lilv_node_as_string(jalv->seq_class)));
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
activate_port(Jalv*    host,
              uint32_t port_index)
{
	struct Port* const port = &host->ports[port_index];

	/* Get the port symbol for console printing */
	const LilvNode* symbol = lilv_port_get_symbol(host->plugin,
	                                              port->lilv_port);
	const char* symbol_str = lilv_node_as_string(symbol);

	/* Connect unsupported ports to NULL (known to be optional by this point) */
	if (port->flow == FLOW_UNKNOWN || port->type == TYPE_UNKNOWN) {
		lilv_instance_connect_port(host->instance, port_index, NULL);
		return;
	}

	/* Build Jack flags for port */
	enum JackPortFlags jack_flags = (port->flow == FLOW_INPUT)
		? JackPortIsInput
		: JackPortIsOutput;

	/* Connect the port based on its type */
	switch (port->type) {
	case TYPE_CONTROL:
		printf("%-*s = %f\n", host->longest_sym, symbol_str,
		       host->ports[port_index].control);
		lilv_instance_connect_port(host->instance, port_index, &port->control);
		break;
	case TYPE_AUDIO:
		port->jack_port = jack_port_register(
			host->jack_client, symbol_str,
			JACK_DEFAULT_AUDIO_TYPE, jack_flags, 0);
		break;
	case TYPE_EVENT:
		port->jack_port = jack_port_register(
			host->jack_client, symbol_str,
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
	Jalv* const host = (Jalv*)data;
	host->buf_size_set = true;
#ifdef jack_port_type_get_buffer_size
	host->midi_buf_size = jack_port_type_get_buffer_size(
		host->jack_client, JACK_DEFAULT_MIDI_TYPE);
#endif
	jalv_allocate_port_buffers(host);
	return 0;
}

/** Jack process callback. */
int
jack_process_cb(jack_nframes_t nframes, void* data)
{
	Jalv* const host = (Jalv*)data;

#if 0
	jack_position_t pos;
	double          speed = 0.0;
	if (jack_transport_query(host->jack_client, &pos) == JackTransportRolling) {
		speed = 1.0;
	}

	if (pos.valid & JackPositionBBT) {
		uint8_t buf[1024];
		lv2_atom_forge_set_buffer(&host->forge, buf, sizeof(buf));
		LV2_Atom_Forge*      forge = &host->forge;
		LV2_Atom_Forge_Frame frame;
		lv2_atom_forge_blank(forge, &frame, 1, host->urids.time_Position);
		lv2_atom_forge_property_head(forge, host->urids.time_barBeat, 0);
		lv2_atom_forge_float(forge, pos.beat - 1 + (pos.tick / (float)pos.ticks_per_beat));
		lv2_atom_forge_property_head(forge, host->urids.time_bar, 0);
		lv2_atom_forge_float(forge, pos.bar - 1);
		lv2_atom_forge_property_head(forge, host->urids.time_beatUnit, 0);
		lv2_atom_forge_float(forge, pos.beat_type);
		lv2_atom_forge_property_head(forge, host->urids.time_beatsPerBar, 0);
		lv2_atom_forge_float(forge, pos.beats_per_bar);
		lv2_atom_forge_property_head(forge, host->urids.time_beatsPerMinute, 0);
		lv2_atom_forge_float(forge, pos.beats_per_minute);
		lv2_atom_forge_property_head(forge, host->urids.time_frame, 0);
		lv2_atom_forge_int64(forge, pos.frame);
		lv2_atom_forge_property_head(forge, host->urids.time_speed, 0);
		lv2_atom_forge_float(forge, speed);

		SerdNode s   = serd_node_from_string(SERD_BLANK, USTR("pos"));
		SerdNode p   = serd_node_from_string(SERD_URI, USTR(NS_RDF "value"));
		char*    str = atom_to_turtle(&host->unmap, &s, &p, (LV2_Atom*)frame.ref);
		printf("\n## Position\n%s\n", str);
		free(str);
	}
#endif

	switch (host->play_state) {
	case JALV_PAUSE_REQUESTED:
		host->play_state = JALV_PAUSED;
		zix_sem_post(&host->paused);
		break;
	case JALV_PAUSED:
		for (uint32_t p = 0; p < host->num_ports; ++p) {
			jack_port_t* jport = host->ports[p].jack_port;
			if (jport && host->ports[p].flow == FLOW_OUTPUT) {
				void* buf = jack_port_get_buffer(jport, nframes);
				if (host->ports[p].type == TYPE_EVENT) {
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
	for (uint32_t p = 0; p < host->num_ports; ++p) {
		if (!host->ports[p].jack_port)
			continue;

		if (host->ports[p].type == TYPE_AUDIO) {
			/* Connect plugin port directly to Jack port buffer. */
			lilv_instance_connect_port(
				host->instance, p,
				jack_port_get_buffer(host->ports[p].jack_port, nframes));

		} else if (host->ports[p].type == TYPE_EVENT) {
			/* Prepare event ports. */
			if (host->ports[p].flow == FLOW_INPUT) {
				lv2_evbuf_reset(host->ports[p].evbuf, true);

				void* buf = jack_port_get_buffer(host->ports[p].jack_port,
				                                 nframes);

				LV2_Evbuf_Iterator iter = lv2_evbuf_begin(host->ports[p].evbuf);
				for (uint32_t i = 0; i < jack_midi_get_event_count(buf); ++i) {
					jack_midi_event_t ev;
					jack_midi_event_get(&ev, buf, i);
					lv2_evbuf_write(&iter,
					                ev.time, 0,
					                host->midi_event_id,
					                ev.size, ev.buffer);
				}
			} else {
				lv2_evbuf_reset(host->ports[p].evbuf, false);
			}
		}
	}

	/* Read and apply control change events from UI */
	if (host->ui) {
		ControlChange ev;
		const size_t  space = jack_ringbuffer_read_space(host->ui_events);
		for (size_t i = 0; i < space; i += sizeof(ev) + ev.size) {
			jack_ringbuffer_read(host->ui_events, (char*)&ev, sizeof(ev));
			char body[ev.size];
			if (jack_ringbuffer_read(host->ui_events, body, ev.size) != ev.size) {
				fprintf(stderr, "error: Error reading from UI ring buffer\n");
				break;
			}
			assert(ev.index < host->num_ports);
			struct Port* const port = &host->ports[ev.index];
			if (ev.protocol == 0) {
				assert(ev.size == sizeof(float));
				port->control = *(float*)body;
			} else if (ev.protocol == host->urids.atom_eventTransfer) {
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
	lilv_instance_run(host->instance, nframes);

	/* Check if it's time to send updates to the UI */
	host->event_delta_t += nframes;
	bool           send_ui_updates = false;
	jack_nframes_t update_frames   = host->sample_rate / JALV_UI_UPDATE_HZ;
	if (host->ui && (host->event_delta_t > update_frames)) {
		send_ui_updates = true;
		host->event_delta_t = 0;
	}

	/* Deliver MIDI output and UI events */
	for (uint32_t p = 0; p < host->num_ports; ++p) {
		struct Port* const port = &host->ports[p];
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
				assert(size > 0);
				// FIXME: check type
				jack_midi_event_write(buf, frames, data, size);

				/* TODO: Be more disciminate about what to send */
				if (!port->old_api) {
					char buf[sizeof(ControlChange) + sizeof(LV2_Atom)];
					ControlChange* ev = (ControlChange*)buf;
					ev->index    = p;
					ev->protocol = host->urids.atom_eventTransfer;
					ev->size     = sizeof(LV2_Atom) + size;
					LV2_Atom* atom = (LV2_Atom*)ev->body;
					atom->type = type;
					atom->size = size;
					if (jack_ringbuffer_write_space(host->plugin_events)
					    < sizeof(buf) + size) {
						break;
					}
					jack_ringbuffer_write(host->plugin_events, buf, sizeof(buf));
					/* TODO: race, ensure reader handles this correctly */
					jack_ringbuffer_write(host->plugin_events, (void*)data, size);
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
			jack_ringbuffer_write(host->plugin_events, buf, sizeof(buf));
		}
	}

	return 0;
}

#ifdef JALV_JACK_SESSION
void
jack_session_cb(jack_session_event_t* event, void* arg)
{
	Jalv* host = (Jalv*)arg;

	#define MAX_CMD_LEN 256
	event->command_line = malloc(MAX_CMD_LEN);
	snprintf(event->command_line, MAX_CMD_LEN, "%s -u %s -l '%s'",
	         host->prog_name,
	         event->client_uuid,
	         event->session_dir);

	switch (event->type) {
	case JackSessionSave:
	case JackSessionSaveTemplate:
		jalv_save(host, event->session_dir);
		break;
	case JackSessionSaveAndQuit:
		jalv_save(host, event->session_dir);
		zix_sem_post(&exit_sem);
		break;
	}

	jack_session_reply(host->jack_client, event);
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
	LilvNode*       p   = lilv_new_uri(jalv->world, NS_UI "optionalFeature");
	LilvNode*       fs  = lilv_new_uri(jalv->world, NS_UI "fixedSize");
	LilvNode*       nrs = lilv_new_uri(jalv->world, NS_UI "noUserResize");

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
	Jalv* host = (Jalv*)controller;

	if (!host->ui) {
		return;
	}

	if (protocol != 0 && protocol != host->urids.atom_eventTransfer) {
		fprintf(stderr, "UI write with unsupported protocol %d (%s)\n",
		        protocol, symap_unmap(host->symap, protocol));
		return;
	}

	if (port_index >= host->num_ports) {
		fprintf(stderr, "UI write to out of range port index %d\n",
		        port_index);
		return;
	}

	if (protocol == host->urids.atom_eventTransfer) {
		SerdNode s = serd_node_from_string(SERD_BLANK, USTR("msg"));
		SerdNode p = serd_node_from_string(SERD_URI, USTR(NS_RDF "value"));

		const LV2_Atom* atom = (const LV2_Atom*)buffer;
		char*           str  = sratom_to_turtle(
			host->sratom, &host->unmap, "jalv:", &s, &p,
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
	jack_ringbuffer_write(host->ui_events, buf, sizeof(buf));
}

bool
jalv_emit_ui_events(Jalv* host)
{
	ControlChange ev;
	const size_t  space = jack_ringbuffer_read_space(host->plugin_events);
	for (size_t i = 0; i < space; i += sizeof(ev) + ev.size) {
		jack_ringbuffer_read(host->plugin_events, (char*)&ev, sizeof(ev));
		char buf[ev.size];
		jack_ringbuffer_read(host->plugin_events, buf, ev.size);

		if (ev.protocol == host->urids.atom_eventTransfer) {
			SerdNode  s    = serd_node_from_string(SERD_BLANK, USTR("msg"));
			SerdNode  p    = serd_node_from_string(SERD_URI, USTR(NS_RDF "value"));
			LV2_Atom* atom = (LV2_Atom*)buf;
			char*     str  = sratom_to_turtle(
				host->sratom, &host->unmap, "jalv:", &s, &p,
				atom->type, atom->size, LV2_ATOM_BODY(atom));
			printf("\n## Plugin => UI (%u bytes) ##\n%s\n", atom->size, str);
			free(str);
		}

		suil_instance_port_event(host->ui_instance, ev.index,
		                         ev.size, ev.protocol, buf);
	}

	return true;
}

static void
signal_handler(int ignored)
{
	zix_sem_post(&exit_sem);
}

int
main(int argc, char** argv)
{
	Jalv host;
	memset(&host, '\0', sizeof(Jalv));
	host.prog_name     = argv[0];
	host.midi_buf_size = 1024;  // Should be set by jack_buffer_size_cb
	host.play_state    = JALV_PAUSED;

	if (jalv_init(&argc, &argv, &host.opts)) {
		return EXIT_FAILURE;
	}

	if (host.opts.uuid) {
		printf("UUID: %s\n", host.opts.uuid);
	}

	host.symap = symap_new();
	uri_map.callback_data = &host;

	host.map.handle  = &host;
	host.map.map     = map_uri;
	map_feature.data = &host.map;

	host.unmap.handle  = &host;
	host.unmap.unmap   = unmap_uri;
	unmap_feature.data = &host.unmap;

	lv2_atom_forge_init(&host.forge, &host.map);

	host.sratom = sratom_new(&host.map);

	host.midi_event_id = uri_to_id(&host,
	                               "http://lv2plug.in/ns/ext/event",
	                               NS_MIDI "MidiEvent");
	host.urids.atom_eventTransfer  = symap_map(host.symap, LV2_ATOM__eventTransfer);
	host.urids.time_Position       = symap_map(host.symap, LV2_TIME__Position);
	host.urids.time_barBeat        = symap_map(host.symap, LV2_TIME__barBeat);
	host.urids.time_bar            = symap_map(host.symap, LV2_TIME__bar);
	host.urids.time_beatUnit       = symap_map(host.symap, LV2_TIME__beatUnit);
	host.urids.time_beatsPerBar    = symap_map(host.symap, LV2_TIME__beatsPerBar);
	host.urids.time_beatsPerMinute = symap_map(host.symap, LV2_TIME__beatsPerMinute);
	host.urids.time_frame          = symap_map(host.symap, LV2_TIME__frame);
	host.urids.time_speed          = symap_map(host.symap, LV2_TIME__speed);

	char* template = jalv_strdup("/tmp/jalv-XXXXXX");
	host.temp_dir = jalv_strjoin(mkdtemp(template), "/");
	free(template);

	LV2_State_Make_Path make_path = { &host, jalv_make_path };
	make_path_feature.data = &make_path;

	zix_sem_init(&exit_sem, 0);
	host.done = &exit_sem;

	zix_sem_init(&host.paused, 0);

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	/* Find all installed plugins */
	LilvWorld* world = lilv_world_new();
	lilv_world_load_all(world);
	host.world = world;
	const LilvPlugins* plugins = lilv_world_get_all_plugins(world);

	/* Set up the port classes this app supports */
	host.input_class    = lilv_new_uri(world, LILV_URI_INPUT_PORT);
	host.output_class   = lilv_new_uri(world, LILV_URI_OUTPUT_PORT);
	host.control_class  = lilv_new_uri(world, LILV_URI_CONTROL_PORT);
	host.audio_class    = lilv_new_uri(world, LILV_URI_AUDIO_PORT);
	host.event_class    = lilv_new_uri(world, LILV_URI_EVENT_PORT);
	host.chunk_class    = lilv_new_uri(world, LV2_ATOM__Chunk);
	host.seq_class      = lilv_new_uri(world, LV2_ATOM__Sequence);
	host.msg_port_class = lilv_new_uri(world, LV2_ATOM__MessagePort);
	host.midi_class     = lilv_new_uri(world, LILV_URI_MIDI_EVENT);
	host.preset_class   = lilv_new_uri(world, NS_PSET "Preset");
	host.label_pred     = lilv_new_uri(world, LILV_NS_RDFS "label");
	host.optional       = lilv_new_uri(world, LILV_NS_LV2
	                                   "connectionOptional");

	/* Get plugin URI from loaded state or command line */
	LilvState* state      = NULL;
	LilvNode*  plugin_uri = NULL;
	if (host.opts.load) {
		struct stat info;
		stat(host.opts.load, &info);
		if (S_ISDIR(info.st_mode)) {
			char* path = jalv_strjoin(host.opts.load, "/state.ttl");
			state = lilv_state_new_from_file(host.world, &host.map, NULL, path);
			free(path);
		} else {
			state = lilv_state_new_from_file(host.world, &host.map, NULL,
			                                 host.opts.load);
		}
		if (!state) {
			fprintf(stderr, "Failed to load state from %s\n", host.opts.load);
			return EXIT_FAILURE;
		}
		plugin_uri = lilv_node_duplicate(lilv_state_get_plugin_uri(state));
	} else if (argc > 1) {
		plugin_uri = lilv_new_uri(world, argv[1]);
	} else {
		fprintf(stderr, "Missing plugin URI parameter\n");
		return EXIT_FAILURE;
	}

	/* Find plugin */
	printf("Plugin:    %s\n", lilv_node_as_string(plugin_uri));
	host.plugin = lilv_plugins_get_by_uri(plugins, plugin_uri);
	lilv_node_free(plugin_uri);
	if (!host.plugin) {
		fprintf(stderr, "Failed to find plugin\n");
		lilv_world_free(world);
		return EXIT_FAILURE;
	}

	/* Get a plugin UI */
	LilvNode*       native_ui_type = jalv_native_ui_type(&host);
	const LilvNode* ui_type        = NULL;
	host.ui = NULL;
	if (native_ui_type) {
		LilvUIs* uis = lilv_plugin_get_uis(host.plugin);  // FIXME: leak
		LILV_FOREACH(uis, u, uis) {
			const LilvUI* this_ui = lilv_uis_get(uis, u);
			if (lilv_ui_is_supported(this_ui,
			                         suil_ui_supported,
			                         native_ui_type,
			                         &ui_type)) {
				// TODO: Multiple UI support
				host.ui = this_ui;
				break;
			}
		}
	}

	if (host.ui) {
		fprintf(stderr, "UI:        %s\n",
		        lilv_node_as_uri(lilv_ui_get_uri(host.ui)));

		host.ui_events     = jack_ringbuffer_create(4096);
		host.plugin_events = jack_ringbuffer_create(4096);
		jack_ringbuffer_mlock(host.ui_events);
		jack_ringbuffer_mlock(host.plugin_events);
	} else {
		fprintf(stderr, "No appropriate UI found\n");
	}

	/* Create port structures (host.ports) */
	jalv_create_ports(&host);

	/* Get the plugin's name */
	LilvNode*   name     = lilv_plugin_get_name(host.plugin);
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
	printf("JACK Name: %s\n\n", jack_name);
#ifdef JALV_JACK_SESSION
	if (host.opts.uuid) {
		host.jack_client = jack_client_open(jack_name, JackSessionID, NULL,
		                                    host.opts.uuid);
	}
#endif

	if (!host.jack_client) {
		host.jack_client = jack_client_open(jack_name, JackNullOption, NULL);
	}

	free(jack_name);
	lilv_node_free(name);

	if (!host.jack_client)
		die("Failed to connect to JACK.\n");

#ifdef jack_port_type_get_buffer_size
	host.midi_buf_size = jack_port_type_get_buffer_size(
		host.jack_client, JACK_DEFAULT_MIDI_TYPE);
	printf("MIDI buffer size: %zu\n", host.midi_buf_size);
#else
	host.midi_buf_size = 4096;
	fprintf(stderr, "warning: Old JACK, using default MIDI buffer size %zu\n",
	        host.midi_buf_size);
#endif

	/* Instantiate the plugin */
	host.instance = lilv_plugin_instantiate(
		host.plugin, jack_get_sample_rate(host.jack_client), features);
	if (!host.instance) {
		die("Failed to instantiate plugin.\n");
	}

	if (!host.buf_size_set) {
		jalv_allocate_port_buffers(&host);
	}

	/* Apply loaded state to plugin instance if necessary */
	if (state) {
		jalv_apply_state(&host, state);
	}

	/* Set instance for instance-access extension */
	instance_feature.data = lilv_instance_get_handle(host.instance);

	/* Set Jack callbacks */
	jack_set_process_callback(host.jack_client,
	                          &jack_process_cb, (void*)(&host));
	jack_set_buffer_size_callback(host.jack_client,
	                              &jack_buffer_size_cb, (void*)(&host));
#ifdef JALV_JACK_SESSION
	jack_set_session_callback(host.jack_client,
	                          &jack_session_cb, (void*)(&host));
#endif

	/* Create Jack ports and connect plugin ports to buffers */
	for (uint32_t i = 0; i < host.num_ports; ++i) {
		activate_port(&host, i);
	}

	/* Activate plugin */
	lilv_instance_activate(host.instance);

	/* Activate Jack */
	jack_activate(host.jack_client);
	host.sample_rate = jack_get_sample_rate(host.jack_client);
	host.play_state  = JALV_RUNNING;

	SuilHost* ui_host = NULL;
	if (host.ui) {
		/* Instantiate UI */
		ui_host = suil_host_new(jalv_ui_write, NULL, NULL, NULL);

		host.ui_instance = suil_instance_new(
			ui_host,
			&host,
			lilv_node_as_uri(native_ui_type),
			lilv_node_as_uri(lilv_plugin_get_uri(host.plugin)),
			lilv_node_as_uri(lilv_ui_get_uri(host.ui)),
			lilv_node_as_uri(ui_type),
			lilv_uri_to_path(lilv_node_as_uri(lilv_ui_get_bundle_uri(host.ui))),
			lilv_uri_to_path(lilv_node_as_uri(lilv_ui_get_binary_uri(host.ui))),
			features);

		if (!host.ui_instance) {
			die("Failed to instantiate plugin.\n");
		}

		/* Set initial control values for UI */
		for (uint32_t i = 0; i < host.num_ports; ++i) {
			if (host.ports[i].type == TYPE_CONTROL) {
				suil_instance_port_event(host.ui_instance, i,
				                         sizeof(float), 0,
				                         &host.ports[i].control);
			}
		}
	}

	/* Run UI (or prompt at console) */
	jalv_open_ui(&host, host.ui_instance);

	/* Wait for finish signal from UI or signal handler */
	zix_sem_wait(&exit_sem);

	fprintf(stderr, "Exiting...\n");

	/* Deactivate JACK */
	jack_deactivate(host.jack_client);
	for (uint32_t i = 0; i < host.num_ports; ++i) {
		if (host.ports[i].evbuf) {
			lv2_evbuf_free(host.ports[i].evbuf);
		}
	}
	jack_client_close(host.jack_client);

	/* Deactivate plugin */
	suil_instance_free(host.ui_instance);
	lilv_instance_deactivate(host.instance);
	lilv_instance_free(host.instance);

	/* Clean up */
	free(host.ports);
	if (host.ui) {
		jack_ringbuffer_free(host.ui_events);
		jack_ringbuffer_free(host.plugin_events);
	}
	lilv_node_free(native_ui_type);
	lilv_node_free(host.input_class);
	lilv_node_free(host.output_class);
	lilv_node_free(host.control_class);
	lilv_node_free(host.audio_class);
	lilv_node_free(host.event_class);
	lilv_node_free(host.midi_class);
	lilv_node_free(host.preset_class);
	lilv_node_free(host.label_pred);
	lilv_node_free(host.optional);
	symap_free(host.symap);
	suil_host_free(ui_host);
	lilv_world_free(world);

	zix_sem_destroy(&exit_sem);

	remove(host.temp_dir);
	free(host.temp_dir);

	return 0;
}
