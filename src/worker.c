/*
  Copyright 2007-2016 David Robillard <http://drobilla.net>

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

#include "worker.h"

static LV2_Worker_Status
jalv_worker_respond(LV2_Worker_Respond_Handle handle,
                    uint32_t                  size,
                    const void*               data)
{
	JalvWorker* worker = (JalvWorker*)handle;
	if (zix_ring_write_space(worker->responses) < (sizeof(size) + size)) {
		return LV2_WORKER_ERR_NO_SPACE;
	}

	zix_ring_write(worker->responses, (const char*)&size, sizeof(size));
	zix_ring_write(worker->responses, (const char*)data, size);
	return LV2_WORKER_SUCCESS;
}

static void*
worker_func(void* data)
{
	JalvWorker* worker = (JalvWorker*)data;
	Jalv*       jalv   = worker->jalv;
	void*       buf    = NULL;
	while (true) {
		zix_sem_wait(&worker->sem);
		if (jalv->exit) {
			break;
		}

		uint32_t size = 0;
		zix_ring_read(worker->requests, (char*)&size, sizeof(size));

		if (!(buf = realloc(buf, size))) {
			fprintf(stderr, "error: realloc() failed\n");
			free(buf);
			return NULL;
		}

		zix_ring_read(worker->requests, (char*)buf, size);

		zix_sem_wait(&jalv->work_lock);
		worker->iface->work(
			jalv->instance->lv2_handle, jalv_worker_respond, worker, size, buf);
		zix_sem_post(&jalv->work_lock);
	}

	free(buf);
	return NULL;
}

void
jalv_worker_init(ZIX_UNUSED Jalv*            jalv,
                 JalvWorker*                 worker,
                 const LV2_Worker_Interface* iface,
                 bool                        threaded)
{
	worker->iface = iface;
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
	if (worker->requests) {
		if (worker->threaded) {
			zix_ring_free(worker->requests);
		}
		zix_ring_free(worker->responses);
		free(worker->response);
	}
}

LV2_Worker_Status
jalv_worker_schedule(LV2_Worker_Schedule_Handle handle,
                     uint32_t                   size,
                     const void*                data)
{
	JalvWorker* worker = (JalvWorker*)handle;
	Jalv*       jalv   = worker->jalv;
	if (worker->threaded) {
		// Schedule a request to be executed by the worker thread
		zix_ring_write(worker->requests, (const char*)&size, sizeof(size));
		zix_ring_write(worker->requests, (const char*)data, size);
		zix_sem_post(&worker->sem);
	} else {
		// Execute work immediately in this thread
		zix_sem_wait(&jalv->work_lock);
		worker->iface->work(
			jalv->instance->lv2_handle, jalv_worker_respond, worker, size, data);
		zix_sem_post(&jalv->work_lock);
	}
	return LV2_WORKER_SUCCESS;
}

void
jalv_worker_emit_responses(JalvWorker* worker, LilvInstance* instance)
{
	if (worker->responses) {
		uint32_t read_space = 0;
		while (read_space = zix_ring_read_space(worker->responses)) {
			uint32_t size = 0;
			if (zix_ring_peek(worker->responses, (char*)&size, sizeof(size)) <= 0) {
				fprintf(stderr, "error: Response buffer corrupted (req %lu avail %u)\n",
					sizeof(size), read_space);
				break;
			}

			const uint32_t packet_size = sizeof(size) + size;
			if (read_space < packet_size) {
				fprintf(stderr, "warning: Try to read bigger response (%u) than data available (%u). Retry later.\n",
					packet_size, read_space);
				break;
			}

			if (zix_ring_skip(worker->responses, sizeof(size)) <= 0) {
				fprintf(stderr, "error: Response buffer corrupted on skip (req %lu avail %u)\n",
					sizeof(size), read_space);
				break;
			}

			if (zix_ring_read(worker->responses, (char*)worker->response, size) <= 0) {
				fprintf(stderr, "error: Response buffer corrupted on read response (req %u avail %u)\n",
					size, zix_ring_read_space(worker->responses));
				break;
			}

			worker->iface->work_response(
				instance->lv2_handle, size, worker->response);
		}
	}
}
