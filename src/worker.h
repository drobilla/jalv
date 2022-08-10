// Copyright 2007-2022 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_WORKER_H
#define JALV_WORKER_H

#include "zix/ring.h"
#include "zix/sem.h"
#include "zix/thread.h"

#include "lv2/core/lv2.h"
#include "lv2/worker/worker.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Worker for running non-realtime tasks for plugins

typedef struct {
  ZixRing*                    requests;  ///< Requests to the worker
  ZixRing*                    responses; ///< Responses from the worker
  void*                       response;  ///< Worker response buffer
  ZixSem*                     lock;      ///< Lock for plugin work() method
  bool*                       exit;      ///< Pointer to exit flag
  ZixSem                      sem;       ///< Worker semaphore
  ZixThread                   thread;    ///< Worker thread
  LV2_Handle                  handle;    ///< Plugin handle
  const LV2_Worker_Interface* iface;     ///< Plugin worker interface
  bool                        threaded;  ///< Run work in another thread
} JalvWorker;

void
jalv_worker_init(JalvWorker*                 worker,
                 const LV2_Worker_Interface* iface,
                 bool                        threaded);

void
jalv_worker_finish(JalvWorker* worker);

void
jalv_worker_destroy(JalvWorker* worker);

LV2_Worker_Status
jalv_worker_schedule(LV2_Worker_Schedule_Handle handle,
                     uint32_t                   size,
                     const void*                data);

void
jalv_worker_emit_responses(JalvWorker* worker, LV2_Handle lv2_handle);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // JALV_WORKER_H
