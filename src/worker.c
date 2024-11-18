// Copyright 2007-2022 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "worker.h"

#include <lv2/core/lv2.h>
#include <lv2/worker/worker.h>
#include <zix/ring.h>
#include <zix/sem.h>
#include <zix/status.h>
#include <zix/thread.h>

#include <stdio.h>
#include <stdlib.h>

#define MAX_PACKET_SIZE 4096U

typedef enum {
  STATE_SINGLE_THREADED, ///< Single-threaded worker (only state)
  STATE_STOPPED,         ///< Thread is exited
  STATE_LAUNCHED,        ///< Thread is running
  STATE_MUST_EXIT,       ///< Thread exit requested
} WorkerState;

struct JalvWorkerImpl {
  ZixRing*                    requests;  ///< Requests to the worker
  ZixRing*                    responses; ///< Responses from the worker
  void*                       response;  ///< Worker response buffer
  ZixSem*                     lock;      ///< Lock for plugin work() method
  ZixSem                      sem;       ///< Worker semaphore
  WorkerState                 state;     ///< Worker state
  ZixThread                   thread;    ///< Worker thread
  LV2_Handle                  handle;    ///< Plugin handle
  const LV2_Worker_Interface* iface;     ///< Plugin worker interface
};

static LV2_Worker_Status
jalv_worker_write_packet(ZixRing* const    target,
                         const uint32_t    size,
                         const void* const data)
{
  ZixRingTransaction tx = zix_ring_begin_write(target);
  if (zix_ring_amend_write(target, &tx, &size, sizeof(size)) ||
      zix_ring_amend_write(target, &tx, data, size)) {
    return LV2_WORKER_ERR_NO_SPACE;
  }

  zix_ring_commit_write(target, &tx);
  return LV2_WORKER_SUCCESS;
}

static LV2_Worker_Status
jalv_worker_respond(LV2_Worker_Respond_Handle handle,
                    const uint32_t            size,
                    const void*               data)
{
  return jalv_worker_write_packet(((JalvWorker*)handle)->responses, size, data);
}

static void*
worker_func(void* const data)
{
  JalvWorker* const worker = (JalvWorker*)data;
  void*             buf    = NULL;

  while (true) {
    // Wait for a request
    zix_sem_wait(&worker->sem);
    if (worker->state == STATE_MUST_EXIT) {
      break;
    }

    // Read the size header of the request
    uint32_t size = 0;
    zix_ring_read(worker->requests, &size, sizeof(size));

    // Reallocate buffer to accommodate request if necessary
    void* const new_buf = realloc(buf, size);
    if (new_buf) {
      // Read request into buffer
      buf = new_buf;
      zix_ring_read(worker->requests, buf, size);

      // Lock and dispatch request to plugin's work handler
      zix_sem_wait(worker->lock);
      worker->iface->work(
        worker->handle, jalv_worker_respond, worker, size, buf);
      zix_sem_post(worker->lock);

    } else {
      // Reallocation failed, skip request to avoid corrupting ring
      zix_ring_skip(worker->requests, size);
    }
  }

  free(buf);
  worker->state = STATE_STOPPED;
  return NULL;
}

JalvWorker*
jalv_worker_new(ZixSem* const lock, const bool threaded)
{
  JalvWorker* const worker    = (JalvWorker*)calloc(1, sizeof(JalvWorker));
  ZixRing* const    requests  = zix_ring_new(NULL, MAX_PACKET_SIZE);
  ZixRing* const    responses = zix_ring_new(NULL, MAX_PACKET_SIZE);
  void* const       response  = calloc(1, MAX_PACKET_SIZE);

  if (worker && responses && response) {
    worker->requests  = requests;
    worker->responses = responses;
    worker->response  = response;
    worker->lock      = lock;
    worker->state     = threaded ? STATE_STOPPED : STATE_SINGLE_THREADED;

    zix_ring_mlock(requests);
    zix_ring_mlock(responses);
    return worker;
  }

  free(response);
  zix_ring_free(responses);
  zix_ring_free(requests);
  free(worker);
  return NULL;
}

void
jalv_worker_free(JalvWorker* const worker)
{
  if (worker) {
    jalv_worker_exit(worker);
    zix_ring_free(worker->requests);
    zix_ring_free(worker->responses);
    free(worker->response);
    free(worker);
  }
}

ZixStatus
jalv_worker_launch(JalvWorker* const worker)
{
  ZixStatus st = ZIX_STATUS_SUCCESS;
  if (worker->state == STATE_STOPPED) {
    if ((st = zix_sem_init(&worker->sem, 0))) {
      return st;
    }

    if ((st = zix_thread_create(&worker->thread, 4096U, worker_func, worker))) {
      zix_sem_destroy(&worker->sem);
      return st;
    }

    worker->state = STATE_LAUNCHED;
  }
  return ZIX_STATUS_SUCCESS;
}

void
jalv_worker_exit(JalvWorker* const worker)
{
  if (worker && worker->state == STATE_LAUNCHED) {
    worker->state = STATE_MUST_EXIT;
    zix_sem_post(&worker->sem);
    zix_thread_join(worker->thread);
  }
}

void
jalv_worker_attach(JalvWorker* const                 worker,
                   const LV2_Worker_Interface* const iface,
                   LV2_Handle                        handle)
{
  if (worker) {
    worker->iface  = iface;
    worker->handle = handle;
  }
}

LV2_Worker_Status
jalv_worker_schedule(LV2_Worker_Schedule_Handle handle,
                     const uint32_t             size,
                     const void* const          data)
{
  JalvWorker* const worker = (JalvWorker*)handle;
  LV2_Worker_Status st     = LV2_WORKER_SUCCESS;

  if (!worker || !size || worker->state == STATE_STOPPED) {
    st = LV2_WORKER_ERR_UNKNOWN;

  } else if (worker->state == STATE_LAUNCHED) {
    // Schedule a request to be executed by the worker thread
    if (!(st = jalv_worker_write_packet(worker->requests, size, data))) {
      zix_sem_post(&worker->sem);
    }

  } else if (worker->state == STATE_SINGLE_THREADED) {
    // Execute work immediately in this thread
    zix_sem_wait(worker->lock);
    st = worker->iface->work(
      worker->handle, jalv_worker_respond, worker, size, data);
    zix_sem_post(worker->lock);
  }

  return st;
}

void
jalv_worker_emit_responses(JalvWorker* const worker, LV2_Handle lv2_handle)
{
  static const uint32_t size_size = (uint32_t)sizeof(uint32_t);

  if (worker && worker->responses) {
    uint32_t size = 0U;
    while (zix_ring_read(worker->responses, &size, size_size) == size_size) {
      if (zix_ring_read(worker->responses, worker->response, size) == size) {
        worker->iface->work_response(lv2_handle, size, worker->response);
      }
    }
  }
}

void
jalv_worker_end_run(JalvWorker* const worker)
{
  if (worker && worker->iface && worker->iface->end_run) {
    worker->iface->end_run(worker->handle);
  }
}
