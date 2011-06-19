/*
  Copyright 2007-2011 David Robillard <http://drobilla.net>

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

#define _XOPEN_SOURCE 500

#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jack/jack.h>
#include <jack/midiport.h>
#ifdef JALV_JACK_SESSION
#    include <jack/session.h>
#endif

#include "lv2/lv2plug.in/ns/ext/event/event-helpers.h"
#include "lv2/lv2plug.in/ns/ext/event/event.h"
#include "lv2/lv2plug.in/ns/ext/uri-map/uri-map.h"

#include "lilv/lilv.h"

#include "suil/suil.h"

#include "jalv_internal.h"
#include "jalv-config.h"

sem_t exit_sem;  /**< Exit semaphore */

#define MIDI_BUFFER_SIZE 1024

typedef struct {
	uint32_t index;
	float    value;
} ControlChange;

enum PortType {
	CONTROL,
	AUDIO,
	EVENT
};

struct Port {
	const LilvPort*   lilv_port;
	enum PortType     type;
	jack_port_t*      jack_port; /**< For audio/MIDI ports, otherwise NULL */
	float             control;   /**< For control ports, otherwise 0.0f */
	LV2_Event_Buffer* ev_buffer; /**< For MIDI ports, otherwise NULL */
	bool              is_input;
};

/**
   Map function for URI map extension.
*/
uint32_t
uri_to_id(LV2_URI_Map_Callback_Data callback_data,
          const char*               map,
          const char*               uri)
{
	Jalv* host = (Jalv*)callback_data;
	return symap_map(host->symap, uri);
}

#define NS_EXT "http://lv2plug.in/ns/ext/"

static LV2_URI_Map_Feature uri_map          = { NULL, &uri_to_id };
static const LV2_Feature   uri_map_feature  = { NS_EXT "uri-map", &uri_map };
static LV2_Feature         instance_feature = { NS_EXT "instance-access", NULL };

const LV2_Feature* features[3] = {
	&uri_map_feature, &instance_feature, NULL
};

/** Abort and exit on error */
static void
die(const char* msg)
{
	fprintf(stderr, "%s\n", msg);
	exit(EXIT_FAILURE);
}

/** Creates a port and connects the plugin instance to its data location.
 *
 * For audio ports, creates a jack port and connects plugin port to buffer.
 *
 * For control ports, sets controls array to default value and connects plugin
 * port to that element.
 */
void
create_port(Jalv*    host,
            uint32_t port_index,
            float    default_value)
{
	struct Port* const port = &host->ports[port_index];

	port->lilv_port = lilv_plugin_get_port_by_index(host->plugin, port_index);
	port->jack_port = NULL;
	port->control   = 0.0f;
	port->ev_buffer = NULL;

	lilv_instance_connect_port(host->instance, port_index, NULL);

	/* Get the port symbol for console printing */
	const LilvNode* symbol     = lilv_port_get_symbol(host->plugin, port->lilv_port);
	const char*     symbol_str = lilv_node_as_string(symbol);

	const bool optional = lilv_port_has_property(host->plugin,
	                                             port->lilv_port,
	                                             host->optional);

	enum JackPortFlags jack_flags = 0;
	if (lilv_port_is_a(host->plugin, port->lilv_port, host->input_class)) {
		jack_flags     = JackPortIsInput;
		port->is_input = true;
	} else if (lilv_port_is_a(host->plugin, port->lilv_port, host->output_class)) {
		jack_flags     = JackPortIsOutput;
		port->is_input = false;
	} else if (optional) {
		lilv_instance_connect_port(host->instance, port_index, NULL);
		return;
	} else {
		die("Mandatory port has unknown type (neither input nor output)");
	}

	/* Set control values */
	if (lilv_port_is_a(host->plugin, port->lilv_port, host->control_class)) {
		port->type    = CONTROL;
		port->control = isnan(default_value) ? 0.0 : default_value;
		printf("%s = %f\n", symbol_str, host->ports[port_index].control);
	} else if (lilv_port_is_a(host->plugin, port->lilv_port, host->audio_class)) {
		port->type = AUDIO;
	} else if (lilv_port_is_a(host->plugin, port->lilv_port, host->event_class)) {
		port->type = EVENT;
	} else if (optional) {
		lilv_instance_connect_port(host->instance, port_index, NULL);
		return;
	} else {
		die("Mandatory port has unknown type (neither control nor audio nor event)");
	}

	/* Connect the port based on its type */
	switch (port->type) {
	case CONTROL:
		lilv_instance_connect_port(host->instance, port_index, &port->control);
		break;
	case AUDIO:
		port->jack_port = jack_port_register(
			host->jack_client, symbol_str, JACK_DEFAULT_AUDIO_TYPE, jack_flags, 0);
		break;
	case EVENT:
		port->jack_port = jack_port_register(
			host->jack_client, symbol_str, JACK_DEFAULT_MIDI_TYPE, jack_flags, 0);
		port->ev_buffer = lv2_event_buffer_new(MIDI_BUFFER_SIZE, LV2_EVENT_AUDIO_STAMP);
		lilv_instance_connect_port(host->instance, port_index, port->ev_buffer);
		break;
	default:
		break;
	}
}

