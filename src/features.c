// Copyright 2018-2024 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "features.h"

#include "macros.h"
#include "settings.h"
#include "urids.h"

#include <lv2/options/options.h>

#include <stdint.h>
#include <string.h>

void
jalv_init_lv2_options(JalvFeatures* const       features,
                      const JalvURIDs* const    urids,
                      const JalvSettings* const settings)
{
  const LV2_Options_Option options[ARRAY_SIZE(features->options)] = {
    {LV2_OPTIONS_INSTANCE,
     0,
     urids->param_sampleRate,
     sizeof(float),
     urids->atom_Float,
     &settings->sample_rate},
    {LV2_OPTIONS_INSTANCE,
     0,
     urids->bufsz_minBlockLength,
     sizeof(int32_t),
     urids->atom_Int,
     &settings->block_length},
    {LV2_OPTIONS_INSTANCE,
     0,
     urids->bufsz_maxBlockLength,
     sizeof(int32_t),
     urids->atom_Int,
     &settings->block_length},
    {LV2_OPTIONS_INSTANCE,
     0,
     urids->bufsz_sequenceSize,
     sizeof(int32_t),
     urids->atom_Int,
     &settings->midi_buf_size},
    {LV2_OPTIONS_INSTANCE,
     0,
     urids->ui_updateRate,
     sizeof(float),
     urids->atom_Float,
     &settings->ui_update_hz},
    {LV2_OPTIONS_INSTANCE,
     0,
     urids->ui_scaleFactor,
     sizeof(float),
     urids->atom_Float,
     &settings->ui_scale_factor},
    {LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, NULL}};

  memcpy(features->options, options, sizeof(features->options));
  features->options_feature.URI  = LV2_OPTIONS__options;
  features->options_feature.data = (void*)features->options;
}
