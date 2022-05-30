// Copyright 2007-2022 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#define _POSIX_C_SOURCE 200809L

#include "log.h"

#include "jalv_config.h"
#include "jalv_internal.h"
#include "options.h"
#include "port.h"
#include "urids.h"

#include "lilv/lilv.h"
#include "lv2/log/log.h"
#include "lv2/urid/urid.h"

#ifdef HAVE_ISATTY
#  include <unistd.h>
#endif

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void
jalv_print_control(Jalv* const              jalv,
                   const struct Port* const port,
                   const float              value)
{
  const LilvNode* sym = lilv_port_get_symbol(jalv->plugin, port->lilv_port);
  printf("%s = %f\n", lilv_node_as_string(sym), value);
}

char*
jalv_strdup(const char* const str)
{
  const size_t len  = strlen(str);
  char*        copy = (char*)malloc(len + 1);
  memcpy(copy, str, len + 1);
  return copy;
}

char*
jalv_strjoin(const char* const a, const char* const b)
{
  const size_t a_len = strlen(a);
  const size_t b_len = strlen(b);
  char* const  out   = (char*)malloc(a_len + b_len + 1);

  memcpy(out, a, a_len);
  memcpy(out + a_len, b, b_len);
  out[a_len + b_len] = '\0';

  return out;
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

int
jalv_vprintf(LV2_Log_Handle handle, LV2_URID type, const char* fmt, va_list ap)
{
  // TODO: Lock
  Jalv* jalv  = (Jalv*)handle;
  bool  fancy = true;
  if (type == jalv->urids.log_Trace && jalv->opts.trace) {
    jalv_ansi_start(stderr, 32);
    fprintf(stderr, "trace: ");
  } else if (type == jalv->urids.log_Error) {
    jalv_ansi_start(stderr, 31);
    fprintf(stderr, "error: ");
  } else if (type == jalv->urids.log_Warning) {
    jalv_ansi_start(stderr, 33);
    fprintf(stderr, "warning: ");
  } else {
    fancy = false;
  }

  const int st = vfprintf(stderr, fmt, ap);

  if (fancy) {
    jalv_ansi_reset(stderr);
  }

  return st;
}

bool
jalv_ansi_start(FILE* stream, int color)
{
#if defined(HAVE_ISATTY) && defined(HAVE_FILENO)
  if (isatty(fileno(stream))) {
    return fprintf(stream, "\033[0;%dm", color);
  }
#endif
  return 0;
}

void
jalv_ansi_reset(FILE* stream)
{
#ifdef HAVE_ISATTY
  if (isatty(fileno(stream))) {
    fprintf(stream, "\033[0m");
    fflush(stream);
  }
#endif
}
