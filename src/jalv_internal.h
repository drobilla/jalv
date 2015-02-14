/*
  Copyright 2007-2014 David Robillard <http://drobilla.net>

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

#include <stdlib.h>
#include <string.h>

#include <jack/jack.h>
#include <jack/ringbuffer.h>

#include "lilv/lilv.h"
#include "serd/serd.h"
#include "suil/suil.h"

#include "lv2/lv2plug.in/ns/ext/atom/atom.h"
#include "lv2/lv2plug.in/ns/ext/atom/forge.h"
#include "lv2/lv2plug.in/ns/ext/log/log.h"
#include "lv2/lv2plug.in/ns/ext/midi/midi.h"
#include "lv2/lv2plug.in/ns/ext/resize-port/resize-port.h"
#include "lv2/lv2plug.in/ns/ext/state/state.h"
#include "lv2/lv2plug.in/ns/ext/urid/urid.h"
#include "lv2/lv2plug.in/ns/ext/worker/worker.h"

#include "zix/sem.h"
#include "zix/thread.h"

#include "sratom/sratom.h"

#include "lv2_evbuf.h"
#include "symap.h"

#ifdef __cplusplus
extern "C" {
#endif

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
	const LilvPort* lilv_port;
	enum PortType   type;
	enum PortFlow   flow;
	jack_port_t*    jack_port;  ///< For audio/MIDI ports, otherwise NULL
	LV2_Evbuf*      evbuf;      ///< For MIDI ports, otherwise NULL
	void*           widget;     ///< Control widget, if applicable
	size_t          buf_size;   ///< Custom buffer size, or 0
	uint32_t        index;      ///< Port index
	float           control;    ///< For control ports, otherwise 0.0f
	bool            old_api;    ///< True for event, false for atom
};

/**
   Control change event, sent through ring buffers for UI updates.
*/
typedef struct {
	uint32_t index;
	uint32_t protocol;
	uint32_t size;
	uint8_t  body[];
} ControlChange;

typedef struct {
	char*    uuid;              ///< Session UUID
	char*    load;              ///< Path for state to load
	char**   controls;          ///< Control values
	uint32_t buffer_size;       ///< Plugin <= >UI communication buffer size
	double   update_rate;       ///< UI update rate in Hz
	int      dump;              ///< Dump communication iff true
	int      generic_ui;        ///< Use generic UI iff true
	int      show_hidden;       ///< Show controls for notOnGUI ports
	int      no_menu;           ///< Hide menu iff true
	int      show_ui;           ///< Show non-embedded UI
	int      print_controls;    ///< Print control changes to stdout
} JalvOptions;

typedef struct {
	LV2_URID atom_Float;
	LV2_URID atom_Int;
	LV2_URID atom_eventTransfer;
	LV2_URID bufsz_maxBlockLength;
	LV2_URID bufsz_minBlockLength;
	LV2_URID bufsz_sequenceSize;
	LV2_URID log_Trace;
	LV2_URID midi_MidiEvent;
	LV2_URID param_sampleRate;
	LV2_URID patch_Set;
	LV2_URID patch_property;
	LV2_URID patch_value;
	LV2_URID time_Position;
	LV2_URID time_bar;
	LV2_URID time_barBeat;
	LV2_URID time_beatUnit;
	LV2_URID time_beatsPerBar;
	LV2_URID time_beatsPerMinute;
	LV2_URID time_frame;
	LV2_URID time_speed;
	LV2_URID ui_updateRate;
} JalvURIDs;

typedef struct {
	LilvNode* atom_AtomPort;
	LilvNode* atom_Chunk;
	LilvNode* atom_Sequence;
	LilvNode* ev_EventPort;
	LilvNode* lv2_AudioPort;
	LilvNode* lv2_ControlPort;
	LilvNode* lv2_InputPort;
	LilvNode* lv2_OutputPort;
	LilvNode* lv2_connectionOptional;
	LilvNode* lv2_control;
	LilvNode* lv2_name;
	LilvNode* lv2_reportsLatency;
	LilvNode* midi_MidiEvent;
	LilvNode* pg_group;
	LilvNode* pset_Preset;
	LilvNode* rdfs_label;
	LilvNode* rsz_minimumSize;
	LilvNode* work_interface;
	LilvNode* work_schedule;
	LilvNode* end;  ///< NULL terminator for easy freeing of entire structure
} JalvNodes;

typedef enum {
	JALV_RUNNING,
	JALV_PAUSE_REQUESTED,
	JALV_PAUSED
} JalvPlayState;

typedef struct {
	jack_ringbuffer_t*          requests;   ///< Requests to the worker
	jack_ringbuffer_t*          responses;  ///< Responses from the worker
	void*                       response;   ///< Worker response buffer
	ZixSem                      sem;        ///< Worker semaphore
	ZixThread                   thread;     ///< Worker thread
	const LV2_Worker_Interface* iface;      ///< Plugin worker interface
} JalvWorker;

