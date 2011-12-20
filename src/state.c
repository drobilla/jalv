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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_LV2_STATE
#    include "lv2/lv2plug.in/ns/ext/state/state.h"
#endif

#include "jalv-config.h"
#include "jalv_internal.h"

#define NS_ATOM  (const uint8_t*)"http://lv2plug.in/ns/ext/atom#"
#define NS_JALV  (const uint8_t*)"http://drobilla.net/ns/jalv#"
#define NS_LV2   (const uint8_t*)"http://lv2plug.in/ns/lv2core#"
#define NS_PSET  (const uint8_t*)"http://lv2plug.in/ns/ext/presets#"
#define NS_STATE (const uint8_t*)"http://lv2plug.in/ns/ext/state#"
#define NS_XSD   (const uint8_t*)"http://www.w3.org/2001/XMLSchema#"
#define NS_RDFS  (const uint8_t*)"http://www.w3.org/2000/01/rdf-schema#"

#define USTR(s) ((const uint8_t*)s)

static int
property_cmp(const void* a, const void* b)
{
	const struct Property* pa = (const struct Property*)a;
	const struct Property* pb = (const struct Property*)b;
	return pa->key - pb->key;
}

#ifdef HAVE_LV2_STATE
typedef struct {
	LV2_URID_Unmap* unmap;
	const SerdNode* subject;
	SerdWriter*     writer;
} StoreData;

static int
store_callback(void*       handle,
               uint32_t    key,
               const void* value,
               size_t      size,
               uint32_t    type,
               uint32_t    flags)
{
	StoreData*  data     = (StoreData*)handle;
	const char* key_uri  = data->unmap->unmap(data->unmap->handle, key);
	const char* type_uri = data->unmap->unmap(data->unmap->handle, type);
	if (strcmp(type_uri, (const char*)(NS_ATOM "String"))) {
		fprintf(stderr, "error: Unsupported (not atom:String) value stored\n");
		return 1;
	}

	if (key_uri && type_uri && value) {
		const SerdNode p = serd_node_from_string(SERD_URI, USTR(key_uri));
		const SerdNode o = serd_node_from_string(SERD_LITERAL, USTR(value));
		const SerdNode t = serd_node_from_string(SERD_URI, USTR(type_uri));

		serd_writer_write_statement(data->writer, SERD_ANON_CONT, NULL,
		                            data->subject, &p, &o, &t, NULL);

		return 0;
	}
	
	fprintf(stderr, "error: Failed to store property (key %d)\n", key);
	return 1;
}

typedef struct {
	LV2_URID_Map*          map;
	const struct Property* props;
	const uint32_t         num_props;
} RetrieveData;

static const void*
retrieve_callback(void*     handle,
                  uint32_t  key,
                  size_t*   size,
                  uint32_t* type,
                  uint32_t* flags)
{
	RetrieveData*    data       = (RetrieveData*)handle;
	struct Property  search_key = { key, SERD_NODE_NULL, SERD_NODE_NULL };
	struct Property* prop       = (struct Property*)bsearch(
		&search_key, data->props, data->num_props,
		sizeof(struct Property), property_cmp);

	if (prop) {
		*size  = prop->value.n_bytes;
		*type  = data->map->map(data->map->handle,
		                        (const char*)(NS_ATOM "String"));
		*flags = 0;
		return prop->value.buf;
	}
		
	return NULL;
	
}
#endif  // HAVE_LV2_STATE

static size_t
file_sink(const void* buf, size_t len, void* stream)
{
	FILE* file = (FILE*)stream;
	return fwrite(buf, 1, len, file);
}

