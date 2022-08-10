// Copyright 2007-2016 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "worker.h"

#include "lv2/core/lv2.h"
#include "lv2/worker/worker.h"
#include "zix/ring.h"
#include "zix/sem.h"
#include "zix/thread.h"

#include <stdio.h>
#include <stdlib.h>

static LV2_Worker_Status
jalv_worker_respond(LV2_Worker_Respond_Handle handle,
                    uint32_t                  size,
                    const void*               data)
{
  JalvWorker* worker = (JalvWorker*)handle;
  zix_ring_write(worker->responses, (const char*)&size, sizeof(size));
  zix_ring_write(worker->responses, (const char*)data, size);
  return LV2_WORKER_SUCCESS;
}

static void*
worker_func(void* data)
{
  JalvWorker* worker = (JalvWorker*)data;
  void*       buf    = NULL;
  while (true) {
    zix_sem_wait(&worker->sem);
    if (*worker->exit) {
      break;
    }

    uint32_t size = 0;
    zix_ring_read(worker->requests, &size, sizeof(size));

    void* const new_buf = realloc(buf, size);
    if (!new_buf) {
      break;
    }

    buf = new_buf;
    zix_ring_read(worker->requests, buf, size);

    zix_sem_wait(worker->lock);
    worker->iface->work(worker->handle, jalv_worker_respond, worker, size, buf);
    zix_sem_post(worker->lock);
  }

  free(buf);
  return NULL;
}

void
jalv_worker_init(JalvWorker*                 worker,
                 const LV2_Worker_Interface* iface,
                 bool                        threaded)
{
  worker->iface    = iface;
  worker->threaded = threaded;
  if (threaded) {
    zix_thread_create(&worker->thread, 4096, worker_func, worker);
    worker->requests = zix_ring_new(4096);
    zix_ring_mlock(worker->requests);
  }
  worker->responses = zix_ring_new(4096);
  worker->response  = malloc(4096);
  zix_ring_mlock(worker->responses);
}

void
jalv_worker_finish(JalvWorker* worker)
{
  if (worker->threaded) {
    zix_sem_post(&worker->sem);
    zix_thread_join(worker->thread, NULL);
  }
}

void
jalv_worker_destroy(JalvWorker* worker)
{
  zix_ring_free(worker->requests);
  zix_ring_free(worker->responses);
  free(worker->response);
}

LV2_Worker_Status
jalv_worker_schedule(LV2_Worker_Schedule_Handle handle,
                     uint32_t                   size,
                     const void*                data)
{
  JalvWorker* worker = (JalvWorker*)handle;

  if (!size) {
    return LV2_WORKER_ERR_UNKNOWN;
  }

  if (worker->threaded) {
    // Schedule a request to be executed by the worker thread
    zix_ring_write(worker->requests, (const char*)&size, sizeof(size));
    zix_ring_write(worker->requests, (const char*)data, size);
    zix_sem_post(&worker->sem);
  } else {
    // Execute work immediately in this thread
    zix_sem_wait(worker->lock);
    worker->iface->work(
      worker->handle, jalv_worker_respond, worker, size, data);
    zix_sem_post(worker->lock);
  }

  return LV2_WORKER_SUCCESS;
}

void
jalv_worker_emit_responses(JalvWorker* worker, LV2_Handle lv2_handle)
{
  if (worker->responses) {
    uint32_t read_space = zix_ring_read_space(worker->responses);
    while (read_space) {
      uint32_t size = 0;
      zix_ring_read(worker->responses, &size, sizeof(size));
      zix_ring_read(worker->responses, worker->response, size);

      worker->iface->work_response(lv2_handle, size, worker->response);

      read_space -= sizeof(size) + size;
    }
  }
}
