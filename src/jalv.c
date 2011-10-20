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

#include "jalv_internal.h"
#include "jalv-config.h"

#include <jack/jack.h>
#include <jack/midiport.h>
#ifdef JALV_JACK_SESSION
#    include <jack/session.h>
#endif

#include "lv2/lv2plug.in/ns/ext/event/event-helpers.h"
#include "lv2/lv2plug.in/ns/ext/uri-map/uri-map.h"
#ifdef HAVE_LV2_UI_RESIZE
#    include "lv2/lv2plug.in/ns/ext/ui-resize/ui-resize.h"
#endif

#include "lilv/lilv.h"

#include "suil/suil.h"

sem_t exit_sem;  /**< Exit semaphore */

/**
   Control change event, sent through ring buffers for UI updates.
*/
typedef struct {
	uint32_t index;
	float    value;
} ControlChange;

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

#ifdef HAVE_LV2_UI_RESIZE
static int
lv2_ui_resize(LV2_UI_Resize_Feature_Data data, int width, int height)
{
	Jalv* jalv = (Jalv*)data;
	jalv->ui_width  = width;
	jalv->ui_height = height;
	return jalv_ui_resize(jalv, width, height);
}

LV2_UI_Resize_Feature    ui_resize         = { NULL, &lv2_ui_resize };
static const LV2_Feature ui_resize_feature = { NS_EXT "ui-resize#UIResize", &ui_resize };