void
jalv_save(Jalv* jalv, const char* dir)
{
	const size_t      dir_len  = strlen(dir);
	const char* const filename = "state.ttl";
	const size_t      path_len = dir_len + strlen(filename);
	char* const       path     = (char*)malloc(path_len + 1);

	snprintf(path, path_len + 1, "%s%s", dir, filename);
	FILE* out_fd = fopen(path, "w");

	SerdEnv* env = serd_env_new(NULL);
	serd_env_set_prefix_from_strings(env, USTR("atom"),  USTR(NS_ATOM));
	serd_env_set_prefix_from_strings(env, USTR("jalv"),  USTR(NS_JALV));
	serd_env_set_prefix_from_strings(env, USTR("lv2"),   USTR(NS_LV2));
	serd_env_set_prefix_from_strings(env, USTR("pset"),  USTR(NS_PSET));
	serd_env_set_prefix_from_strings(env, USTR("rdfs"),  USTR(NS_RDFS));
	serd_env_set_prefix_from_strings(env, USTR("state"), USTR(NS_STATE));

	SerdNode lv2_appliesTo = serd_node_from_string(SERD_URI, NS_LV2 "appliesTo");

	SerdNode plugin_uri = serd_node_from_string(SERD_URI, USTR(lilv_node_as_uri(
			               lilv_plugin_get_uri(jalv->plugin))));

	SerdNode subject = serd_node_from_string(SERD_URI, USTR(""));

	SerdWriter* writer = serd_writer_new(
		SERD_TURTLE,
		SERD_STYLE_ABBREVIATED|SERD_STYLE_CURIED,
		env,
		&SERD_URI_NULL,
		file_sink,
		out_fd);

	serd_env_foreach(env, (SerdPrefixSink)serd_writer_set_prefix, writer);

	// <> lv2:appliesTo <http://example.org/plugin>
	serd_writer_write_statement(writer, 0, NULL,
	                            &subject,
	                            &lv2_appliesTo,
	                            &plugin_uri, NULL, NULL);

	jalv_save_port_values(jalv, writer, &subject);

#ifdef HAVE_LV2_STATE
	assert(jalv->symap);
	const LV2_State_Interface* state = (const LV2_State_Interface*)
		lilv_instance_get_extension_data(jalv->instance, LV2_STATE_INTERFACE_URI);

	if (state) {
		SerdNode state_instanceState = serd_node_from_string(
			SERD_URI, (NS_STATE "instanceState"));

		// [] state:instanceState [
		SerdNode state_node = serd_node_from_string(SERD_BLANK, USTR("state"));
		serd_writer_write_statement(writer, SERD_ANON_O_BEGIN, NULL,
		                            &subject,
		                            &state_instanceState,
		                            &state_node, NULL, NULL);

		StoreData data = { &jalv->unmap, &state_node, writer };

		// Write properties to state blank node
		state->save(lilv_instance_get_handle(jalv->instance),
		            store_callback,
		            &data,
		            LV2_STATE_IS_POD|LV2_STATE_IS_PORTABLE,
		            NULL);

		// ]
		serd_writer_end_anon(writer, &state_node);
	}
#endif  // HAVE_LV2_STATE

	// Close state file and clean up Serd
	serd_writer_free(writer);
	fclose(out_fd);
	serd_env_free(env);

	free(path);
}

typedef struct {
	SerdNode symbol;
	SerdNode value;
	SerdNode datatype;
} PortValue;

