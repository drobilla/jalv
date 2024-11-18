// Copyright 2007-2022 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "log.h"

#include "jalv_config.h"

#include "lv2/log/log.h"
#include "lv2/urid/urid.h"

#if USE_ISATTY
#  include <unistd.h>
#endif

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>

JALV_LOG_FUNC(2, 0)
static int
jalv_vlog(const JalvLogLevel level, const char* const fmt, va_list ap)
{
  bool fancy = false;
  switch (level) {
  case JALV_LOG_ERR:
    fancy = jalv_ansi_start(stderr, 31);
    fprintf(stderr, "error: ");
    break;
  case JALV_LOG_WARNING:
    fancy = jalv_ansi_start(stderr, 33);
    fprintf(stderr, "warning: ");
    break;
  case JALV_LOG_INFO:
    break;
  case JALV_LOG_DEBUG:
    fancy = jalv_ansi_start(stderr, 32);
    fprintf(stderr, "trace: ");
    break;
  }

  const int st = vfprintf(stderr, fmt, ap);

  if (fancy) {
    jalv_ansi_reset(stderr);
  }

  return st;
}

int
jalv_log(const JalvLogLevel level, const char* const fmt, ...)
{
  va_list args;
  va_start(args, fmt);

  const int ret = jalv_vlog(level, fmt, args);

  va_end(args);
  return ret;
}

int
jalv_vprintf(LV2_Log_Handle handle, LV2_URID type, const char* fmt, va_list ap)
{
  JalvLog* const log = (JalvLog*)handle;

  if (type == log->urids->log_Trace) {
    return log->tracing ? jalv_vlog(JALV_LOG_DEBUG, fmt, ap) : 0;
  }

  if (type == log->urids->log_Error) {
    return jalv_vlog(JALV_LOG_ERR, fmt, ap);
  }

  if (type == log->urids->log_Warning) {
    return jalv_vlog(JALV_LOG_WARNING, fmt, ap);
  }

  return vfprintf(stderr, fmt, ap);
}

int
jalv_printf(LV2_Log_Handle handle, LV2_URID type, const char* fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  const int ret = jalv_vprintf(handle, type, fmt, args);
  va_end(args);
  return ret;
}

bool
jalv_ansi_start(FILE* stream, int color)
{
#if USE_ISATTY && USE_FILENO
  if (isatty(fileno(stream))) {
    return fprintf(stream, "\033[0;%dm", color);
  }
#endif
  return 0;
}

void
jalv_ansi_reset(FILE* stream)
{
#if USE_ISATTY
  if (isatty(fileno(stream))) {
    fprintf(stream, "\033[0m");
    fflush(stream);
  }
#endif
}
