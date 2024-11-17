// Copyright 2007-2024 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_TYPES_H
#define JALV_TYPES_H

#include "attributes.h"

JALV_BEGIN_DECLS

/// Process thread running state
typedef enum {
  JALV_RUNNING, ///< Active and running the plugin
  JALV_PAUSED,  ///< Active but bypassing the plugin (silent)
} JalvRunState;

/// "Global" application state
typedef struct JalvImpl Jalv;

/// Audio/MIDI backend
typedef struct JalvBackendImpl JalvBackend;

JALV_END_DECLS

#endif // JALV_TYPES_H
