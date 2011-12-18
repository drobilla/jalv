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

#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "lilv/lilv.h"

#include "jalv-config.h"
#include "jalv_internal.h"

#define NS_LV2  "http://lv2plug.in/ns/lv2core#"
#define NS_PSET "http://lv2plug.in/ns/ext/presets#"
#define NS_RDF  "http://www.w3.org/1999/02/22-rdf-syntax-ns#"
#define NS_RDFS "http://www.w3.org/2000/01/rdf-schema#"
#define NS_XSD  "http://www.w3.org/2001/XMLSchema#"

#define USTR(s) ((const uint8_t*)s)

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

static size_t
file_sink(const void* buf, size_t len, void* stream)
{
	FILE* file = (FILE*)stream;
	return fwrite(buf, 1, len, file);
}

static char*
pathify(const char* in, const char* ext)
{
	const size_t in_len  = strlen(in);
	const size_t ext_len = ext ? strlen(ext) : 0;

	char* out = malloc(in_len + ext_len + 1);
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

static char*
strjoin(const char* a, const char* b)
{
	const size_t a_len = strlen(a);
	const size_t b_len = strlen(a);
	char* const  out   = malloc(a_len + b_len + 1);

	memcpy(out,         a, a_len);
	memcpy(out + a_len, b, b_len);
	out[a_len + b_len] = '\0';

	return out;
}

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
	char* const manifest_uri  = strjoin("file: //", manifest_path);

	int ret = 0;
	if ((mkdir(lv2dir, 0755) && errno != EEXIST)
	    || (mkdir(bundle, 0755) && errno != EEXIST)) {
		fprintf(stderr, "error: Unable to create %s (%s)\n",
		        lv2dir, strerror(errno));
		ret = 2;
		goto done;
	}

	// Open preset file
	FILE* fd = fopen((char*)path, "w");
	if (!fd) {
		fprintf(stderr, "error: Failed to open %s (%s)\n",
		        path, strerror(errno));
		ret = 3;
		goto done;
	}

	SerdURI  base_uri;
	SerdNode base = serd_node_new_uri_from_string((const uint8_t*)uri,
	                                              NULL, &base_uri);

	SerdEnv* env = serd_env_new(&base);
	serd_env_set_prefix_from_strings(env, USTR("lv2"),  USTR(NS_LV2));
	serd_env_set_prefix_from_strings(env, USTR("pset"), USTR(NS_PSET));
	serd_env_set_prefix_from_strings(env, USTR("rdfs"), USTR(NS_RDFS));

	// Write preset file

	SerdWriter* writer = serd_writer_new(
		SERD_TURTLE, SERD_STYLE_ABBREVIATED|SERD_STYLE_CURIED,
		env, &base_uri,
		file_sink,
		fd);

	serd_env_foreach(env, (SerdPrefixSink)serd_writer_set_prefix, writer);

	// <> a pset:Preset
	SerdNode s = serd_node_from_string(SERD_URI, USTR(""));
	SerdNode p = serd_node_from_string(SERD_URI, USTR(NS_RDF "type"));
	SerdNode o = serd_node_from_string(SERD_CURIE, USTR("pset:Preset"));
	serd_writer_write_statement(writer, 0, NULL, &s, &p, &o, NULL, NULL);

	// <> rdfs:label label
	p = serd_node_from_string(SERD_URI, USTR(NS_RDFS "label"));
	o = serd_node_from_string(SERD_LITERAL, USTR(label));
	serd_writer_write_statement(writer, 0,
	                            NULL, &s, &p, &o, NULL, NULL);

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
		p = serd_node_from_string(SERD_URI, USTR(NS_LV2 "port"));
		serd_writer_write_statement(writer, SERD_ANON_O_BEGIN,
		                            NULL, &s, &p, &port_node, NULL, NULL);

		// _:symbol lv2:symbol "symbol"
		p = serd_node_from_string(SERD_URI, USTR(NS_LV2 "symbol"));
		o = serd_node_from_string(SERD_LITERAL,
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

	serd_writer_free(writer);
	serd_node_free(&base);

	// Append record to manifest

	fclose(fd);
	fd = fopen((char*)manifest_path, "a");
	if (!fd) {
		fprintf(stderr, "error: Failed to open %s (%s)\n",
		        path, strerror(errno));
		serd_env_free(env);
		ret = 4;
		goto done;
	}

	base = serd_node_new_uri_from_string((const uint8_t*)manifest_uri,
	                                     NULL, &base_uri);

	writer = serd_writer_new(
		SERD_TURTLE, SERD_STYLE_ABBREVIATED|SERD_STYLE_CURIED,
		env, &base_uri,
		file_sink,
		fd);

	fseek(fd, 0, SEEK_END);
	if (ftell(fd) == 0) {
		serd_env_foreach(env, (SerdPrefixSink)serd_writer_set_prefix, writer);
	}

	s = serd_node_from_string(SERD_URI, USTR(filename));

	// <preset> a pset:Preset
	p = serd_node_from_string(SERD_URI, USTR(NS_RDF "type"));
	o = serd_node_from_string(SERD_CURIE, USTR("pset:Preset"));
	serd_writer_write_statement(writer, 0, NULL, &s, &p, &o, NULL, NULL);

	// <preset> rdfs:seeAlso <preset>
	p = serd_node_from_string(SERD_URI, USTR(NS_RDFS "seeAlso"));
	serd_writer_write_statement(writer, 0, NULL, &s, &p, &s, NULL, NULL);

	// <preset> lv2:appliesTo <plugin>
	p = serd_node_from_string(SERD_URI, USTR(NS_LV2 "appliesTo"));
	o = serd_node_from_string(
		SERD_URI, USTR(lilv_node_as_string(lilv_plugin_get_uri(jalv->plugin))));
	serd_writer_write_statement(writer, 0, NULL, &s, &p, &o, NULL, NULL);

	serd_writer_free(writer);
	serd_env_free(env);
	serd_node_free(&base);
	fclose(fd);

done:
	free(manifest_uri);
	free(manifest_path);
	free(uri);
	free(path);
	free(filename);
	free(bundle);
	free(lv2dir);

	return ret;
}
