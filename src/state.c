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

#define _POSIX_C_SOURCE 200112L /* for fileno */
#define _BSD_SOURCE 1 /* for lockf */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef HAVE_LV2_STATE
#    include "lv2/lv2plug.in/ns/ext/state/state.h"
#endif

#include "lilv/lilv.h"

#include "jalv-config.h"
#include "jalv_internal.h"

#ifdef HAVE_LOCKF
#include <unistd.h>
#endif

#define NS_ATOM  "http://lv2plug.in/ns/ext/atom#"
#define NS_JALV  "http://drobilla.net/ns/jalv#"
#define NS_LV2   "http://lv2plug.in/ns/lv2core#"
#define NS_PSET  "http://lv2plug.in/ns/ext/presets#"
#define NS_RDF   "http://www.w3.org/1999/02/22-rdf-syntax-ns#"
#define NS_RDFS  "http://www.w3.org/2000/01/rdf-schema#"
#define NS_STATE "http://lv2plug.in/ns/ext/state#"
#define NS_XSD   "http://www.w3.org/2001/XMLSchema#"

#define USTR(s) ((const uint8_t*)s)

typedef struct {
	uint32_t key;
	SerdNode value;
	SerdNode datatype;
} Property;

typedef struct {
	SerdNode symbol;
	SerdNode value;
	SerdNode datatype;
} PortValue;

struct PluginStateImpl {
	LilvNode*  plugin_uri;
	Property*  props;
	PortValue* values;
	uint32_t   num_props;
	uint32_t   num_values;
};

const LilvNode*
plugin_state_get_plugin_uri(const PluginState* state)
{
	return state->plugin_uri;
}

static int
property_cmp(const void* a, const void* b)
{
	const Property* pa = (const Property*)a;
	const Property* pb = (const Property*)b;
	return pa->key - pb->key;
}

static int
value_cmp(const void* a, const void* b)
{
	const PortValue* pa = (const PortValue*)a;
	const PortValue* pb = (const PortValue*)b;
	return strcmp((const char*)pa->symbol.buf, (const char*)pb->symbol.buf);
}

static char*
strjoin(const char* a, const char* b)
{
	const size_t a_len = strlen(a);
	const size_t b_len = strlen(b);
	char* const  out   = malloc(a_len + b_len + 1);

	memcpy(out,         a, a_len);
	memcpy(out + a_len, b, b_len);
	out[a_len + b_len] = '\0';

	return out;
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
	LV2_URID_Map*   map;
	const Property* props;
	const uint32_t  num_props;
} RetrieveData;