typedef struct {
	JalvOptions        opts;           ///< Command-line options
	JalvURIDs          urids;          ///< URIDs
	JalvNodes          nodes;          ///< Nodes
	LV2_Atom_Forge     forge;          ///< Atom forge
	const char*        prog_name;      ///< Program name (argv[0])
	LilvWorld*         world;          ///< Lilv World
	LV2_URID_Map       map;            ///< URI => Int map
	LV2_URID_Unmap     unmap;          ///< Int => URI map
	Sratom*            sratom;         ///< Atom serialiser
	Sratom*            ui_sratom;      ///< Atom serialiser for UI thread
	Symap*             symap;          ///< URI map
	ZixSem             symap_lock;     ///< Lock for URI map
	jack_client_t*     jack_client;    ///< Jack client
	jack_ringbuffer_t* ui_events;      ///< Port events from UI
	jack_ringbuffer_t* plugin_events;  ///< Port events from plugin
	void*              ui_event_buf;   ///< Buffer for reading UI port events
	JalvWorker         worker;         ///< Worker thread implementation
	ZixSem*            done;           ///< Exit semaphore
	ZixSem             paused;         ///< Paused signal from process thread
	JalvPlayState      play_state;     ///< Current play state
	char*              temp_dir;       ///< Temporary plugin state directory
	char*              save_dir;       ///< Plugin save directory
	const LilvPlugin*  plugin;         ///< Plugin class (RDF data)
	LilvUIs*           uis;            ///< All plugin UIs (RDF data)
	const LilvUI*      ui;             ///< Plugin UI (RDF data)
	const LilvNode*    ui_type;        ///< Plugin UI type (unwrapped)
	LilvInstance*      instance;       ///< Plugin instance (shared library)
	SuilHost*          ui_host;        ///< Plugin UI host support
	SuilInstance*      ui_instance;    ///< Plugin UI instance (shared library)
	void*              window;         ///< Window (if applicable)
	struct Port*       ports;          ///< Port array of size num_ports
	uint32_t           block_length;   ///< Jack buffer size (block length)
	size_t             midi_buf_size;  ///< Size of MIDI port buffers
	uint32_t           control_in;     ///< Index of control input port
	uint32_t           num_ports;      ///< Size of the two following arrays:
	uint32_t           longest_sym;    ///< Longest port symbol
	uint32_t           plugin_latency; ///< Latency reported by plugin (if any)
	float              ui_update_hz;   ///< Frequency of UI updates
	jack_nframes_t     sample_rate;    ///< Sample rate
	jack_nframes_t     event_delta_t;  ///< Frames since last update sent to UI
	uint32_t           midi_event_id;  ///< MIDI event class ID in event context
	jack_nframes_t     position;       ///< Transport position in frames
	float              bpm;            ///< Transport tempo in beats per minute
	bool               rolling;        ///< Transport speed (0=stop, 1=play)
	bool               buf_size_set;   ///< True iff buffer size callback fired
	bool               exit;           ///< True iff execution is finished
	bool               has_ui;         ///< True iff a control UI is present
} Jalv;

int
jalv_init(int* argc, char*** argv, JalvOptions* opts);

void
jalv_create_ports(Jalv* jalv);

struct Port*
jalv_port_by_symbol(Jalv* jalv, const char* sym);

const char*
jalv_native_ui_type(Jalv* jalv);

int
jalv_open_ui(Jalv* jalv);

int
jalv_close_ui(Jalv* jalv);

void
jalv_ui_instantiate(Jalv*       jalv,
                    const char* native_ui_type,
                    void*       parent);

bool
jalv_ui_is_resizable(Jalv* jalv);

void
jalv_ui_write(SuilController controller,
              uint32_t       port_index,
              uint32_t       buffer_size,
              uint32_t       protocol,
              const void*    buffer);

uint32_t
jalv_ui_port_index(SuilController controller, const char* port_symbol);

void
jalv_ui_port_event(Jalv*       jalv,
                   uint32_t    port_index,
                   uint32_t    buffer_size,
                   uint32_t    protocol,
                   const void* buffer);

bool
jalv_update(Jalv* jalv);

int
jalv_ui_resize(Jalv* jalv, int width, int height);

typedef int (*PresetSink)(Jalv*           jalv,
                          const LilvNode* node,
                          const LilvNode* title,
                          void*           data);

int
jalv_load_presets(Jalv* jalv, PresetSink sink, void* data);

int
jalv_unload_presets(Jalv* jalv);

int
jalv_apply_preset(Jalv* jalv, const LilvNode* preset);

int
jalv_save_preset(Jalv*       jalv,
                 const char* dir,
                 const char* uri,
                 const char* label,
                 const char* filename);

void
jalv_save(Jalv* jalv, const char* dir);

void
jalv_save_port_values(Jalv*           jalv,
                      SerdWriter*     writer,
                      const SerdNode* subject);
char*
jalv_make_path(LV2_State_Make_Path_Handle handle,
               const char*                path);

void
jalv_apply_state(Jalv* jalv, LilvState* state);

char*
atom_to_turtle(LV2_URID_Unmap* unmap,
               const SerdNode* subject,
               const SerdNode* predicate,
               const LV2_Atom* atom);

static inline char*
jalv_strdup(const char* str)
{
	const size_t len  = strlen(str);
	char*        copy = (char*)malloc(len + 1);
	memcpy(copy, str, len + 1);
	return copy;
}

static inline char*
jalv_strjoin(const char* a, const char* b)
{
	const size_t a_len = strlen(a);
	const size_t b_len = strlen(b);
	char* const  out   = (char*)malloc(a_len + b_len + 1);

	memcpy(out,         a, a_len);
	memcpy(out + a_len, b, b_len);
	out[a_len + b_len] = '\0';

	return out;
}

int
jalv_printf(LV2_Log_Handle handle,
            LV2_URID       type,
            const char*    fmt, ...);

int
jalv_vprintf(LV2_Log_Handle handle,
             LV2_URID       type,
             const char*    fmt,
             va_list        ap);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // JALV_INTERNAL_H