/** Jack process callback. */
int
jack_process_cb(jack_nframes_t nframes, void* data)
{
	Jalv* const host = (Jalv*)data;

	/* Prepare port buffers */
	for (uint32_t p = 0; p < host->num_ports; ++p) {
		if (!host->ports[p].jack_port)
			continue;

		if (host->ports[p].type == AUDIO) {
			/* Connect plugin port directly to Jack port buffer. */
			lilv_instance_connect_port(
				host->instance, p,
				jack_port_get_buffer(host->ports[p].jack_port, nframes));

		} else if (host->ports[p].type == EVENT) {
			/* Clear Jack event port buffer. */
			lv2_event_buffer_reset(host->ports[p].ev_buffer,
			                       LV2_EVENT_AUDIO_STAMP,
			                       (uint8_t*)(host->ports[p].ev_buffer + 1));

			if (host->ports[p].is_input) {
				void* buf = jack_port_get_buffer(host->ports[p].jack_port,
				                                 nframes);

				LV2_Event_Iterator iter;
				lv2_event_begin(&iter, host->ports[p].ev_buffer);

				for (uint32_t i = 0; i < jack_midi_get_event_count(buf); ++i) {
					jack_midi_event_t ev;
					jack_midi_event_get(&ev, buf, i);
					lv2_event_write(&iter,
					                ev.time, 0,
					                host->midi_event_id,
					                ev.size, ev.buffer);
				}
			}
		}
	}

	/* Read and apply control change events from UI */
	if (host->ui) {
		ControlChange ev;
		size_t        ev_read_size = jack_ringbuffer_read_space(host->ui_events);
		for (size_t i = 0; i < ev_read_size; i += sizeof(ev)) {
			jack_ringbuffer_read(host->ui_events, (char*)&ev, sizeof(ev));
			host->ports[ev.index].control = ev.value;
		}
	}

	/* Run plugin for this cycle */
	lilv_instance_run(host->instance, nframes);

	/* Check if it's time to send updates to the UI */
	host->event_delta_t += nframes;
	bool send_ui_updates = false;
	if (host->ui && (host->event_delta_t > host->sample_rate / JALV_UI_UPDATE_HZ)) {
		send_ui_updates = true;
		host->event_delta_t = 0;
	}

	/* Deliver MIDI output and UI events */
	for (uint32_t p = 0; p < host->num_ports; ++p) {
		if (host->ports[p].jack_port
		    && !host->ports[p].is_input
		    && host->ports[p].type == EVENT) {

			void* buf = jack_port_get_buffer(host->ports[p].jack_port,
			                                 nframes);

			jack_midi_clear_buffer(buf);

			LV2_Event_Iterator iter;
			lv2_event_begin(&iter, host->ports[p].ev_buffer);

			for (uint32_t i = 0; i < iter.buf->event_count; ++i) {
				uint8_t*   data;
				LV2_Event* ev = lv2_event_get(&iter, &data);
				jack_midi_event_write(buf, ev->frames, data, ev->size);
				lv2_event_increment(&iter);
			}
		} else if (send_ui_updates
		           && !host->ports[p].is_input
		           && host->ports[p].type == CONTROL) {
			const ControlChange ev = { p, host->ports[p].control };
			jack_ringbuffer_write(host->plugin_events, (const char*)&ev, sizeof(ev));
		}
	}

	return 0;
}

