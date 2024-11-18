// Copyright 2022-2024 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "urids.h"

#include "mapper.h"

#include <lv2/atom/atom.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/log/log.h>
#include <lv2/midi/midi.h>
#include <lv2/parameters/parameters.h>
#include <lv2/patch/patch.h>
#include <lv2/time/time.h>
#include <lv2/ui/ui.h>

void
jalv_init_urids(JalvMapper* const mapper, JalvURIDs* const urids)
{
#define MAP_URI(uri) jalv_mapper_map_uri(mapper, (uri))

  urids->atom_Chunk           = MAP_URI(LV2_ATOM__Chunk);
  urids->atom_Float           = MAP_URI(LV2_ATOM__Float);
  urids->atom_Int             = MAP_URI(LV2_ATOM__Int);
  urids->atom_Object          = MAP_URI(LV2_ATOM__Object);
  urids->atom_Path            = MAP_URI(LV2_ATOM__Path);
  urids->atom_Sequence        = MAP_URI(LV2_ATOM__Sequence);
  urids->atom_String          = MAP_URI(LV2_ATOM__String);
  urids->atom_eventTransfer   = MAP_URI(LV2_ATOM__eventTransfer);
  urids->bufsz_maxBlockLength = MAP_URI(LV2_BUF_SIZE__maxBlockLength);
  urids->bufsz_minBlockLength = MAP_URI(LV2_BUF_SIZE__minBlockLength);
  urids->bufsz_sequenceSize   = MAP_URI(LV2_BUF_SIZE__sequenceSize);
  urids->log_Error            = MAP_URI(LV2_LOG__Error);
  urids->log_Trace            = MAP_URI(LV2_LOG__Trace);
  urids->log_Warning          = MAP_URI(LV2_LOG__Warning);
  urids->midi_MidiEvent       = MAP_URI(LV2_MIDI__MidiEvent);
  urids->param_sampleRate     = MAP_URI(LV2_PARAMETERS__sampleRate);
  urids->patch_Get            = MAP_URI(LV2_PATCH__Get);
  urids->patch_Put            = MAP_URI(LV2_PATCH__Put);
  urids->patch_Set            = MAP_URI(LV2_PATCH__Set);
  urids->patch_body           = MAP_URI(LV2_PATCH__body);
  urids->patch_property       = MAP_URI(LV2_PATCH__property);
  urids->patch_value          = MAP_URI(LV2_PATCH__value);
  urids->time_Position        = MAP_URI(LV2_TIME__Position);
  urids->time_bar             = MAP_URI(LV2_TIME__bar);
  urids->time_barBeat         = MAP_URI(LV2_TIME__barBeat);
  urids->time_beatUnit        = MAP_URI(LV2_TIME__beatUnit);
  urids->time_beatsPerBar     = MAP_URI(LV2_TIME__beatsPerBar);
  urids->time_beatsPerMinute  = MAP_URI(LV2_TIME__beatsPerMinute);
  urids->time_frame           = MAP_URI(LV2_TIME__frame);
  urids->time_speed           = MAP_URI(LV2_TIME__speed);
  urids->ui_scaleFactor       = MAP_URI(LV2_UI__scaleFactor);
  urids->ui_updateRate        = MAP_URI(LV2_UI__updateRate);

#undef MAP_URI
}
