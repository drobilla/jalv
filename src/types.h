// Copyright 2007-2024 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_TYPES_H
#define JALV_TYPES_H

#include "attributes.h"

// Basic internal type declarations
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

/// Plugin port "direction"
typedef enum { FLOW_UNKNOWN, FLOW_INPUT, FLOW_OUTPUT } PortFlow;

/// Plugin port type
typedef enum {
  TYPE_UNKNOWN,
  TYPE_CONTROL,
  TYPE_AUDIO,
  TYPE_EVENT,
  TYPE_CV
} PortType;

/// Command-line arguments passed to a frontend/program
typedef struct {
  int    argc; ///< Argument count as in `main`
  char** argv; ///< argument vector as in `main`
} ProgramArgs;

JALV_END_DECLS

#endif // JALV_TYPES_H
