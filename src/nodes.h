// Copyright 2007-2022 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_NODES_H
#define JALV_NODES_H

#include "attributes.h"

#include <lilv/lilv.h>

// Cached lilv nodes
JALV_BEGIN_DECLS

typedef struct {
  LilvNode* atom_AtomPort;
  LilvNode* atom_Chunk;
  LilvNode* atom_Float;
  LilvNode* atom_Path;
  LilvNode* atom_Sequence;
  LilvNode* lv2_AudioPort;
  LilvNode* lv2_CVPort;
  LilvNode* lv2_ControlPort;
  LilvNode* lv2_InputPort;
  LilvNode* lv2_OutputPort;
  LilvNode* lv2_connectionOptional;
  LilvNode* lv2_control;
  LilvNode* lv2_default;
  LilvNode* lv2_designation;
  LilvNode* lv2_enumeration;
  LilvNode* lv2_extensionData;
  LilvNode* lv2_integer;
  LilvNode* lv2_latency;
  LilvNode* lv2_maximum;
  LilvNode* lv2_minimum;
  LilvNode* lv2_name;
  LilvNode* lv2_reportsLatency;
  LilvNode* lv2_sampleRate;
  LilvNode* lv2_symbol;
  LilvNode* lv2_toggled;
  LilvNode* midi_MidiEvent;
  LilvNode* pg_group;
  LilvNode* pprops_logarithmic;
  LilvNode* pprops_notOnGUI;
  LilvNode* pprops_rangeSteps;
  LilvNode* pset_Preset;
  LilvNode* pset_bank;
  LilvNode* rdfs_comment;
  LilvNode* rdfs_label;
  LilvNode* rdfs_range;
  LilvNode* rsz_minimumSize;
  LilvNode* state_threadSafeRestore;
  LilvNode* time_Position;
  LilvNode* time_beatsPerMinute;
  LilvNode* ui_showInterface;
  LilvNode* work_interface;
  LilvNode* work_schedule;
  LilvNode* end; ///< NULL terminator for easy freeing of entire structure
} JalvNodes;

void
jalv_init_nodes(LilvWorld* world, JalvNodes* nodes);

void
jalv_free_nodes(JalvNodes* nodes);

JALV_END_DECLS

#endif // JALV_NODES_H