#ifdef JALV_JACK_SESSION
void
jack_session_cb(jack_session_event_t* event, void* arg)
{
	Jalv* host = (Jalv*)arg;

	char cmd[256];
	snprintf(cmd, sizeof(cmd), "jalv %s %s",
	         lilv_node_as_uri(lilv_plugin_get_uri(host->plugin)),
	         event->client_uuid);

	event->command_line = strdup(cmd);
	jack_session_reply(host->jack_client, event);

	switch (event->type) {
	case JackSessionSave:
		break;
	case JackSessionSaveAndQuit:
		sem_post(&exit_sem);
		break;
	case JackSessionSaveTemplate:
		break;
	}

	jack_session_event_free(event);
}
#endif /* JALV_JACK_SESSION */

static void
lv2_ui_write(SuilController controller,
             uint32_t       port_index,
             uint32_t       buffer_size,
             uint32_t       format,
             const void*    buffer)
{
	if (format != 0) {
		return;
	}

	Jalv* host = (Jalv*)controller;

	const ControlChange ev = { port_index, *(float*)buffer };
	jack_ringbuffer_write(host->ui_events, (const char*)&ev, sizeof(ev));
}

bool
jalv_emit_ui_events(Jalv* host)
{
	ControlChange ev;
	size_t        ev_read_size = jack_ringbuffer_read_space(host->plugin_events);
	for (size_t i = 0; i < ev_read_size; i += sizeof(ev)) {
		jack_ringbuffer_read(host->plugin_events, (char*)&ev, sizeof(ev));
		suil_instance_port_event(host->ui_instance, ev.index,
		                         sizeof(float), 0, &ev.value);
	}

	return true;
}

static void
signal_handler(int ignored)
{
	sem_post(&exit_sem);
}

