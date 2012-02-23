/*
  Copyright 2012 David Robillard <http://drobilla.net>

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

#include <stdlib.h>
#include <string.h>

#include "lv2/lv2plug.in/ns/ext/atom/atom.h"
#include "lv2/lv2plug.in/ns/ext/atom/util.h"
#include "lv2/lv2plug.in/ns/ext/urid/urid.h"

#include "serd/serd.h"

#define NS_XSD "http://www.w3.org/2001/XMLSchema#"

#define USTR(str) ((const uint8_t*)(str))

typedef struct {
	char*  buf;
	size_t len;
} String;

static size_t
string_sink(const void* buf, size_t len, void* stream)
{
	String* str = (String*)stream;
	str->buf = realloc(str->buf, str->len + len);
	memcpy(str->buf + str->len, buf, len);
	str->len += len;
	return len;
}

void
atom_to_rdf(SerdWriter*     writer,
            LV2_URID_Unmap* unmap,
            const SerdNode* subject,
            const SerdNode* predicate,
            const LV2_Atom* atom,
            uint32_t        flags)
{
	const char* const type     = unmap->unmap(unmap->handle, atom->type);
	SerdNode          object   = SERD_NODE_NULL;
	SerdNode          datatype = SERD_NODE_NULL;
	bool              new_node = false;
	if (atom->type == 0 && atom->size == 0) {
		object = serd_node_from_string(SERD_BLANK, USTR("null"));
	} else if (!strcmp(type, LV2_ATOM__String)) {
		const uint8_t* str = USTR(LV2_ATOM_BODY(atom));
		object = serd_node_from_string(SERD_LITERAL, str);
	} else if (!strcmp(type, LV2_ATOM__URID)) {
		const uint32_t id  = *(const uint32_t*)LV2_ATOM_BODY(atom);
		const uint8_t* str = USTR(unmap->unmap(unmap->handle, id));
		object = serd_node_from_string(SERD_URI, str);
	} else if (!strcmp(type, LV2_ATOM__Path)) {
		const uint8_t* str = USTR(LV2_ATOM_BODY(atom));
		object = serd_node_from_string(SERD_LITERAL, str);
		datatype = serd_node_from_string(SERD_URI, USTR(LV2_ATOM__Path));
	} else if (!strcmp(type, LV2_ATOM__URI)) {
		const uint8_t* str = USTR(LV2_ATOM_BODY(atom));
		object = serd_node_from_string(SERD_URI, str);
	} else if (!strcmp(type, LV2_ATOM__Int32)) {
		new_node = true;
		object   = serd_node_new_integer(*(int32_t*)LV2_ATOM_BODY(atom));
		datatype = serd_node_from_string(SERD_URI, USTR(NS_XSD "integer"));
	} else if (!strcmp(type, LV2_ATOM__Float)) {
		new_node = true;
		object   = serd_node_new_decimal(*(float*)LV2_ATOM_BODY(atom), 8);
		datatype = serd_node_from_string(SERD_URI, USTR(NS_XSD "decimal"));
	} else if (!strcmp(type, LV2_ATOM__Double)) {
		new_node = true;
		object   = serd_node_new_decimal(*(float*)LV2_ATOM_BODY(atom), 16);
		datatype = serd_node_from_string(SERD_URI, USTR(NS_XSD "decimal"));
	} else if (!strcmp(type, LV2_ATOM__Bool)) {
		const int32_t val = *(const int32_t*)LV2_ATOM_BODY(atom);
		new_node = true;
		datatype = serd_node_from_string(SERD_URI, USTR(NS_XSD "boolean"));
		object   = serd_node_from_string(SERD_LITERAL,
		                                 USTR(val ? "true" : "false"));
	} else if (!strcmp(type, LV2_ATOM__Blank)) {
		const LV2_Atom_Object* obj = (const LV2_Atom_Object*)atom;
		SerdNode idnum = serd_node_new_integer(obj->body.id);
		SerdNode id    = serd_node_from_string(SERD_BLANK, idnum.buf);
		serd_writer_write_statement(
			writer, flags|SERD_ANON_O_BEGIN, NULL,
			subject, predicate, &id, NULL, NULL);
		LV2_OBJECT_FOREACH(obj, i) {
			const LV2_Atom_Property_Body* prop = lv2_object_iter_get(i);
			const char* const key = unmap->unmap(unmap->handle, prop->key);
			SerdNode pred = serd_node_from_string(SERD_URI, USTR(key));
			atom_to_rdf(writer, unmap, &id, &pred, &prop->value, SERD_ANON_CONT);
		}
		serd_writer_end_anon(writer, &id);
		serd_node_free(&idnum);
	}

	if (object.buf) {
		serd_writer_write_statement(
			writer, flags, NULL, subject, predicate, &object, &datatype, NULL);
	}

	if (new_node) {
		serd_node_free(&object);
	}
}

char*
atom_to_turtle(LV2_URID_Unmap* unmap,
               const SerdNode* subject,
               const SerdNode* predicate,
               const LV2_Atom* atom)
{
	SerdURI     base_uri = SERD_URI_NULL;
	SerdEnv*    env      = serd_env_new(NULL);
	String      str      = { NULL, 0 };
	SerdWriter* writer   = serd_writer_new(
		SERD_TURTLE,
		SERD_STYLE_ABBREVIATED|SERD_STYLE_RESOLVED|SERD_STYLE_CURIED,
		env, &base_uri, string_sink, &str);

	atom_to_rdf(writer, unmap, subject, predicate, atom, 0);
	serd_writer_finish(writer);
	string_sink("", 1, &str);

	serd_writer_free(writer);
	serd_env_free(env);
	return str.buf;
}
