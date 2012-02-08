/*
  Copyright 2008-2012 David Robillard <http://drobilla.net>

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

#include "lv2/lv2plug.in/ns/ext/atom/atom.h"
#include "lv2/lv2plug.in/ns/ext/event/event.h"

#include "lv2_evbuf.h"

struct LV2_Evbuf_Impl {
	LV2_Evbuf_Type type;
	union {
		LV2_Event_Buffer  event;
		LV2_Atom_Sequence atom;
	} buf;
};

static inline uint32_t
lv2_evbuf_pad_size(uint32_t size)
{
	return (size + 7) & (~7);
}

LV2_Evbuf*
lv2_evbuf_new(uint32_t capacity, LV2_Evbuf_Type type)
{
	LV2_Evbuf* evbuf = (LV2_Evbuf*)malloc(sizeof(LV2_Evbuf) + capacity);
	evbuf->type = type;
	switch (type) {
	case LV2_EVBUF_EVENT:
		evbuf->buf.event.data     = (uint8_t*)evbuf + sizeof(LV2_Evbuf);
		evbuf->buf.event.capacity = capacity;
		break;
	case LV2_EVBUF_ATOM:
		// FIXME: set type
		evbuf->buf.atom.capacity = capacity;
	}
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
	switch (evbuf->type) {
	case LV2_EVBUF_EVENT:
		evbuf->buf.event.header_size = sizeof(LV2_Event_Buffer);
		evbuf->buf.event.stamp_type  = LV2_EVENT_AUDIO_STAMP;
		evbuf->buf.event.event_count = 0;
		evbuf->buf.event.size        = 0;
		break;
	case LV2_EVBUF_ATOM:
		evbuf->buf.atom.atom.size = 0;
	}
}

uint32_t
lv2_evbuf_get_size(LV2_Evbuf* evbuf)
{
	switch (evbuf->type) {
	case LV2_EVBUF_EVENT:
		return evbuf->buf.event.size;
	case LV2_EVBUF_ATOM:
		return evbuf->buf.atom.atom.size;
	}
	return 0;
}

void*
lv2_evbuf_get_buffer(LV2_Evbuf* evbuf)
{
	switch (evbuf->type) {
	case LV2_EVBUF_EVENT:
		return &evbuf->buf.event;
	case LV2_EVBUF_ATOM:
		return &evbuf->buf.atom;
	}
	return NULL;
}

LV2_Evbuf_Iterator
lv2_evbuf_begin(LV2_Evbuf* evbuf)
{
	LV2_Evbuf_Iterator iter = { evbuf, 0 };
	return iter;
}

LV2_Evbuf_Iterator
lv2_evbuf_end(LV2_Evbuf* evbuf)
{
	const size_t             size = lv2_evbuf_get_size(evbuf);
	const LV2_Evbuf_Iterator iter = { evbuf, lv2_evbuf_pad_size(size) };
	return iter;
}

bool
lv2_evbuf_is_valid(LV2_Evbuf_Iterator iter)
{
	return iter.offset < lv2_evbuf_get_size(iter.evbuf);
}

LV2_Evbuf_Iterator
lv2_evbuf_next(LV2_Evbuf_Iterator iter)
{
	if (!lv2_evbuf_is_valid(iter)) {
		return iter;
	}

	LV2_Evbuf* evbuf  = iter.evbuf;
	uint32_t   offset = iter.offset;
	uint32_t   size;
	switch (evbuf->type) {
	case LV2_EVBUF_EVENT:
		size    = ((LV2_Event*)(evbuf->buf.event.data + offset))->size;
		offset += lv2_evbuf_pad_size(sizeof(LV2_Event) + size);
		break;
	case LV2_EVBUF_ATOM:
		size = ((LV2_Atom_Event*)
		        ((char*)LV2_ATOM_CONTENTS(LV2_Atom_Sequence, &evbuf->buf.atom)
		         + offset))->body.size;
		offset += lv2_evbuf_pad_size(sizeof(LV2_Atom_Event) + size);
		break;
	}

	LV2_Evbuf_Iterator next = { evbuf, offset };
	return next;
}

bool
lv2_evbuf_get(LV2_Evbuf_Iterator iter,
              uint32_t*          frames,
              uint32_t*          subframes,
              uint32_t*          type,
              uint32_t*          size,
              uint8_t**          data)
{
	*frames = *subframes = *type = *size = 0;
	*data = NULL;

	if (!lv2_evbuf_is_valid(iter)) {
		return false;
	}

	LV2_Event*      ev;
	LV2_Atom_Event* aev;
	switch (iter.evbuf->type) {
	case LV2_EVBUF_EVENT:
		ev = (LV2_Event*)(iter.evbuf->buf.event.data + iter.offset);
		*frames    = ev->frames;
		*subframes = ev->subframes;
		*type      = ev->type;
		*size      = ev->size;
		*data      = (uint8_t*)ev + sizeof(LV2_Event);
		break;
	case LV2_EVBUF_ATOM:
		aev = (LV2_Atom_Event*)(
			(char*)LV2_ATOM_CONTENTS(LV2_Atom_Sequence, &iter.evbuf->buf.atom)
			+ iter.offset);
		*frames    = aev->time.audio.frames;
		*subframes = aev->time.audio.subframes;
		*type      = aev->body.type;
		*size      = aev->body.size;
		*data      = LV2_ATOM_BODY(&aev->body);
		break;
	}

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
	LV2_Event_Buffer*  ebuf;
	LV2_Event*         ev;
	LV2_Atom_Sequence* abuf;
	LV2_Atom_Event*    aev;
	switch (iter->evbuf->type) {
	case LV2_EVBUF_EVENT:
		ebuf = &iter->evbuf->buf.event;
		if (ebuf->capacity - ebuf->size < sizeof(LV2_Event) + size) {
			return false;
		}

		ev = (LV2_Event*)(ebuf->data + iter->offset);
		ev->frames    = frames;
		ev->subframes = subframes;
		ev->type      = type;
		ev->size      = size;
		memcpy((uint8_t*)ev + sizeof(LV2_Event), data, size);

		size               = lv2_evbuf_pad_size(sizeof(LV2_Event) + size);
		ebuf->size        += size;
		ebuf->event_count += 1;
		iter->offset      += size;
		break;
	case LV2_EVBUF_ATOM:
		abuf = &iter->evbuf->buf.atom;
		if (abuf->capacity - abuf->atom.size < sizeof(LV2_Atom_Event) + size) {
			return false;
		}

		aev = (LV2_Atom_Event*)(
			(char*)LV2_ATOM_CONTENTS(LV2_Atom_Sequence, abuf)
			+ iter->offset);
		aev->time.audio.frames    = frames;
		aev->time.audio.subframes = subframes;
		aev->body.type            = type;
		aev->body.size            = size;
		memcpy(LV2_ATOM_BODY(&aev->body), data, size);

		size             = lv2_evbuf_pad_size(sizeof(LV2_Atom_Event) + size);
		abuf->atom.size += size;
		iter->offset    += size;
		break;
	}

	return true;
}
