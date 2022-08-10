// Copyright 2007-2022 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_TYPES_H
#define JALV_TYPES_H

#include "attributes.h"

JALV_BEGIN_DECLS

/// Backend playing state
typedef enum { JALV_RUNNING, JALV_PAUSE_REQUESTED, JALV_PAUSED } JalvPlayState;

/// "Global" application state
typedef struct JalvImpl Jalv;

/// Audio/MIDI backend
typedef struct JalvBackendImpl JalvBackend;

JALV_END_DECLS

#endif // JALV_TYPES_H
