// Copyright 2007-2022 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_LOG_H
#define JALV_LOG_H

#include "attributes.h"
#include "types.h"
#include "urids.h"
#include "control.h"

#include "lv2/log/log.h"
#include "lv2/urid/urid.h"

#include <stdbool.h>
#include <stdio.h>
#include <pthread.h>

#ifdef __GNUC__
#  define JALV_LOG_FUNC(fmt, arg1) __attribute__((format(printf, fmt, arg1)))
#else
#  define JALV_LOG_FUNC(fmt, arg1)
#endif

JALV_BEGIN_DECLS

struct Port;

// String and log utilities

typedef enum {
  JALV_LOG_ERR     = 3,
  JALV_LOG_WARNING = 4,
  JALV_LOG_INFO    = 6,
  JALV_LOG_DEBUG   = 7,
} JalvLogLevel;

typedef struct {
  JalvURIDs* urids;
  bool       tracing;
} JalvLog;

//-----------------------------------------------------------------------------
// Zynthian Extended Terminal Control
//-----------------------------------------------------------------------------

void
jalv_print_port(Jalv* const jalv, const struct Port* port, const float value);

void
jalv_print_control(Jalv* const jalv, const ControlID* control, const float value);

void
jalv_print_control_path(Jalv* const jalv, const ControlID* control, const char* value);

void
jalv_print_controls(Jalv* const jalv, bool writable, bool readable);

void
jalv_print_ports(Jalv* const jalv, bool writable, bool readable);

int
jalv_print_preset(Jalv* const jalv, const LilvNode* node, const LilvNode* title, void* data);

void
jalv_print_preset_str(char *uri, char *name);

//-----------------------------------------------------------------------------

char*
jalv_strdup(const char* str);

char*
jalv_strjoin(const char* a, const char* b);

JALV_LOG_FUNC(2, 0)
int
jalv_vlog(JalvLogLevel level, const char* fmt, va_list ap);

JALV_LOG_FUNC(2, 3)
int
jalv_log(JalvLogLevel level, const char* fmt, ...);

JALV_LOG_FUNC(3, 0)
int
jalv_vprintf(LV2_Log_Handle handle, LV2_URID type, const char* fmt, va_list ap);

JALV_LOG_FUNC(3, 4)
int
jalv_printf(LV2_Log_Handle handle, LV2_URID type, const char* fmt, ...);

bool
jalv_ansi_start(FILE* stream, int color);

void
jalv_ansi_reset(FILE* stream);

JALV_END_DECLS

#endif // JALV_LOG_H
