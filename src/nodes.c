// Copyright 2022-2024 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "nodes.h"

#include "lilv/lilv.h"
#include "lv2/atom/atom.h"
#include "lv2/core/lv2.h"
#include "lv2/midi/midi.h"
#include "lv2/port-groups/port-groups.h"
#include "lv2/port-props/port-props.h"
#include "lv2/presets/presets.h"
#include "lv2/resize-port/resize-port.h"
#include "lv2/state/state.h"
#include "lv2/ui/ui.h"
#include "lv2/worker/worker.h"

#include <stddef.h>

void
jalv_init_nodes(LilvWorld* const world, JalvNodes* const nodes)
{
#define MAP_NODE(uri) lilv_new_uri(world, (uri))

  nodes->atom_AtomPort           = MAP_NODE(LV2_ATOM__AtomPort);
  nodes->atom_Chunk              = MAP_NODE(LV2_ATOM__Chunk);
  nodes->atom_Float              = MAP_NODE(LV2_ATOM__Float);
  nodes->atom_Path               = MAP_NODE(LV2_ATOM__Path);
  nodes->atom_Sequence           = MAP_NODE(LV2_ATOM__Sequence);
  nodes->lv2_AudioPort           = MAP_NODE(LV2_CORE__AudioPort);
  nodes->lv2_CVPort              = MAP_NODE(LV2_CORE__CVPort);
  nodes->lv2_ControlPort         = MAP_NODE(LV2_CORE__ControlPort);
  nodes->lv2_InputPort           = MAP_NODE(LV2_CORE__InputPort);
  nodes->lv2_OutputPort          = MAP_NODE(LV2_CORE__OutputPort);
  nodes->lv2_connectionOptional  = MAP_NODE(LV2_CORE__connectionOptional);
  nodes->lv2_control             = MAP_NODE(LV2_CORE__control);
  nodes->lv2_default             = MAP_NODE(LV2_CORE__default);
  nodes->lv2_designation         = MAP_NODE(LV2_CORE__designation);
  nodes->lv2_enumeration         = MAP_NODE(LV2_CORE__enumeration);
  nodes->lv2_extensionData       = MAP_NODE(LV2_CORE__extensionData);
  nodes->lv2_integer             = MAP_NODE(LV2_CORE__integer);
  nodes->lv2_latency             = MAP_NODE(LV2_CORE__latency);
  nodes->lv2_maximum             = MAP_NODE(LV2_CORE__maximum);
  nodes->lv2_minimum             = MAP_NODE(LV2_CORE__minimum);
  nodes->lv2_name                = MAP_NODE(LV2_CORE__name);
  nodes->lv2_reportsLatency      = MAP_NODE(LV2_CORE__reportsLatency);
  nodes->lv2_sampleRate          = MAP_NODE(LV2_CORE__sampleRate);
  nodes->lv2_symbol              = MAP_NODE(LV2_CORE__symbol);
  nodes->lv2_toggled             = MAP_NODE(LV2_CORE__toggled);
  nodes->midi_MidiEvent          = MAP_NODE(LV2_MIDI__MidiEvent);
  nodes->pg_group                = MAP_NODE(LV2_PORT_GROUPS__group);
  nodes->pprops_logarithmic      = MAP_NODE(LV2_PORT_PROPS__logarithmic);
  nodes->pprops_notOnGUI         = MAP_NODE(LV2_PORT_PROPS__notOnGUI);
  nodes->pprops_rangeSteps       = MAP_NODE(LV2_PORT_PROPS__rangeSteps);
  nodes->pset_Preset             = MAP_NODE(LV2_PRESETS__Preset);
  nodes->pset_bank               = MAP_NODE(LV2_PRESETS__bank);
  nodes->rdfs_comment            = MAP_NODE(LILV_NS_RDFS "comment");
  nodes->rdfs_label              = MAP_NODE(LILV_NS_RDFS "label");
  nodes->rdfs_range              = MAP_NODE(LILV_NS_RDFS "range");
  nodes->rsz_minimumSize         = MAP_NODE(LV2_RESIZE_PORT__minimumSize);
  nodes->state_threadSafeRestore = MAP_NODE(LV2_STATE__threadSafeRestore);
  nodes->ui_showInterface        = MAP_NODE(LV2_UI__showInterface);
  nodes->work_interface          = MAP_NODE(LV2_WORKER__interface);
  nodes->work_schedule           = MAP_NODE(LV2_WORKER__schedule);
  nodes->end                     = NULL;

#undef MAP_NODE
}
