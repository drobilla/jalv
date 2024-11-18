// Copyright 2007-2022 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_URIDS_H
#define JALV_URIDS_H

#include "attributes.h"
#include "mapper.h"

#include <lv2/urid/urid.h>

// Cached LV2 URIDs
JALV_BEGIN_DECLS

typedef struct {
  LV2_URID atom_Chunk;
  LV2_URID atom_Float;
  LV2_URID atom_Int;
  LV2_URID atom_Object;
  LV2_URID atom_Path;
  LV2_URID atom_Sequence;
  LV2_URID atom_String;
  LV2_URID atom_eventTransfer;
  LV2_URID bufsz_maxBlockLength;
  LV2_URID bufsz_minBlockLength;
  LV2_URID bufsz_sequenceSize;
  LV2_URID log_Error;
  LV2_URID log_Trace;
  LV2_URID log_Warning;
  LV2_URID midi_MidiEvent;
  LV2_URID param_sampleRate;
  LV2_URID patch_Get;
  LV2_URID patch_Put;
  LV2_URID patch_Set;
  LV2_URID patch_body;
  LV2_URID patch_property;
  LV2_URID patch_value;
  LV2_URID time_Position;
  LV2_URID time_bar;
  LV2_URID time_barBeat;
  LV2_URID time_beatUnit;
  LV2_URID time_beatsPerBar;
  LV2_URID time_beatsPerMinute;
  LV2_URID time_frame;
  LV2_URID time_speed;
  LV2_URID ui_scaleFactor;
  LV2_URID ui_updateRate;
} JalvURIDs;

void
jalv_init_urids(JalvMapper* mapper, JalvURIDs* urids);

JALV_END_DECLS

#endif // JALV_URIDS_H
