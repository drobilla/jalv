// Copyright 2007-2022 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_INTERNAL_H
#define JALV_INTERNAL_H

#include "attributes.h"
#include "control.h"
#include "jalv_config.h"
#include "log.h"
#include "mapper.h"
#include "nodes.h"
#include "options.h"
#include "types.h"
#include "urids.h"
#include "worker.h"

#include "zix/ring.h"
#include "zix/sem.h"

#include "lilv/lilv.h"
#include "serd/serd.h"
#include "sratom/sratom.h"
#if USE_SUIL
#  include "suil/suil.h"
#endif

#include "lv2/atom/forge.h"
#include "lv2/core/lv2.h"
#include "lv2/data-access/data-access.h"
#include "lv2/log/log.h"
#include "lv2/options/options.h"
#include "lv2/state/state.h"
#include "lv2/ui/ui.h"
#include "lv2/urid/urid.h"
#include "lv2/worker/worker.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

// "Shared" internal application declarations
JALV_BEGIN_DECLS

/// LV2 features and associated data to be passed to plugins
typedef struct {
  LV2_Feature                map_feature;
  LV2_Feature                unmap_feature;
  LV2_State_Make_Path        make_path;
  LV2_Feature                make_path_feature;
  LV2_Worker_Schedule        sched;
  LV2_Feature                sched_feature;
  LV2_Worker_Schedule        ssched;
  LV2_Feature                state_sched_feature;
  LV2_Log_Log                llog;
  LV2_Feature                log_feature;
  LV2_Options_Option         options[7];
  LV2_Feature                options_feature;
  LV2_Feature                safe_restore_feature;
  LV2UI_Request_Value        request_value;
  LV2_Feature                request_value_feature;
  LV2_Extension_Data_Feature ext_data;
} JalvFeatures;

/// Internal application state
struct JalvImpl {
  JalvOptions       opts;         ///< Command-line options
  LilvWorld*        world;        ///< Lilv World
  JalvMapper*       mapper;       ///< URI mapper/unmapper
  JalvURIDs         urids;        ///< URIDs
  JalvNodes         nodes;        ///< Nodes
  JalvLog           log;          ///< Log for error/warning/debug messages
  LV2_Atom_Forge    forge;        ///< Atom forge
  SerdEnv*          env;          ///< Environment for RDF printing
  Sratom*           sratom;       ///< Atom serialiser
  JalvBackend*      backend;      ///< Audio system backend
  ZixRing*          ui_to_plugin; ///< Port events from UI
  ZixRing*          plugin_to_ui; ///< Port events from plugin
  void*             audio_msg;    ///< Buffer for messages in the audio thread
  void*             ui_msg;       ///< Buffer for messages in the UI thread
  JalvWorker*       worker;       ///< Worker thread implementation
  JalvWorker*       state_worker; ///< Synchronous worker for state restore
  ZixSem            work_lock;    ///< Lock for plugin work() method
  ZixSem            done;         ///< Exit semaphore
  ZixSem            paused;       ///< Paused signal from process thread
  JalvRunState      run_state;    ///< Current process thread run state
  char*             temp_dir;     ///< Temporary plugin state directory
  char*             save_dir;     ///< Plugin save directory
  const LilvPlugin* plugin;       ///< Plugin class (RDF data)
  LilvState*        preset;       ///< Current preset
  LilvUIs*          uis;          ///< All plugin UIs (RDF data)
  const LilvUI*     ui;           ///< Plugin UI (RDF data)
  const LilvNode*   ui_type;      ///< Plugin UI type (unwrapped)
  LilvInstance*     instance;     ///< Plugin instance (shared library)
#if USE_SUIL
  SuilHost*     ui_host;     ///< Plugin UI host support
  SuilInstance* ui_instance; ///< Plugin UI instance (shared library)
#endif
  void*               window;          ///< Window (if applicable)
  JalvPort*           ports;           ///< Port array of size num_ports
  Controls            controls;        ///< Available plugin controls
  float*              controls_buf;    ///< Control port buffers array
  uint32_t            block_length;    ///< Audio buffer size (block length)
  size_t              midi_buf_size;   ///< Size of MIDI port buffers
  size_t              msg_buf_size;    ///< Maximum size of a single message
  uint32_t            control_in;      ///< Index of control input port
  uint32_t            num_ports;       ///< Total number of ports on the plugin
  uint32_t            plugin_latency;  ///< Latency reported by plugin (if any)
  float               ui_update_hz;    ///< Frequency of UI updates
  float               ui_scale_factor; ///< UI scale factor
  float               sample_rate;     ///< Sample rate
  uint32_t            event_delta_t;   ///< Frames since last update sent to UI
  uint32_t            position;        ///< Transport position in frames
  float               bpm;             ///< Transport tempo in beats per minute
  bool                rolling;         ///< Transport speed (0=stop, 1=play)
  bool                buf_size_set;    ///< True iff buffer size callback fired
  bool                has_ui;          ///< True iff a control UI is present
  bool                safe_restore;    ///< Plugin restore() is thread-safe
  JalvFeatures        features;
  const LV2_Feature** feature_list;
};

/// Load the plugin and set up the application
int
jalv_open(Jalv* jalv, int* argc, char*** argv);

/// Shut down the application (counterpart to jalv_open)
int
jalv_close(Jalv* jalv);

/// Allocate appropriately-sized port buffers and connect the plugin to them
void
jalv_allocate_port_buffers(Jalv* jalv);

/// Find a port by symbol
JalvPort*
jalv_port_by_symbol(Jalv* jalv, const char* sym);

/// Set a control to the given value
void
jalv_set_control(Jalv*            jalv,
                 const ControlID* control,
                 uint32_t         size,
                 LV2_URID         type,
                 const void*      body);

/// Request and/or set initial control values to initialize the UI
void
jalv_init_ui(Jalv* jalv);

/// Instantiate the UI instance using suil if available
void
jalv_ui_instantiate(Jalv* jalv, const char* native_ui_type, void* parent);

/// Return true if the plugin UI isn't declared as non-resizable
bool
jalv_ui_is_resizable(Jalv* jalv);

/// Dump an atom to stdout in a "developer-readable" format (Turtle)
void
jalv_dump_atom(Jalv*           jalv,
               FILE*           stream,
               const char*     label,
               const LV2_Atom* atom,
               int             color);

/// Periodically update user interface
int
jalv_update(Jalv* jalv);

JALV_END_DECLS

#endif // JALV_INTERNAL_H