const LV2_Feature* features[4] = {
	&uri_map_feature, &instance_feature, &ui_resize_feature
};
#else
const LV2_Feature* features[3] = {
	&uri_map_feature, &instance_feature, NULL
};
#endif

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
	port->control   = 0.0f;
	port->ev_buffer = NULL;
	port->flow      = FLOW_UNKNOWN;

	/* Get the port symbol for console printing */
	const LilvNode* symbol     = lilv_port_get_symbol(host->plugin, port->lilv_port);
	const char*     symbol_str = lilv_node_as_string(symbol);

	const bool optional = lilv_port_has_property(host->plugin,
	                                             port->lilv_port,
	                                             host->optional);

	/* Set the port flow (input or output) */
	if (lilv_port_is_a(host->plugin, port->lilv_port, host->input_class)) {
		port->flow = FLOW_INPUT;
	} else if (lilv_port_is_a(host->plugin, port->lilv_port, host->output_class)) {
		port->flow = FLOW_OUTPUT;
	} else if (!optional) {
		die("Mandatory port has unknown type (neither input nor output)");
	}

	/* Set control values */
	if (lilv_port_is_a(host->plugin, port->lilv_port, host->control_class)) {
		port->type    = TYPE_CONTROL;
		port->control = isnan(default_value) ? 0.0 : default_value;
	} else if (lilv_port_is_a(host->plugin, port->lilv_port, host->audio_class)) {
		port->type = TYPE_AUDIO;
	} else if (lilv_port_is_a(host->plugin, port->lilv_port, host->event_class)) {
		port->type = TYPE_EVENT;
	} else if (!optional) {
		die("Mandatory port has unknown type (neither control nor audio nor event)");
	}

	const size_t sym_len = strlen(symbol_str);
	host->longest_sym = (sym_len > host->longest_sym) ? sym_len : host->longest_sym;
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
			free(port->ev_buffer);
			port->ev_buffer = lv2_event_buffer_new(
				jalv->midi_buf_size, LV2_EVENT_AUDIO_STAMP);
			lilv_instance_connect_port(
				jalv->instance, i, port->ev_buffer);
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
	const LilvNode* symbol     = lilv_port_get_symbol(host->plugin, port->lilv_port);
	const char*     symbol_str = lilv_node_as_string(symbol);

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
			host->jack_client, symbol_str, JACK_DEFAULT_AUDIO_TYPE, jack_flags, 0);
		break;
	case TYPE_EVENT:
		port->jack_port = jack_port_register(
			host->jack_client, symbol_str, JACK_DEFAULT_MIDI_TYPE, jack_flags, 0);
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
	host->midi_buf_size = jack_port_type_get_buffer_size(
		host->jack_client, JACK_DEFAULT_MIDI_TYPE);
	jalv_allocate_port_buffers(host);
	return 0;
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

		if (host->ports[p].type == TYPE_AUDIO) {
			/* Connect plugin port directly to Jack port buffer. */
			lilv_instance_connect_port(
				host->instance, p,
				jack_port_get_buffer(host->ports[p].jack_port, nframes));

		} else if (host->ports[p].type == TYPE_EVENT) {
			/* Clear Jack event port buffer. */
			lv2_event_buffer_reset(host->ports[p].ev_buffer,
			                       LV2_EVENT_AUDIO_STAMP,
			                       (uint8_t*)(host->ports[p].ev_buffer + 1));

			if (host->ports[p].flow == FLOW_INPUT) {
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
		    && !host->ports[p].flow == FLOW_INPUT
		    && host->ports[p].type == TYPE_EVENT) {

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
		           && !host->ports[p].flow == FLOW_INPUT
		           && host->ports[p].type == TYPE_CONTROL) {
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
	snprintf(cmd, sizeof(cmd), "%s -u %s -l '%s'",
	         host->prog_name,
	         event->client_uuid,
	         event->session_dir);

	event->command_line = strdup(cmd);

	switch (event->type) {
	case JackSessionSave:
	case JackSessionSaveTemplate:
		jalv_save(host, event->session_dir);
		break;
	case JackSessionSaveAndQuit:
		jalv_save(host, event->session_dir);
		sem_post(&exit_sem);
		break;
	}

	jack_session_reply(host->jack_client, event);
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
	Jalv host;
	memset(&host, '\0', sizeof(Jalv));
	host.prog_name     = argv[0];
	host.midi_buf_size = 1024;  // Should be set by jack_buffer_size_cb

	if (jalv_init(&argc, &argv, &host.opts)) {
		return EXIT_FAILURE;
	}

	if (host.opts.uuid) {
		printf("UUID: %s\n", host.opts.uuid);
	}

	host.symap = symap_new();
	uri_map.callback_data = &host;
	host.midi_event_id = uri_to_id(&host,
	                               "http://lv2plug.in/ns/ext/event",
	                               "http://lv2plug.in/ns/ext/midi#MidiEvent");

#ifdef HAVE_LV2_UI_RESIZE
	ui_resize.data = &host;
	host.ui_width  = -1;
	host.ui_height = -1;
#endif

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

	if (host.opts.load) {
		jalv_restore(&host, host.opts.load);
	} else if (argc > 1) {
		const char* const plugin_uri_str = argv[1];

		/* Get the plugin */
		LilvNode* plugin_uri = lilv_new_uri(world, plugin_uri_str);
		host.plugin = lilv_plugins_get_by_uri(plugins, plugin_uri);
		lilv_node_free(plugin_uri);
		if (!host.plugin) {
			fprintf(stderr, "Failed to find plugin %s\n", plugin_uri_str);
			lilv_world_free(world);
			return EXIT_FAILURE;
		}
	} else {
		fprintf(stderr, "Missing plugin URI parameter\n");
		return EXIT_FAILURE;
	}

	printf("Plugin:    %s\n",
	       lilv_node_as_string(lilv_plugin_get_uri(host.plugin)));

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

	host.midi_buf_size = jack_port_type_get_buffer_size(
		host.jack_client, JACK_DEFAULT_MIDI_TYPE);

	/* Instantiate the plugin */
	host.instance = lilv_plugin_instantiate(
		host.plugin, jack_get_sample_rate(host.jack_client), features);
	if (!host.instance)
		die("Failed to instantiate plugin.\n");

	if (!host.buf_size_set) {
		jalv_allocate_port_buffers(&host);
	}

	/* Apply restored state to plugin instance (if applicable) */
	if (host.opts.load) {
		jalv_restore_instance(&host, host.opts.load);
	} else {
		jalv_create_ports(&host);
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
	lilv_node_free(host.optional);
	symap_free(host.symap);
	suil_instance_free(host.ui_instance);
	suil_host_free(ui_host);
	lilv_world_free(world);

	sem_destroy(&exit_sem);

	return 0;
}