typedef struct {
	LilvWorld*       world;
	LV2_URID_Map*    map;
	LilvNode*        plugin_uri;
	struct Property* props;
	PortValue*       ports;
	uint32_t         num_props;
	uint32_t         num_ports;
	bool             in_state;
} RestoreData;

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
	RestoreData* data = (RestoreData*)handle;
	if (data->in_state) {
		data->props = (struct Property*)realloc(
			data->props, sizeof(struct Property) * (++data->num_props));
		struct Property* prop = &data->props[data->num_props - 1];
		prop->key      = data->map->map(data->map->handle,
		                                (const char*)predicate->buf);
		prop->value    = serd_node_copy(object);
		prop->datatype = serd_node_copy(object_datatype);
	} else if (!strcmp((const char*)predicate->buf, "lv2:appliesTo")) {
		data->plugin_uri = lilv_new_uri(data->world, (const char*)object->buf);
	} else if (!strcmp((const char*)predicate->buf, "lv2:port")) {
		data->ports = (PortValue*)realloc(
			data->ports, sizeof(PortValue) * (++data->num_ports));
		data->ports[data->num_ports - 1].symbol   = SERD_NODE_NULL;
		data->ports[data->num_ports - 1].value    = SERD_NODE_NULL;
		data->ports[data->num_ports - 1].datatype = SERD_NODE_NULL;
	} else if (!strcmp((const char*)predicate->buf, "lv2:symbol")) {
		data->ports[data->num_ports - 1].symbol = serd_node_copy(object);
	} else if (!strcmp((const char*)predicate->buf, "pset:value")) {
		data->ports[data->num_ports - 1].value    = serd_node_copy(object);
		data->ports[data->num_ports - 1].datatype = serd_node_copy(object_datatype);
	} else if (!strcmp((const char*)predicate->buf, "state:instanceState")) {
		data->in_state = true;
	}
	
	return SERD_SUCCESS;
}

void
jalv_restore(Jalv* jalv, const char* dir)
{
	RestoreData* data = (RestoreData*)malloc(sizeof(RestoreData));
	memset(data, '\0', sizeof(RestoreData));
	data->world = jalv->world;
	data->map   = &jalv->map;

	SerdReader* reader = serd_reader_new(
		SERD_TURTLE,
		data, NULL,
		NULL,
		NULL,
		on_statement,
		NULL);

	const size_t dir_len       = strlen(dir);
	const size_t state_uri_len = strlen("file:///state.ttl") + dir_len + 1;
	char*        state_uri     = (char*)malloc(state_uri_len);
	snprintf(state_uri, state_uri_len, "file://%s/state.ttl", dir);

	SerdStatus st = serd_reader_read_file(reader, USTR(state_uri));
	if (st) {
		fprintf(stderr, "Error reading state from %s (%s)\n",
		        state_uri, serd_strerror(st));
		return;
	}

	serd_reader_free(reader);

	const LilvPlugins* plugins = lilv_world_get_all_plugins(data->world);
	jalv->plugin = lilv_plugins_get_by_uri(plugins, data->plugin_uri);
	if (!jalv->plugin) {
		fprintf(stderr, "Failed to find plugin <%s> to restore\n",
		        lilv_node_as_string(data->plugin_uri));
		return;
	}
	
	jalv->num_ports = lilv_plugin_get_num_ports(jalv->plugin);
	jalv->ports     = calloc(data->num_ports, sizeof(struct Port));
	
	jalv_create_ports(jalv);

	for (uint32_t i = 0; i < data->num_ports; ++i) {
		PortValue*         dport = &data->ports[i];
		const char*        sym   = (const char*)dport->symbol.buf;
		struct Port* const jport = jalv_port_by_symbol(jalv, sym);
		if (jport) {
			jport->control = atof((const char*)dport->value.buf);  // FIXME: Check type
		} else {
			fprintf(stderr, "error: Failed to find port `%s'\n", sym);
		}
	}

	if (jalv->props) {
		qsort(jalv->props, jalv->num_props, sizeof(struct Property), property_cmp);
	}

	jalv->props     = data->props;
	jalv->num_props = data->num_props;
	free(data->ports);
	free(data);
}

void
jalv_restore_instance(Jalv* jalv, const char* dir)
{
#ifdef HAVE_LV2_STATE
	const LV2_State_Interface* state_iface = (const LV2_State_Interface*)
		lilv_instance_get_extension_data(jalv->instance, LV2_STATE_INTERFACE_URI);

	if (state_iface) {
		RetrieveData data = { &jalv->map, jalv->props, jalv->num_props };
		state_iface->restore(lilv_instance_get_handle(jalv->instance),
		                     retrieve_callback,
		                     &data, 0, NULL);
	}
#endif  // HAVE_LV2_STATE
}