static const void*
retrieve_callback(void*     handle,
                  uint32_t  key,
                  size_t*   size,
                  uint32_t* type,
                  uint32_t* flags)
{
	RetrieveData* data       = (RetrieveData*)handle;
	Property      search_key = { key, SERD_NODE_NULL, SERD_NODE_NULL };
	Property*     prop       = (Property*)bsearch(
		&search_key, data->props, data->num_props,
		sizeof(Property), property_cmp);

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

int
write_preset(Jalv* jalv, const char* path, const char* label)
{
	FILE* fd = fopen(path, "w");
	if (!fd) {
		fprintf(stderr, "error: Failed to open %s (%s)\n",
		        path, strerror(errno));
		return 1;
	}

	SerdEnv* env = serd_env_new(NULL);
	serd_env_set_prefix_from_strings(env, USTR("atom"),  USTR(NS_ATOM));
	serd_env_set_prefix_from_strings(env, USTR("jalv"),  USTR(NS_JALV));
	serd_env_set_prefix_from_strings(env, USTR("lv2"),   USTR(NS_LV2));
	serd_env_set_prefix_from_strings(env, USTR("pset"),  USTR(NS_PSET));
	serd_env_set_prefix_from_strings(env, USTR("rdfs"),  USTR(NS_RDFS));
	serd_env_set_prefix_from_strings(env, USTR("state"), USTR(NS_STATE));

	SerdNode lv2_appliesTo = serd_node_from_string(
		SERD_CURIE, USTR("lv2:appliesTo"));

	SerdNode plugin_uri = serd_node_from_string(SERD_URI, USTR(lilv_node_as_uri(
			               lilv_plugin_get_uri(jalv->plugin))));

	SerdNode subject = serd_node_from_string(SERD_URI, USTR(""));

	SerdWriter* writer = serd_writer_new(
		SERD_TURTLE,
		SERD_STYLE_ABBREVIATED|SERD_STYLE_CURIED,
		env,
		&SERD_URI_NULL,
		file_sink,
		fd);

	serd_env_foreach(env, (SerdPrefixSink)serd_writer_set_prefix, writer);

	// subject a pset:Preset
	SerdNode p = serd_node_from_string(SERD_URI, USTR(NS_RDF "type"));
	SerdNode o = serd_node_from_string(SERD_CURIE, USTR("pset:Preset"));
	serd_writer_write_statement(writer, 0, NULL,
	                            &subject, &p, &o, NULL, NULL);

	// subject lv2:appliesTo <http://example.org/plugin>
	serd_writer_write_statement(writer, 0, NULL,
	                            &subject,
	                            &lv2_appliesTo,
	                            &plugin_uri, NULL, NULL);

	// subject rdfs:label label
	if (label) {
		p = serd_node_from_string(SERD_URI, USTR(NS_RDFS "label"));
		o = serd_node_from_string(SERD_LITERAL, USTR(label));
		serd_writer_write_statement(writer, 0,
		                            NULL, &subject, &p, &o, NULL, NULL);
	}

	jalv_save_port_values(jalv, writer, &subject);

#ifdef HAVE_LV2_STATE
	assert(jalv->symap);
	const LV2_State_Interface* state = (const LV2_State_Interface*)
		lilv_instance_get_extension_data(jalv->instance, LV2_STATE_INTERFACE_URI);

	if (state) {
		SerdNode state_instanceState = serd_node_from_string(
			SERD_URI, USTR(NS_STATE "instanceState"));

		// subject state:instanceState [
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
	fclose(fd);
	serd_env_free(env);

	return 0;
}

void
jalv_save(Jalv* jalv, const char* dir)
{
	char* const path = strjoin(dir, "/state.ttl");
	write_preset(jalv, path, NULL);
	free(path);
}

typedef struct {
	LilvWorld*    world;
	LV2_URID_Map* map;
	LilvNode*     plugin_uri;
	Property*     props;
	PortValue*    ports;
	uint32_t      num_props;
	uint32_t      num_ports;
	bool          in_state;
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
		data->props = (Property*)realloc(
			data->props, sizeof(Property) * (++data->num_props));
		Property* prop = &data->props[data->num_props - 1];
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

static PluginState*
load_state_from_file(LilvWorld*    world,
                     LV2_URID_Map* map,
                     const char*   state_uri);

PluginState*
jalv_load_state(Jalv* jalv, const char* dir)
{
	char* base_uri  = strjoin("file://", dir);
	char* state_uri = strjoin(base_uri, "/state.ttl");

	PluginState* state = load_state_from_file(
		jalv->world, &jalv->map, state_uri);

	free(state_uri);
	free(base_uri);
	return state;
}

static PluginState*
load_state_from_file(LilvWorld*    world,
                     LV2_URID_Map* map,
                     const char*   state_uri)
{
	RestoreData data;
	memset(&data, '\0', sizeof(RestoreData));
	data.world = world;
	data.map   = map;

	SerdReader* reader = serd_reader_new(
		SERD_TURTLE,
		&data, NULL,
		NULL,
		NULL,
		on_statement,
		NULL);

	SerdStatus st = serd_reader_read_file(reader, USTR(state_uri));
	if (st) {
		fprintf(stderr, "Error reading state from %s (%s)\n",
		        state_uri, serd_strerror(st));
		return NULL;
	}

	serd_reader_free(reader);

	PluginState* state = (PluginState*)malloc(sizeof(PluginState));
	state->plugin_uri = data.plugin_uri;
	state->props      = data.props;
	state->num_props  = data.num_props;
	state->values     = data.ports;
	state->num_values = data.num_ports;

	if (state->props) {
		qsort(state->props, state->num_props, sizeof(Property), property_cmp);
	}
	if (state->values) {
		qsort(state->values, state->num_values, sizeof(PortValue), value_cmp);
	}

	return state;
}

void
jalv_apply_state(Jalv* jalv, PluginState* state)
{
	/* Set port values */
	for (uint32_t i = 0; i < state->num_values; ++i) {
		PortValue*         dport = &state->values[i];
		const char*        sym   = (const char*)dport->symbol.buf;
		struct Port* const jport = jalv_port_by_symbol(jalv, sym);
		if (jport) {
			jport->control = atof((const char*)dport->value.buf);  // FIXME: Check type
		} else {
			fprintf(stderr, "error: Failed to find port `%s'\n", sym);
		}
	}

#ifdef HAVE_LV2_STATE
	const LV2_State_Interface* state_iface = (const LV2_State_Interface*)
		lilv_instance_get_extension_data(jalv->instance, LV2_STATE_INTERFACE_URI);

	if (state_iface) {
		RetrieveData data = { &jalv->map, state->props, state->num_props };
		state_iface->restore(lilv_instance_get_handle(jalv->instance),
		                     retrieve_callback,
		                     &data, 0, NULL);
	}
#endif  // HAVE_LV2_STATE
}

int
jalv_load_presets(Jalv* jalv, PresetSink sink, void* data)
{
	LilvNodes* presets = lilv_plugin_get_related(jalv->plugin,
	                                             jalv->preset_class);
	LILV_FOREACH(nodes, i, presets) {
		const LilvNode* preset = lilv_nodes_get(presets, i);
		lilv_world_load_resource(jalv->world, preset);
		LilvNodes* labels = lilv_world_find_nodes(
			jalv->world, preset, jalv->label_pred, NULL);
		if (labels) {
			const LilvNode* label = lilv_nodes_get_first(labels);
			sink(jalv, preset, label, data);
			lilv_nodes_free(labels);
		} else {
			fprintf(stderr, "Preset <%s> has no rdfs:label\n",
			        lilv_node_as_string(lilv_nodes_get(presets, i)));
		}
	}
	lilv_nodes_free(presets);

	return 0;
}

static inline const LilvNode*
get_value(LilvWorld* world, const LilvNode* subject, const LilvNode* predicate)
{
	LilvNodes* vs = lilv_world_find_nodes(world, subject, predicate, NULL);
	return vs ? lilv_nodes_get_first(vs) : NULL;
}

int
jalv_apply_preset(Jalv* jalv, const LilvNode* preset)
{
	LilvNode* lv2_port   = lilv_new_uri(jalv->world, NS_LV2 "port");
	LilvNode* lv2_symbol = lilv_new_uri(jalv->world, NS_LV2 "symbol");
	LilvNode* pset_value = lilv_new_uri(jalv->world, NS_PSET "value");

	LilvNodes* ports = lilv_world_find_nodes(
		jalv->world, preset, lv2_port, NULL);
	LILV_FOREACH(nodes, i, ports) {
		const LilvNode* port   = lilv_nodes_get(ports, i);
		const LilvNode* symbol = get_value(jalv->world, port, lv2_symbol);
		const LilvNode* value  = get_value(jalv->world, port, pset_value);
		if (!symbol) {
			fprintf(stderr, "error: Preset port missing symbol.\n");
		} else if (!value) {
			fprintf(stderr, "error: Preset port missing value.\n");
		} else if (!lilv_node_is_float(value) && !lilv_node_is_int(value)) {
			fprintf(stderr, "error: Preset port value is not a number.\n");
		} else {
			const char*  sym = lilv_node_as_string(symbol);
			struct Port* p   = jalv_port_by_symbol(jalv, sym);
			if (p) {
				const float fvalue = lilv_node_as_float(value);
				// Send value to plugin
				jalv_ui_write(jalv, p->index, sizeof(float), 0, &fvalue);

				// Update UI
				char buf[sizeof(ControlChange) + sizeof(float)];
				ControlChange* ev = (ControlChange*)buf;
				ev->index    = p->index;
				ev->protocol = 0;
				ev->size     = sizeof(float);
				*(float*)ev->body = fvalue;
				jack_ringbuffer_write(jalv->plugin_events, buf, sizeof(buf));
			} else {
				fprintf(stderr, "error: Preset port `%s' is missing\n", sym);
			}
		}
	}
	lilv_nodes_free(ports);

	lilv_node_free(pset_value);
	lilv_node_free(lv2_symbol);
	lilv_node_free(lv2_port);

	return 0;
}

static char*
pathify(const char* in, const char* ext)
{
	const size_t in_len  = strlen(in);
	const size_t ext_len = ext ? strlen(ext) : 0;

	char* out = calloc(in_len + ext_len + 1, 1);
	for (size_t i = 0; i < in_len; ++i) {
		char c = in[i];
		if (!((c >= 'a' && c <= 'z')
		      || (c >= 'A' && c <= 'Z')
		      || (c >= '0' && c <= '9'))) {
			c = '-';
		}
		out[i] = c;
	}
	if (ext) {
		memcpy(out + in_len, ext, ext_len);
	}
	return out;
}

void
jalv_save_port_values(Jalv*           jalv,
                      SerdWriter*     writer,
                      const SerdNode* subject)
{
	for (uint32_t i = 0; i < jalv->num_ports; ++i) {
		struct Port* const port = &jalv->ports[i];
		if (port->type != TYPE_CONTROL || port->flow != FLOW_INPUT) {
			continue;
		}

		const LilvNode* sym = lilv_port_get_symbol(jalv->plugin, port->lilv_port);
		LilvNode*       val = lilv_new_float(jalv->world, port->control);

		const SerdNode port_node = serd_node_from_string(
			SERD_BLANK, USTR(lilv_node_as_string(sym)));

		// <> lv2:port _:symbol
		SerdNode p = serd_node_from_string(SERD_URI,
		                                   USTR(NS_LV2 "port"));
		serd_writer_write_statement(writer, SERD_ANON_O_BEGIN,
		                            NULL, subject, &p, &port_node, NULL, NULL);

		// _:symbol lv2:symbol "symbol"
		p = serd_node_from_string(SERD_URI, USTR(NS_LV2 "symbol"));
		SerdNode o = serd_node_from_string(SERD_LITERAL,
		                                   USTR(lilv_node_as_string(sym)));
		serd_writer_write_statement(writer, SERD_ANON_CONT,
		                            NULL, &port_node, &p, &o, NULL, NULL);

		// _:symbol pset:value value
		p = serd_node_from_string(SERD_URI, USTR(NS_PSET "value"));
		o = serd_node_from_string(SERD_LITERAL,
		                          USTR(lilv_node_as_string(val)));
		SerdNode t = serd_node_from_string(SERD_URI, USTR(NS_XSD "decimal"));
		serd_writer_write_statement(writer, SERD_ANON_CONT,
		                            NULL, &port_node, &p, &o, &t, NULL);

		lilv_node_free(val);
		serd_writer_end_anon(writer, &port_node);
	}
}

static int
add_preset_to_manifest(const LilvPlugin* plugin,
                       const char*       manifest_path,
                       const char*       preset_uri,
                       const char*       preset_file);

int
jalv_save_preset(Jalv* jalv, const char* label)
{
	const char* const home = getenv("HOME");
	if (!home) {
		fprintf(stderr, "error: $HOME is undefined\n");
		return 1;
	}

	const char* const bundle_name = "presets.lv2/";

	// Create ~/.lv2/ and ~/.lv2/presets.lv2/ if necessary
	char* const lv2dir        = strjoin(home, "/.lv2/");
	char* const bundle        = strjoin(lv2dir, bundle_name);
	char* const filename      = pathify(label, ".ttl");
	char* const path          = strjoin(bundle, filename);
	char* const uri           = strjoin("file://", path);
	char* const manifest_path = strjoin(bundle, "manifest.ttl");

	int ret = 0;
	if ((mkdir(lv2dir, 0755) && errno != EEXIST)
	    || (mkdir(bundle, 0755) && errno != EEXIST)) {
		fprintf(stderr, "error: Unable to create %s (%s)\n",
		        lv2dir, strerror(errno));
		ret = 2;
		goto done;
	}

	// Write preset file
	write_preset(jalv, path, label);

	// Add entry to manifest
	add_preset_to_manifest(jalv->plugin, manifest_path, filename, filename);

done:
	free(manifest_path);
	free(uri);
	free(path);
	free(filename);
	free(bundle);
	free(lv2dir);

	return ret;
}

static int
add_preset_to_manifest(const LilvPlugin* plugin,
                       const char*       manifest_path,
                       const char*       preset_uri,
                       const char*       preset_file)
{
	FILE* fd = fopen((char*)manifest_path, "a");
	if (!fd) {
		fprintf(stderr, "error: Failed to open %s (%s)\n",
		        manifest_path, strerror(errno));
		return 4;
	}

	SerdEnv* env = serd_env_new(NULL);
	serd_env_set_prefix_from_strings(env, USTR("lv2"),   USTR(NS_LV2));
	serd_env_set_prefix_from_strings(env, USTR("pset"),  USTR(NS_PSET));
	serd_env_set_prefix_from_strings(env, USTR("rdfs"),  USTR(NS_RDFS));

#ifdef HAVE_LOCKF
	lockf(fileno(fd), F_LOCK, 0);
#endif

	char* const manifest_uri = strjoin("file://", manifest_path);

	SerdURI base_uri;
	SerdNode base = serd_node_new_uri_from_string(
		(const uint8_t*)manifest_uri, NULL, &base_uri);

	SerdWriter* writer = serd_writer_new(
		SERD_TURTLE, SERD_STYLE_ABBREVIATED|SERD_STYLE_CURIED,
		env, &base_uri,
		file_sink,
		fd);

	fseek(fd, 0, SEEK_END);
	if (ftell(fd) == 0) {
		serd_env_foreach(env, (SerdPrefixSink)serd_writer_set_prefix, writer);
	}

	SerdNode s = serd_node_from_string(SERD_URI, USTR(preset_uri));

	// <preset> a pset:Preset
	SerdNode p = serd_node_from_string(SERD_URI, USTR(NS_RDF "type"));
	SerdNode o = serd_node_from_string(SERD_CURIE, USTR("pset:Preset"));
	serd_writer_write_statement(writer, 0, NULL, &s, &p, &o, NULL, NULL);

	// <preset> rdfs:seeAlso <preset>
	p = serd_node_from_string(SERD_URI, USTR(NS_RDFS "seeAlso"));
	serd_writer_write_statement(writer, 0, NULL, &s, &p, &s, NULL, NULL);

	// <preset> lv2:appliesTo <plugin>
	p = serd_node_from_string(SERD_URI, USTR(NS_LV2 "appliesTo"));
	o = serd_node_from_string(
		SERD_URI, USTR(lilv_node_as_string(lilv_plugin_get_uri(plugin))));
	serd_writer_write_statement(writer, 0, NULL, &s, &p, &o, NULL, NULL);

	serd_writer_free(writer);
	serd_node_free(&base);

#ifdef HAVE_LOCKF
	lockf(fileno(fd), F_ULOCK, 0);
#endif

	fclose(fd);
	free(manifest_uri);
	serd_env_free(env);

	return 0;
}
