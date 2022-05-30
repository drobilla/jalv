// Copyright 2007-2022 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_LOG_H
#define JALV_LOG_H

#include "jalv_internal.h"

#include "lv2/log/log.h"
#include "lv2/urid/urid.h"

#include <stdbool.h>
#include <stdio.h>

#ifdef __GNUC__
#  define JALV_LOG_FUNC(fmt, arg1) __attribute__((format(printf, fmt, arg1)))
#else
#  define JALV_LOG_FUNC(fmt, arg1)
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct Port;

// String and log utilities

void
jalv_print_control(Jalv* jalv, const struct Port* port, float value);

char*
jalv_strdup(const char* str);

char*
jalv_strjoin(const char* a, const char* b);

JALV_LOG_FUNC(3, 4)
int
jalv_printf(LV2_Log_Handle handle, LV2_URID type, const char* fmt, ...);

JALV_LOG_FUNC(3, 0)
int
jalv_vprintf(LV2_Log_Handle handle, LV2_URID type, const char* fmt, va_list ap);

bool
jalv_ansi_start(FILE* stream, int color);

void
jalv_ansi_reset(FILE* stream);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // JALV_LOG_H
