// Copyright 2016-2025 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_PROCESS_SETUP_H
#define JALV_PROCESS_SETUP_H

#include "attributes.h"
#include "mapper.h"
#include "nodes.h"
#include "process.h"
#include "settings.h"
#include "urids.h"

#include <lilv/lilv.h>

#include <stdbool.h>
#include <stdint.h>

// Code for setting up the realtime process thread (but that isn't used in it)
JALV_BEGIN_DECLS

/**
   Initialize process thread and allocate necessary structures.

   This only initializes the state structure, it doesn't create any threads or
   start plugin execution.
*/
int
jalv_process_init(JalvProcess*     proc,
                  const JalvURIDs* urids,
                  JalvMapper*      mapper,
                  uint32_t         update_frames,
                  bool             trace);

/**
   Clean up process thread.

   This frees everything allocated by jalv_process_init() and
   jalv_process_activate().
*/
void
jalv_process_cleanup(JalvProcess* proc);

/**
   Allocate necessary buffers, connect the plugin to them, and prepare to run.

   @param proc Process thread state.
   @param urids Application vocabulary.
   @param instance Plugin instance to run.
   @param settings Process thread settings.
*/
void
jalv_process_activate(JalvProcess*        proc,
                      const JalvURIDs*    urids,
                      LilvInstance*       instance,
                      const JalvSettings* settings);

/**
   Clean up after jalv_process_activate() and disconnect plugin.

   @param proc Process thread state.
*/
void
jalv_process_deactivate(JalvProcess* proc);

/**
   Initialize the process thread state for a port.

   @return Zero on success.
*/
int
jalv_process_port_init(JalvProcessPort*  port,
                       const JalvNodes*  nodes,
                       const LilvPlugin* plugin,
                       const LilvPort*   lilv_port);

/**
   Free memory allocated by jalv_setup_init_port().
*/
void
jalv_process_port_cleanup(JalvProcessPort* port);

JALV_END_DECLS

#endif // JALV_PROCESS_SETUP_H
