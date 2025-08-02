// Copyright 2016-2025 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_PROCESS_H
#define JALV_PROCESS_H

#include "attributes.h"
#include "lv2_evbuf.h"
#include "types.h"
#include "worker.h"

#include <lilv/lilv.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <zix/attributes.h>
#include <zix/ring.h>
#include <zix/sem.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Code and data used in the realtime process thread
JALV_BEGIN_DECLS

typedef enum {
  JALV_PROCESS_SUCCESS,
  JALV_PROCESS_SEND_UPDATES,
  JALV_PROCESS_BAD_HEADER,
  JALV_PROCESS_BAD_CONTROL_VALUE,
  JALV_PROCESS_BAD_EVENT,
  JALV_PROCESS_BAD_STATE_CHANGE,
  JALV_PROCESS_BAD_MESSAGE_TYPE,
} JalvProcessStatus;

/// Port state used in the process thread
typedef struct {
  PortType   type;            ///< Data type
  PortFlow   flow;            ///< Data flow direction
  void*      sys_port;        ///< For audio/MIDI ports, otherwise NULL
  char*      symbol;          ///< Port symbol (stable/unique C-like identifier)
  char*      label;           ///< Human-readable label
  LV2_Evbuf* evbuf;           ///< Sequence port event buffer
  uint32_t   buf_size;        ///< Custom buffer size, or 0
  bool       reports_latency; ///< Whether control port reports latency
  bool       is_primary;      ///< True for main control/response channel
  bool       is_bpm;          ///< True if port is a BPM control port
  bool       supports_midi;   ///< Whether event port supports MIDI
  bool       supports_pos;    ///< Whether event port supports Position
} JalvProcessPort;

/// Transport state used in the process thread
typedef struct {
  uint32_t position; ///< Transport position in frames
  float    bpm;      ///< Transport tempo in beats per minute
  bool     rolling;  ///< Transport speed (0=stop, 1=play)
} JalvPosition;

/**
   State accessed in the process thread.

   Everything accessed by the process thread is stored here, to keep it
   somewhat insulated from the UI and to make references to it stand out in the
   code.
*/
typedef struct {
  LilvInstance*    instance;         ///< Plugin instance
  ZixRing*         ui_to_plugin;     ///< Messages from UI to plugin/process
  ZixRing*         plugin_to_ui;     ///< Messages from plugin/process to UI
  JalvWorker*      worker;           ///< Worker thread implementation
  JalvWorker*      state_worker;     ///< Synchronous worker for state restore
  JalvProcessPort* ports;            ///< Port array of size num_ports
  LV2_Atom_Forge   forge;            ///< Atom forge
  LV2_Atom_Object  get_msg;          ///< General patch:Get message
  float*           controls_buf;     ///< Control port buffers array
  size_t           process_msg_size; ///< Maximum size of a single message
  void*            process_msg;      ///< Buffer for receiving messages
  ZixSem           paused;           ///< Paused signal from process thread
  JalvRunState     run_state;        ///< Current run state
  uint32_t         control_in;       ///< Index of control input port
  uint32_t         num_ports;        ///< Total number of ports on the plugin
  uint32_t         pending_frames;   ///< Frames since last UI update sent
  uint32_t         update_frames;    ///< UI update period in frames, or zero
  uint32_t         plugin_latency;   ///< Latency reported by plugin (if any)
  JalvPosition     transport;        ///< Transport state
  bool             has_ui;           ///< True iff a control UI is present
  bool             trace;            ///< Print debug trace messages
} JalvProcess;

/**
   Run the plugin for a block of frames.

   Applies any pending messages from the UI, runs the plugin instance, and
   processes any worker replies.

   @param proc Process thread state.
   @param nframes Number of frames to process.
   @return Whether output value updates should be sent to the UI now.
*/
ZIX_REALTIME JalvProcessStatus
jalv_run(JalvProcess* proc, uint32_t nframes);

/**
   Bypass the plugin for a block of frames.

   This is like jalv_run(), but doesn't actually run the plugin and only does
   the minimum necessary internal work for the cycle.

   @param proc Process thread state.
   @param nframes Number of frames to bypass.
   @return Zero.
*/
ZIX_REALTIME int
jalv_bypass(JalvProcess* proc, uint32_t nframes);

JALV_END_DECLS

#endif // JALV_PROCESS_H
