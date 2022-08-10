// Copyright 2007-2022 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "worker.h"

#include "lv2/core/lv2.h"
#include "lv2/worker/worker.h"
#include "zix/ring.h"
#include "zix/sem.h"
#include "zix/thread.h"

#include <stdio.h>
#include <stdlib.h>

#define MAX_PACKET_SIZE 4096U

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
    if (*worker->exit) {
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
  return NULL;
}

void
jalv_worker_init(JalvWorker* const                 worker,
                 const LV2_Worker_Interface* const iface,
                 const bool                        threaded)
{
  worker->iface     = iface;
  worker->threaded  = threaded;
  worker->responses = zix_ring_new(NULL, MAX_PACKET_SIZE);
  worker->response  = malloc(MAX_PACKET_SIZE);

  if (threaded) {
    worker->requests = zix_ring_new(NULL, MAX_PACKET_SIZE);

    zix_thread_create(&worker->thread, 4096U, worker_func, worker);
    zix_ring_mlock(worker->requests);
  }

  zix_ring_mlock(worker->responses);
}

void
jalv_worker_finish(JalvWorker* const worker)
{
  if (worker->threaded) {
    zix_sem_post(&worker->sem);
    zix_thread_join(worker->thread, NULL);
  }
}

void
jalv_worker_destroy(JalvWorker* const worker)
{
  zix_ring_free(worker->requests);
  zix_ring_free(worker->responses);
  free(worker->response);
}

LV2_Worker_Status
jalv_worker_schedule(LV2_Worker_Schedule_Handle handle,
                     const uint32_t             size,
                     const void* const          data)
{
  JalvWorker*       worker = (JalvWorker*)handle;
  LV2_Worker_Status st     = LV2_WORKER_SUCCESS;

  if (!size) {
    return LV2_WORKER_ERR_UNKNOWN;
  }

  if (worker->threaded) {
    // Schedule a request to be executed by the worker thread
    if (!(st = jalv_worker_write_packet(worker->requests, size, data))) {
      zix_sem_post(&worker->sem);
    }

  } else {
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

  if (worker->responses) {
    uint32_t size = 0U;
    while (zix_ring_read(worker->responses, &size, size_size) == size_size) {
      if (zix_ring_read(worker->responses, worker->response, size) == size) {
        worker->iface->work_response(lv2_handle, size, worker->response);
      }
    }
  }
}
