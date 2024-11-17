// Copyright 2012-2024 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "query.h"

#include "lv2/core/lv2.h"
#include "lv2/ui/ui.h"

bool
jalv_port_has_designation(const JalvNodes* const  nodes,
                          const LilvPlugin* const plugin,
                          const LilvPort* const   port,
                          const LilvNode* const   designation)
{
  LilvNodes* const designations =
    lilv_port_get_value(plugin, port, nodes->lv2_designation);

  bool found = false;
  LILV_FOREACH (nodes, n, designations) {
    const LilvNode* const node = lilv_nodes_get(designations, n);
    if (lilv_node_equals(node, designation)) {
      found = true;
      break;
    }
  }

  lilv_nodes_free(designations);
  return found;
}

bool
jalv_ui_is_resizable(LilvWorld* const world, const LilvUI* const ui)
{
  if (!ui) {
    return false;
  }

  const LilvNode* s   = lilv_ui_get_uri(ui);
  LilvNode*       p   = lilv_new_uri(world, LV2_CORE__optionalFeature);
  LilvNode*       fs  = lilv_new_uri(world, LV2_UI__fixedSize);
  LilvNode*       nrs = lilv_new_uri(world, LV2_UI__noUserResize);

  LilvNodes* fs_matches  = lilv_world_find_nodes(world, s, p, fs);
  LilvNodes* nrs_matches = lilv_world_find_nodes(world, s, p, nrs);

  lilv_nodes_free(nrs_matches);
  lilv_nodes_free(fs_matches);
  lilv_node_free(nrs);
  lilv_node_free(fs);
  lilv_node_free(p);

  return !fs_matches && !nrs_matches;
}
