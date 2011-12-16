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

#include "jalv-config.h"
#include "jalv_internal.h"

#define NS_LV2  "http://lv2plug.in/ns/lv2core#"
#define NS_PSET "http://lv2plug.in/ns/ext/presets#"

#define USTR(s) ((const uint8_t*)s)

int
jalv_load_presets(Jalv* jalv, PresetSink sink, void* data)
{
	LilvNodes* presets = lilv_plugin_get_related(jalv->plugin,
	                                             jalv->preset_class);
	LILV_FOREACH(nodes, i, presets) {
		const LilvNode* preset = lilv_nodes_get(presets, i); 
		lilv_world_load_resource(jalv->world, preset);
		LilvNodes* titles = lilv_world_find_nodes(
			jalv->world, preset, jalv->label_pred, NULL);
		if (titles) {
			const LilvNode* title = lilv_nodes_get_first(titles);
			sink(jalv, preset, title, data);
			lilv_nodes_free(titles);
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
jalv_apply_preset(Jalv* jalv, LilvNode* preset)
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
				jalv_ui_write(jalv, p->index, sizeof(float), 0, &fvalue);
			} else {
				fprintf(stderr, "error: Preset port `%s' is missing\n", sym);
			}
		}
	}
	lilv_nodes_free(ports);

	lilv_node_free(pset_value);
	lilv_node_free(preset);
	lilv_node_free(lv2_symbol);
	lilv_node_free(lv2_port);

	return 0;
}
