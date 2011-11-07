/*
  Copyright 2007-2011 David Robillard <http://drobilla.net>

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

#include <assert.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_LV2_PERSIST
#    include "lv2/lv2plug.in/ns/ext/persist/persist.h"
#endif

#include "jalv-config.h"
#include "jalv_internal.h"

#define NS_JALV    (const uint8_t*)"http://drobilla.net/ns/jalv#"
#define NS_LV2     (const uint8_t*)"http://lv2plug.in/ns/lv2core#"
#define NS_XSD     (const uint8_t*)"http://www.w3.org/2001/XMLSchema#"
#define NS_ATOM    (const uint8_t*)"http://lv2plug.in/ns/ext/atom#"
#define NS_PERSIST (const uint8_t*)"http://lv2plug.in/ns/ext/persist#"

#define USTR(s) ((const uint8_t*)s)

static int
property_cmp(const void* a, const void* b)
{
	const struct Property* pa = (const struct Property*)a;
	const struct Property* pb = (const struct Property*)b;
	return pa->key - pb->key;
}

#ifdef HAVE_LV2_PERSIST
static int
store_callback(void*       host_data,
               uint32_t    key,
               const void* value,
               size_t      size,
               uint32_t    type,
               uint32_t    flags)
{
	Jalv*       jalv     = (Jalv*)host_data;
	const char* key_uri  = symap_unmap(jalv->symap, key);
	const char* type_uri = symap_unmap(jalv->symap, type);
	if (strcmp(type_uri, (const char*)(NS_ATOM "String"))) {
		fprintf(stderr, "error: Unsupported (not atom:String) value stored\n");
		return 1;
	}

	if (key_uri && type_uri && value) {
		const SerdNode p = serd_node_from_string(SERD_URI, USTR(key_uri));
		const SerdNode o = serd_node_from_string(SERD_LITERAL, USTR(value));
		const SerdNode t = serd_node_from_string(SERD_URI, USTR(type_uri));

		serd_writer_write_statement(jalv->writer, SERD_ANON_CONT, NULL,
		                            &jalv->state_node, &p, &o, &t, NULL);

		return 0;
	}
	
	fprintf(stderr, "error: Failed to store property (key %d)\n", key);
	return 1;
}

static const void*
retrieve_callback(void*     host_data,
                  uint32_t  key,
                  size_t*   size,
                  uint32_t* type,
                  uint32_t* flags)
{
	Jalv* jalv = (Jalv*)host_data;
	struct Property  search_key = { key, SERD_NODE_NULL, SERD_NODE_NULL };
	struct Property* prop       = (struct Property*)bsearch(
		&search_key, jalv->props, jalv->num_props,
		sizeof(struct Property), property_cmp);

	if (prop) {
		*size  = prop->value.n_bytes;
		*type  = symap_map(jalv->symap, (const char*)(NS_ATOM "String"));
		*flags = 0;
		return prop->value.buf;
	}
		
	return NULL;
	
}
#endif  // HAVE_LV2_PERSIST

static size_t
file_sink(const void* buf, size_t len, void* stream)
{
	FILE* file = (FILE*)stream;
	return fwrite(buf, 1, len, file);
}

void
jalv_save(Jalv* jalv, const char* dir)
{
	assert(!jalv->writer);

	// Set numeric locale to C so snprintf %f is Turtle compatible
	char* locale = jalv_strdup(setlocale(LC_NUMERIC, NULL));
	setlocale(LC_NUMERIC, "C");

	const size_t      dir_len  = strlen(dir);
	const char* const filename = "state.ttl";
	const size_t      path_len = dir_len + strlen(filename);
	char* const       path     = (char*)malloc(path_len + 1);

	snprintf(path, path_len + 1, "%s%s", dir, filename);
	FILE* out_fd = fopen(path, "w");

	SerdNode jalv_name      = serd_node_from_string(SERD_LITERAL, USTR("jalv"));
	SerdNode jalv_prefix    = serd_node_from_string(SERD_URI, NS_JALV);
	SerdNode lv2_name       = serd_node_from_string(SERD_LITERAL, USTR("lv2"));
	SerdNode lv2_prefix     = serd_node_from_string(SERD_URI, NS_LV2);
	SerdNode persist_name   = serd_node_from_string(SERD_LITERAL, USTR("persist"));
	SerdNode persist_prefix = serd_node_from_string(SERD_URI, NS_PERSIST);
	SerdNode atom_name      = serd_node_from_string(SERD_LITERAL, USTR("atom"));
	SerdNode atom_prefix    = serd_node_from_string(SERD_URI, NS_ATOM);
	SerdNode jalv_plugin    = serd_node_from_string(SERD_URI, NS_JALV "plugin");
	SerdNode jalv_value     = serd_node_from_string(SERD_URI, (NS_JALV "value"));
	SerdNode lv2_symbol     = serd_node_from_string(SERD_URI, (NS_LV2 "symbol"));
	SerdNode xsd_decimal    = serd_node_from_string(SERD_URI, (NS_XSD "decimal"));
	SerdNode jalv_port      = serd_node_from_string(SERD_URI, (NS_JALV "port"));

	SerdNode plugin_uri = serd_node_from_string(SERD_URI, USTR(lilv_node_as_uri(
			               lilv_plugin_get_uri(jalv->plugin))));

	SerdEnv* env = serd_env_new(NULL);

	SerdNode subject = serd_node_from_string(SERD_URI, USTR(""));

	jalv->writer = serd_writer_new(
		SERD_TURTLE,
		SERD_STYLE_ABBREVIATED|SERD_STYLE_CURIED,
		env,
		&SERD_URI_NULL,
		file_sink,
		out_fd);

	serd_writer_set_prefix(jalv->writer, &atom_name, &atom_prefix);
	serd_writer_set_prefix(jalv->writer, &jalv_name, &jalv_prefix);
	serd_writer_set_prefix(jalv->writer, &lv2_name, &lv2_prefix);
	serd_writer_set_prefix(jalv->writer, &persist_name, &persist_prefix);

	// <> jalv:plugin <http://example.org/plugin>
	serd_writer_write_statement(jalv->writer, 0, NULL,
	                            &subject,
	                            &jalv_plugin,
	                            &plugin_uri, NULL, NULL);

	for (uint32_t i = 0; i < jalv->num_ports; ++i) {
		struct Port* port = &jalv->ports[i];
		if (port->flow != FLOW_INPUT || port->type != TYPE_CONTROL) {
			continue;
		}

		const uint8_t* sym = (const uint8_t*)lilv_node_as_string(
			lilv_port_get_symbol(jalv->plugin, port->lilv_port));

		const SerdNode sym_node = serd_node_from_string(SERD_LITERAL, sym);
		const SerdNode blank    = serd_node_from_string(SERD_BLANK, sym);

		// <> jalv:port []
		serd_writer_write_statement(jalv->writer, SERD_ANON_O_BEGIN, NULL,
		                            &subject,
		                            &jalv_port,
		                            &blank, NULL, NULL);

		char value_str[128];
		snprintf(value_str, sizeof(value_str), "%f", port->control);
			
		SerdNode value = serd_node_from_string(SERD_LITERAL, USTR(value_str));

		// [] lv2:symbol "example"
		serd_writer_write_statement(jalv->writer, SERD_ANON_CONT, NULL,
		                            &blank, &lv2_symbol, &sym_node,
		                            NULL, NULL);

		// [] jalv:value 1.0
		serd_writer_write_statement(jalv->writer, SERD_ANON_CONT, NULL,
		                            &blank, &jalv_value, &value,
		                            &xsd_decimal, NULL);

		serd_writer_end_anon(jalv->writer, &blank);
	}

#ifdef HAVE_LV2_PERSIST
	assert(jalv->symap);
	const LV2_Persist* persist = (const LV2_Persist*)
		lilv_instance_get_extension_data(jalv->instance,
		                                 "http://lv2plug.in/ns/ext/persist");

	if (persist) {
		SerdNode persist_instanceState = serd_node_from_string(
			SERD_URI, (NS_PERSIST "instanceState"));

		// [] persist:instanceState [
		jalv->state_node = serd_node_from_string(SERD_BLANK, USTR("state"));
		serd_writer_write_statement(jalv->writer, SERD_ANON_O_BEGIN, NULL,
		                            &subject,
		                            &persist_instanceState,
		                            &jalv->state_node, NULL, NULL);

		// Write properties to state blank node
		persist->save(lilv_instance_get_handle(jalv->instance),
		              store_callback,
		              jalv);

		// ]
		serd_writer_end_anon(jalv->writer, &jalv->state_node);
		jalv->state_node = SERD_NODE_NULL;
	}
#endif  // HAVE_LV2_PERSIST

	// Close state file and clean up Serd
	serd_writer_free(jalv->writer);
	jalv->writer = NULL;
	fclose(out_fd);
	serd_env_free(env);

	// Reset numeric locale to original value
	setlocale(LC_NUMERIC, locale);
	free(locale);

	free(path);
}

static SerdStatus
on_statement(void*              handle,
             SerdStatementFlags flags,
             const SerdNode*    graph,
             const SerdNode*    subject,
             const SerdNode*    predicate,
             const SerdNode*    object,
             const SerdNode*    object_datatype,
             const SerdNode*    object_lang)
{
	Jalv* jalv = (Jalv*)handle;
	if (jalv->in_state) {
		jalv->props = (struct Property*)realloc(
			jalv->props,
			sizeof(struct Property) * (++jalv->num_props));
		struct Property* prop = &jalv->props[jalv->num_props - 1];
		prop->key      = symap_map(jalv->symap, (const char*)predicate->buf);
		prop->value    = serd_node_copy(object);
		prop->datatype = serd_node_copy(object_datatype);
	} else if (!strcmp((const char*)predicate->buf, "jalv:plugin")) {
		const LilvPlugins* plugins    = lilv_world_get_all_plugins(jalv->world);
		LilvNode*          plugin_uri = lilv_new_uri(jalv->world,
		                                             (const char*)object->buf);
		jalv->plugin = lilv_plugins_get_by_uri(plugins, plugin_uri);
		lilv_node_free(plugin_uri);

		jalv->num_ports = lilv_plugin_get_num_ports(jalv->plugin);
		jalv->ports     = calloc((size_t)jalv->num_ports, sizeof(struct Port));

		jalv_create_ports(jalv);
	} else if (!strcmp((const char*)predicate->buf, "lv2:symbol")) {
		serd_node_free(&jalv->last_sym);
		jalv->last_sym = serd_node_copy(object);
	} else if (!strcmp((const char*)predicate->buf, "jalv:value")) {
		const char*  sym  = (const char*)jalv->last_sym.buf;
		struct Port* port = jalv_port_by_symbol(jalv, sym);
		if (port) {
			port->control = atof((const char*)object->buf);  // FIXME: Check type
		} else {
			fprintf(stderr, "error: Failed to find port `%s'\n", sym);
		}
	} else if (!strcmp((const char*)predicate->buf, "persist:instanceState")) {
		jalv->in_state = true;
	}
	
	return SERD_SUCCESS;
}

void
jalv_restore(Jalv* jalv, const char* dir)
{
	jalv->reader = serd_reader_new(
		SERD_TURTLE,
		jalv, NULL,
		NULL,
		NULL,
		on_statement,
		NULL);

	const size_t dir_len       = strlen(dir);
	const size_t state_uri_len = strlen("file:///state.ttl") + dir_len + 1;
	char*        state_uri     = (char*)malloc(state_uri_len);
	snprintf(state_uri, state_uri_len, "file://%s/state.ttl", dir);

	SerdStatus st = serd_reader_read_file(jalv->reader, USTR(state_uri));
	serd_node_free(&jalv->last_sym);
	if (st) {
		fprintf(stderr, "Error reading state from %s (%s)\n",
		        state_uri, serd_strerror(st));
		return;
	}

	serd_reader_free(jalv->reader);
	jalv->reader   = NULL;
	jalv->in_state = false;

	if (jalv->props) {
		qsort(jalv->props, jalv->num_props, sizeof(struct Property), property_cmp);
	}
}

void
jalv_restore_instance(Jalv* jalv, const char* dir)
{
#ifdef HAVE_LV2_PERSIST
	const LV2_Persist* persist = (const LV2_Persist*)
		lilv_instance_get_extension_data(jalv->instance,
		                                 "http://lv2plug.in/ns/ext/persist");
	if (persist) {
		persist->restore(lilv_instance_get_handle(jalv->instance),
		                 retrieve_callback,
		                 jalv);
	}
#endif  // HAVE_LV2_PERSIST
}
