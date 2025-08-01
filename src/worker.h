// Copyright 2007-2025 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_WORKER_H
#define JALV_WORKER_H

#include "attributes.h"

#include <lv2/core/lv2.h>
#include <lv2/worker/worker.h>
#include <zix/attributes.h>
#include <zix/sem.h>
#include <zix/status.h>

#include <stdbool.h>
#include <stdint.h>

// Worker implementation
JALV_BEGIN_DECLS

/**
   A worker for running non-realtime tasks for plugins.

   The worker can be used in threaded mode, which allows non-realtime work to be
   done with latency when running realtime, or non-threaded mode, which performs
   work immediately for state restoration or offline rendering.
*/
typedef struct JalvWorkerImpl JalvWorker;

/**
   Allocate a new worker and launch its thread if necessary.

   @param lock Pointer to lock used to guard doing work.
   @param threaded If true, launch a thread to perform work in.
   @return A newly allocated worker, or null on error.
*/
JalvWorker*
jalv_worker_new(ZixSem* lock, bool threaded);

/**
   Free a worker allocated with jalv_worker_new().

   Calls jalv_worker_exit() to terminate the running thread if necessary.
*/
void
jalv_worker_free(JalvWorker* worker);

/**
   Launch the worker's thread.

   For threaded workers, this launches the thread if it isn't already running.
   For non-threaded workers, this does nothing.

   @return Zero on success, or a non-zero error code if launching failed.
*/
ZixStatus
jalv_worker_launch(JalvWorker* worker);

/**
   Terminate the worker's thread if necessary.

   For threaded workers, this blocks until the thread has exited.  For
   non-threaded workers, this does nothing.
*/
void
jalv_worker_exit(JalvWorker* worker);

/**
   Attach the worker to a plugin instance.

   This must be called before scheduling any work.

   @param worker Worker to activate and attach to a plugin.
   @param iface Worker interface from plugin.
   @param handle Handle to the LV2 plugin this worker is for.
*/
void
jalv_worker_attach(JalvWorker*                 worker,
                   const LV2_Worker_Interface* iface,
                   LV2_Handle                  handle);

/**
   Schedule work to be performed by the worker in the audio thread.

   For threaded workers, this enqueues a request that will be handled by the
   worker thread asynchronously.  For non-threaded workers, the work is
   performed immediately before returning.
*/
ZIX_REALTIME LV2_Worker_Status
jalv_worker_schedule(LV2_Worker_Schedule_Handle handle,
                     uint32_t                   size,
                     const void*                data);

/**
   Emit any pending responses to the plugin in the audio thread.

   This dispatches responses from work that has been completed since the last
   call, so the plugin knows it is finished and can apply the changes.
*/
ZIX_REALTIME void
jalv_worker_emit_responses(JalvWorker* worker, LV2_Handle lv2_handle);

/**
   Notify the plugin that the run() cycle is finished.

   This must be called near the end of every cycle, after any potential calls
   to jalv_worker_emit_responses().
*/
ZIX_REALTIME void
jalv_worker_end_run(JalvWorker* worker);

JALV_END_DECLS

#endif // JALV_WORKER_H
