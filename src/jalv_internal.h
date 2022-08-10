// Copyright 2007-2022 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_INTERNAL_H
#define JALV_INTERNAL_H

#include "control.h"
#include "jalv_config.h"
#include "log.h"
#include "nodes.h"
#include "options.h"
#include "symap.h"
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

#ifdef __cplusplus
extern "C" {
#endif

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

struct JalvImpl {
  JalvOptions       opts;         ///< Command-line options
  JalvURIDs         urids;        ///< URIDs
  JalvNodes         nodes;        ///< Nodes
  JalvLog           log;          ///< Log for error/warning/debug messages
  LV2_Atom_Forge    forge;        ///< Atom forge
  LilvWorld*        world;        ///< Lilv World
  LV2_URID_Map      map;          ///< URI => Int map
  LV2_URID_Unmap    unmap;        ///< Int => URI map
  SerdEnv*          env;          ///< Environment for RDF printing
  Sratom*           sratom;       ///< Atom serialiser
  Sratom*           ui_sratom;    ///< Atom serialiser for UI thread
  Symap*            symap;        ///< URI map
  ZixSem            symap_lock;   ///< Lock for URI map
  JalvBackend*      backend;      ///< Audio system backend
  ZixRing*          ui_to_plugin; ///< Port events from UI
  ZixRing*          plugin_to_ui; ///< Port events from plugin
  void*             ui_event_buf; ///< Buffer for reading UI port events
  JalvWorker        worker;       ///< Worker thread implementation
  JalvWorker        state_worker; ///< Synchronous worker for state restore
  ZixSem            work_lock;    ///< Lock for plugin work() method
  ZixSem            done;         ///< Exit semaphore
  ZixSem            paused;       ///< Paused signal from process thread
  JalvPlayState     play_state;   ///< Current play state
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
  struct Port*        ports;           ///< Port array of size num_ports
  Controls            controls;        ///< Available plugin controls
  uint32_t            block_length;    ///< Audio buffer size (block length)
  size_t              midi_buf_size;   ///< Size of MIDI port buffers
  uint32_t            control_in;      ///< Index of control input port
  uint32_t            num_ports;       ///< Size of the two following arrays:
  uint32_t            plugin_latency;  ///< Latency reported by plugin (if any)
  float               ui_update_hz;    ///< Frequency of UI updates
  float               ui_scale_factor; ///< UI scale factor
  float               sample_rate;     ///< Sample rate
  uint32_t            event_delta_t;   ///< Frames since last update sent to UI
  uint32_t            position;        ///< Transport position in frames
  float               bpm;             ///< Transport tempo in beats per minute
  bool                rolling;         ///< Transport speed (0=stop, 1=play)
  bool                buf_size_set;    ///< True iff buffer size callback fired
  bool                exit;            ///< True iff execution is finished
  bool                has_ui;          ///< True iff a control UI is present
  bool                request_update;  ///< True iff a plugin update is needed
  bool                safe_restore;    ///< Plugin restore() is thread-safe
  JalvFeatures        features;
  const LV2_Feature** feature_list;
};

int
jalv_open(Jalv* jalv, int* argc, char*** argv);

int
jalv_close(Jalv* jalv);

void
jalv_create_ports(Jalv* jalv);

void
jalv_allocate_port_buffers(Jalv* jalv);

struct Port*
jalv_port_by_symbol(Jalv* jalv, const char* sym);

void
jalv_create_controls(Jalv* jalv, bool writable);

ControlID*
jalv_control_by_symbol(Jalv* jalv, const char* sym);

void
jalv_set_control(Jalv*            jalv,
                 const ControlID* control,
                 uint32_t         size,
                 LV2_URID         type,
                 const void*      body);

void
jalv_init_ui(Jalv* jalv);

void
jalv_ui_instantiate(Jalv* jalv, const char* native_ui_type, void* parent);

bool
jalv_ui_is_resizable(Jalv* jalv);

void
jalv_send_to_plugin(void*       jalv_handle,
                    uint32_t    port_index,
                    uint32_t    buffer_size,
                    uint32_t    protocol,
                    const void* buffer);

void
jalv_apply_ui_events(Jalv* jalv, uint32_t nframes);

void
jalv_ui_port_event(Jalv*       jalv,
                   uint32_t    port_index,
                   uint32_t    buffer_size,
                   uint32_t    protocol,
                   const void* buffer);

bool
jalv_send_to_ui(Jalv*       jalv,
                uint32_t    port_index,
                uint32_t    type,
                uint32_t    size,
                const void* body);

/**
   Write a control port change using the default (0) protocol.

   This is used to transfer control port value changes between the plugin and
   UI.

   @param jalv Jalv instance.
   @param target Communication ring (jalv->plugin_to_ui or jalv->ui_to_plugin).
   @param port_index Index of the port this change is for.
   @param value New control port value.
   @return 0 on success, non-zero on failure (overflow).
*/
int
jalv_write_control(Jalv*    jalv,
                   ZixRing* target,
                   uint32_t port_index,
                   float    value);

void
jalv_dump_atom(Jalv*           jalv,
               FILE*           stream,
               const char*     label,
               const LV2_Atom* atom,
               int             color);

bool
jalv_run(Jalv* jalv, uint32_t nframes);

int
jalv_update(Jalv* jalv);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // JALV_INTERNAL_H
