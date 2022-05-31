// Copyright 2007-2022 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_TYPES_H
#define JALV_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

/// Backend playing state
typedef enum { JALV_RUNNING, JALV_PAUSE_REQUESTED, JALV_PAUSED } JalvPlayState;

/// "Global" application state
typedef struct JalvImpl Jalv;

/// Audio/MIDI backend
typedef struct JalvBackendImpl JalvBackend;

#ifdef __cplusplus
} // extern "C"
#endif

#endif // JALV_TYPES_H
