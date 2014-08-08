/*
  Copyright 2007-2013 David Robillard <http://drobilla.net>

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
	Jalv* jalv = (Jalv*)handle;
	jack_ringbuffer_write(jalv->worker.responses,
	                      (const char*)&size, sizeof(size));
	jack_ringbuffer_write(jalv->worker.responses, (const char*)data, size);
	return LV2_WORKER_SUCCESS;
}

static void*
worker_func(void* data)
{
	Jalv* jalv = (Jalv*)data;
	void* buf  = NULL;
	while (true) {
		zix_sem_wait(&jalv->worker.sem);
		if (jalv->exit) {
			break;
		}

		uint32_t size = 0;
		jack_ringbuffer_read(jalv->worker.requests, (char*)&size, sizeof(size));

		if (!(buf = realloc(buf, size))) {
			fprintf(stderr, "error: realloc() failed\n");
			free(buf);
			return NULL;
		}

		jack_ringbuffer_read(jalv->worker.requests, (char*)buf, size);

		jalv->worker.iface->work(
			jalv->instance->lv2_handle, jalv_worker_respond, jalv, size, buf);
	}

	free(buf);
	return NULL;
}

void
jalv_worker_init(Jalv*                       jalv,
                 JalvWorker*                 worker,
                 const LV2_Worker_Interface* iface)
{
	worker->iface = iface;
	zix_thread_create(&worker->thread, 4096, worker_func, jalv);
	worker->requests  = jack_ringbuffer_create(4096);
	worker->responses = jack_ringbuffer_create(4096);
	worker->response  = malloc(4096);
	jack_ringbuffer_mlock(worker->requests);
	jack_ringbuffer_mlock(worker->responses);
}

void
jalv_worker_finish(JalvWorker* worker)
{
	if (worker->requests) {
		zix_sem_post(&worker->sem);
		zix_thread_join(worker->thread, NULL);
		jack_ringbuffer_free(worker->requests);
		jack_ringbuffer_free(worker->responses);
		free(worker->response);
	}
}

LV2_Worker_Status
jalv_worker_schedule(LV2_Worker_Schedule_Handle handle,
                     uint32_t                   size,
                     const void*                data)
{
	Jalv* jalv = (Jalv*)handle;
	jack_ringbuffer_write(jalv->worker.requests,
	                      (const char*)&size, sizeof(size));
	jack_ringbuffer_write(jalv->worker.requests, (const char*)data, size);
	zix_sem_post(&jalv->worker.sem);
	return LV2_WORKER_SUCCESS;
}

void
jalv_worker_emit_responses(Jalv* jalv, JalvWorker* worker)
{
	if (worker->responses) {
		uint32_t read_space = jack_ringbuffer_read_space(worker->responses);
		while (read_space) {
			uint32_t size = 0;
			jack_ringbuffer_read(worker->responses, (char*)&size, sizeof(size));

			jack_ringbuffer_read(
				worker->responses, (char*)worker->response, size);

			worker->iface->work_response(
				jalv->instance->lv2_handle, size, worker->response);

			read_space -= sizeof(size) + size;
		}
	}
}
