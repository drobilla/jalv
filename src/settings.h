// Copyright 2018-2024 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_SETTINGS_H
#define JALV_SETTINGS_H

#include "attributes.h"

#include <stddef.h>
#include <stdint.h>

// Process thread settings
JALV_BEGIN_DECLS

/// System and/or configuration settings for the execution process
typedef struct {
  float    sample_rate;     ///< Sample rate
  uint32_t block_length;    ///< Audio buffer length in frames
  size_t   midi_buf_size;   ///< MIDI buffer size in bytes
  uint32_t ring_size;       ///< Communication ring size in bytes
  float    ui_update_hz;    ///< Frequency of UI updates
  float    ui_scale_factor; ///< UI scale factor
} JalvSettings;

JALV_END_DECLS

#endif // JALV_SETTINGS_H