int
main(int argc, char** argv)
{
	jalv_init(&argc, &argv);

	Jalv host;
	host.jack_client   = NULL;
	host.num_ports     = 0;
	host.ports         = NULL;
	host.ui_events     = NULL;
	host.plugin_events = NULL;
	host.event_delta_t = 0;

	host.symap = symap_new();
	uri_map.callback_data = &host;
	host.midi_event_id = uri_to_id(&host,
	                               "http://lv2plug.in/ns/ext/event",
	                               "http://lv2plug.in/ns/ext/midi#MidiEvent");

	sem_init(&exit_sem, 0, 0);
	host.done = &exit_sem;

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	/* Find all installed plugins */
	LilvWorld* world = lilv_world_new();
	lilv_world_load_all(world);
	host.world = world;
	const LilvPlugins* plugins = lilv_world_get_all_plugins(world);

	/* Set up the port classes this app supports */
	host.input_class   = lilv_new_uri(world, LILV_URI_INPUT_PORT);
	host.output_class  = lilv_new_uri(world, LILV_URI_OUTPUT_PORT);
	host.control_class = lilv_new_uri(world, LILV_URI_CONTROL_PORT);
	host.audio_class   = lilv_new_uri(world, LILV_URI_AUDIO_PORT);
	host.event_class   = lilv_new_uri(world, LILV_URI_EVENT_PORT);
	host.midi_class    = lilv_new_uri(world, LILV_URI_MIDI_EVENT);
	host.optional      = lilv_new_uri(world, LILV_NS_LV2
	                                  "connectionOptional");

#ifdef JALV_JACK_SESSION
	if (argc != 2 && argc != 3) {
		fprintf(stderr, "Usage: %s PLUGIN_URI [JACK_UUID]\n", argv[0]);
#else
	if (argc != 2) {
		fprintf(stderr, "Usage: %s PLUGIN_URI\n", argv[0]);
#endif
		lilv_world_free(world);
		return EXIT_FAILURE;
	}

	const char* const plugin_uri_str = argv[1];

	printf("Plugin:    %s\n", plugin_uri_str);

	/* Get the plugin */
	LilvNode* plugin_uri = lilv_new_uri(world, plugin_uri_str);
	host.plugin = lilv_plugins_get_by_uri(plugins, plugin_uri);
	lilv_node_free(plugin_uri);
	if (!host.plugin) {
		fprintf(stderr, "Failed to find plugin %s.\n", plugin_uri_str);
		lilv_world_free(world);
		return EXIT_FAILURE;
	}

	/* Get a plugin UI */
	LilvNode*       native_ui_type = jalv_native_ui_type(&host);
	const LilvNode* ui_type        = NULL;
	host.ui = NULL;
	if (native_ui_type) {
		LilvUIs* uis = lilv_plugin_get_uis(host.plugin); // FIXME: leak
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

	/* Get the plugin's name */
	LilvNode*   name     = lilv_plugin_get_name(host.plugin);
	const char* name_str = lilv_node_as_string(name);

	/* Truncate plugin name to suit JACK (if necessary) */
	char* jack_name = NULL;
	if (strlen(name_str) >= (unsigned)jack_client_name_size() - 1) {
		jack_name = calloc(jack_client_name_size(), sizeof(char));
		strncpy(jack_name, name_str, jack_client_name_size() - 1);
	} else {
		jack_name = strdup(name_str);
	}

	/* Connect to JACK */
	printf("JACK Name: %s\n\n", jack_name);
#ifdef JALV_JACK_SESSION
	const char* const jack_uuid_str = (argc > 2) ? argv[2] : NULL;
	if (jack_uuid_str) {
		host.jack_client = jack_client_open(jack_name, JackSessionID, NULL,
		                                    jack_uuid_str);
	}
#endif

	if (!host.jack_client) {
		host.jack_client = jack_client_open(jack_name, JackNullOption, NULL);
	}

	free(jack_name);
	lilv_node_free(name);

	if (!host.jack_client)
		die("Failed to connect to JACK.\n");

	/* Instantiate the plugin */
	host.instance = lilv_plugin_instantiate(
		host.plugin, jack_get_sample_rate(host.jack_client), features);
	if (!host.instance)
		die("Failed to instantiate plugin.\n");

	/* Set instance for instance-access extension */
	instance_feature.data = lilv_instance_get_handle(host.instance);

	/* Set Jack callbacks */
	jack_set_process_callback(host.jack_client, &jack_process_cb, (void*)(&host));
#ifdef JALV_JACK_SESSION
	jack_set_session_callback(host.jack_client, &jack_session_cb, (void*)(&host));
#endif

	/* Create ports */
	host.num_ports = lilv_plugin_get_num_ports(host.plugin);
	host.ports     = calloc((size_t)host.num_ports, sizeof(struct Port));
	float* default_values = calloc(lilv_plugin_get_num_ports(host.plugin),
	                               sizeof(float));
	lilv_plugin_get_port_ranges_float(host.plugin, NULL, NULL, default_values);

	for (uint32_t i = 0; i < host.num_ports; ++i)
		create_port(&host, i, default_values[i]);

	free(default_values);

	/* Activate plugin and JACK */
	lilv_instance_activate(host.instance);
	jack_activate(host.jack_client);
	host.sample_rate = jack_get_sample_rate(host.jack_client);

	SuilHost* ui_host = NULL;
	if (host.ui) {
		/* Instantiate UI */
		ui_host = suil_host_new(lv2_ui_write, NULL, NULL, NULL);

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
	}

	/* Run UI (or prompt at console) */
	jalv_open_ui(&host, host.ui_instance);

	/* Wait for finish signal from UI or signal handler */
	sem_wait(&exit_sem);

	fprintf(stderr, "Exiting...\n");

	/* Deactivate JACK */
	jack_deactivate(host.jack_client);
	for (uint32_t i = 0; i < host.num_ports; ++i) {
		if (host.ports[i].ev_buffer) {
			free(host.ports[i].ev_buffer);
		}
	}
	jack_client_close(host.jack_client);

	/* Deactivate plugin */
	lilv_instance_deactivate(host.instance);
	lilv_instance_free(host.instance);

	/* Clean up */
	free(host.ports);
	jack_ringbuffer_free(host.ui_events);
	jack_ringbuffer_free(host.plugin_events);
	lilv_node_free(native_ui_type);
	lilv_node_free(host.input_class);
	lilv_node_free(host.output_class);
	lilv_node_free(host.control_class);
	lilv_node_free(host.audio_class);
	lilv_node_free(host.event_class);
	lilv_node_free(host.midi_class);
	lilv_node_free(host.optional);
	symap_free(host.symap);
	suil_instance_free(host.ui_instance);
	suil_host_free(ui_host);
	lilv_world_free(world);

	sem_destroy(&exit_sem);

	return 0;
}
