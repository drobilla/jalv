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

#ifndef JALV_INTERNAL_H
#define JALV_INTERNAL_H

#include <semaphore.h>

#include <jack/jack.h>
#include <jack/ringbuffer.h>

#include "lilv/lilv.h"
#include "serd/serd.h"
#include "suil/suil.h"

#include "lv2_evbuf.h"
#include "symap.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JALV_UI_UPDATE_HZ 15

enum PortFlow {
	FLOW_UNKNOWN,
	FLOW_INPUT,
	FLOW_OUTPUT
};

enum PortType {
	TYPE_UNKNOWN,
	TYPE_CONTROL,
	TYPE_AUDIO,
	TYPE_EVENT
};

struct Port {
	const LilvPort*   lilv_port;
	enum PortType     type;
	enum PortFlow     flow;
	jack_port_t*      jack_port;  /**< For audio/MIDI ports, otherwise NULL */
	float             control;    /**< For control ports, otherwise 0.0f */
	LV2_Evbuf*        evbuf;      /**< For MIDI ports, otherwise NULL */
};

struct Property {
	uint32_t key;
	SerdNode value;
	SerdNode datatype;
};

typedef struct {
	char* uuid;
	char* load;
} JalvOptions;

typedef struct {
	JalvOptions        opts;          /**< Command-line options */
	const char*        prog_name;     /**< Program name (argv[0]) */
	LilvWorld*         world;         /**< Lilv World */
	int                ui_width;      /**< Requested UI width */
	int                ui_height;     /**< Requested UI height */
	SerdReader*        reader;        /**< RDF reader (for persistence) */
	SerdWriter*        writer;        /**< RDF writer (for persistence) */
	struct Property*   props;         /**< Restored state properties */
	SerdNode           state_node;    /**< Instance state node (for persistence) */
	SerdNode           last_sym;      /**< Last port symbol encountered in state */
	Symap*             symap;         /**< Symbol (URI) map */
	jack_client_t*     jack_client;   /**< Jack client */
	jack_ringbuffer_t* ui_events;     /**< Port events from UI */
	jack_ringbuffer_t* plugin_events; /**< Port events from plugin */
	sem_t*             done;          /**< Exit semaphore */
	const LilvPlugin*  plugin;        /**< Plugin class (RDF data) */
	const LilvUI*      ui;            /**< Plugin UI (RDF data) */
	LilvInstance*      instance;      /**< Plugin instance (shared library) */
	SuilInstance*      ui_instance;   /**< Plugin UI instance (shared library) */
	struct Port*       ports;         /**< Port array of size num_ports */
	size_t             midi_buf_size; /**< Size of MIDI port buffers */
	uint32_t           num_ports;     /**< Size of the two following arrays: */
	uint32_t           num_props;     /**< Number of properties */
	uint32_t           longest_sym;   /**< Longest port symbol */
	jack_nframes_t     sample_rate;   /**< Sample rate */
	jack_nframes_t     event_delta_t; /**< Frames since last update sent to UI */
	LilvNode*          input_class;   /**< Input port class (URI) */
	LilvNode*          output_class;  /**< Output port class (URI) */
	LilvNode*          control_class; /**< Control port class (URI) */
	LilvNode*          audio_class;   /**< Audio port class (URI) */
	LilvNode*          event_class;   /**< Event port class (URI) */
	LilvNode*          midi_class;    /**< MIDI event class (URI) */
	LilvNode*          optional;      /**< lv2:connectionOptional port property */
	uint32_t           midi_event_id; /**< MIDI event class ID */
	bool               in_state;      /**< True iff reading instance state */
	bool               buf_size_set;  /**< True iff Jack buf size callback fired */
} Jalv;

int
jalv_init(int* argc, char*** argv, JalvOptions* opts);

void
jalv_create_ports(Jalv* jalv);

struct Port*
jalv_port_by_symbol(Jalv* jalv, const char* sym);

LilvNode*
jalv_native_ui_type(Jalv* jalv);

int
jalv_open_ui(Jalv*         jalv,
             SuilInstance* instance);

bool
jalv_emit_ui_events(Jalv* jalv);

int
jalv_ui_resize(Jalv* jalv, int width, int height);

void
jalv_save(Jalv* jalv, const char* dir);

void
jalv_restore(Jalv* jalv, const char* dir);

void
jalv_restore_instance(Jalv* jalv, const char* dir);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // JALV_INTERNAL_H
