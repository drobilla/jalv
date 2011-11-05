/*
  Copyright 2008-2011 David Robillard <http://drobilla.net>

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

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include "lv2/lv2plug.in/ns/ext/atom/atom.h"
#include "lv2/lv2plug.in/ns/ext/event/event.h"

#include "lv2_evbuf.h"

struct LV2_Evbuf_Impl {
	union {
		LV2_Event_Buffer      event;
		LV2_Atom_Event_Buffer atom_event;
	} buf;
	LV2_Evbuf_Type type;
};

static inline uint32_t
lv2_evbuf_pad_size(uint32_t size)
{
	return (size + 7) & (~7);
}

LV2_Evbuf*
lv2_evbuf_new(uint32_t capacity)
{
	LV2_Evbuf* evbuf = (LV2_Evbuf*)malloc(sizeof(LV2_Evbuf));
	evbuf->type = LV2_EVBUF_EVENT_BUFFER;
	evbuf->buf.event.capacity = capacity;
	lv2_evbuf_reset(evbuf);
	return evbuf;
}

void
lv2_evbuf_free(LV2_Evbuf* evbuf)
{
	free(evbuf);
}

void
lv2_evbuf_reset(LV2_Evbuf* evbuf)
{
	LV2_Event_Buffer* buf = &evbuf->buf.event;
	buf->header_size = sizeof(LV2_Event_Buffer);
	buf->stamp_type  = LV2_EVENT_AUDIO_STAMP;
	buf->event_count = 0;
	buf->size        = 0;
}

uint32_t
lv2_evbuf_get_event_count(LV2_Evbuf* evbuf)
{
	return evbuf->buf.event.event_count;
}

void*
lv2_evbuf_get_buffer(LV2_Evbuf* evbuf)
{
	return &evbuf->buf.event;
}

bool
lv2_evbuf_begin(LV2_Evbuf_Iterator* iter,
                LV2_Evbuf*          evbuf)
{
	LV2_Event_Buffer* buf = &evbuf->buf.event;
	iter->offset = 0;
	return buf->size > 0;
}

bool
lv2_evbuf_is_valid(LV2_Evbuf_Iterator* iter)
{
	return iter->offset < iter->evbuf->buf.event.size;
}

bool
lv2_evbuf_increment(LV2_Evbuf_Iterator* iter)
{
	assert(lv2_evbuf_is_valid(iter));

	LV2_Event* const ev = (LV2_Event*)(
		(uint8_t*)iter->evbuf->buf.event.data + iter->offset);

	iter->offset += lv2_evbuf_pad_size(sizeof(LV2_Event) + ev->size);

	return true;
}

bool
lv2_evbuf_get(LV2_Evbuf_Iterator* iter,
              uint32_t*           frames,
              uint32_t*           subframes,
              uint32_t*           type,
              uint32_t*           size,
              uint8_t**           data)
{
	*frames = *subframes = *type = *size = 0;
	*data = NULL;
	assert(lv2_evbuf_is_valid(iter));

	LV2_Event* const ev = (LV2_Event*)(
		(uint8_t*)iter->evbuf->buf.event.data + iter->offset);

	*frames    = ev->frames;
	*subframes = ev->subframes;
	*type      = ev->type;
	*size      = ev->size;
	*data      = (uint8_t*)ev + sizeof(LV2_Event);

	return true;
}

bool
lv2_evbuf_write(LV2_Evbuf_Iterator* iter,
                uint32_t            frames,
                uint32_t            subframes,
                uint32_t            type,
                uint32_t            size,
                const uint8_t*      data)
{
	LV2_Event_Buffer* buf = &iter->evbuf->buf.event;
	if (buf->capacity - buf->size < sizeof(LV2_Event) + size) {
		return false;
	}

	LV2_Event* const ev = (LV2_Event*)((uint8_t*)buf->data + iter->offset);

	ev->frames    = frames;
	ev->subframes = subframes;
	ev->type      = type;
	ev->size      = size;
	memcpy((uint8_t*)ev + sizeof(LV2_Event), data, size);
	++buf->event_count;

	size             = lv2_evbuf_pad_size(sizeof(LV2_Event) + size);
	buf->size += size;
	iter->offset    += size;

	return true;
}
