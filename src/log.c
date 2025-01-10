// Copyright 2007-2022 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "log.h"

#include "jalv_config.h"
#include "jalv_internal.h"
#include "port.h"
#include "urids.h"
#include "state.h"

#include "lilv/lilv.h"
#include "lv2/log/log.h"
#include "lv2/urid/urid.h"

#if USE_ISATTY
#  include <unistd.h>
#endif

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

//-----------------------------------------------------------------------------
// Zynthian Extended Terminal Control
//-----------------------------------------------------------------------------

char * CTR_PREFIX = "#CTR>";
char * MON_PREFIX = "#MON>";
char * PRS_PREFIX = "#PRS>";

void
jalv_print_port(Jalv* const jalv,
                const struct Port* const port,
                const float value)
{
  const LilvNode* sym = lilv_port_get_symbol(jalv->plugin, port->lilv_port);
  fprintf(stdout, "%s #%s=%f\n", CTR_PREFIX, lilv_node_as_string(sym), value);
  fflush(stdout);
}

void
jalv_print_control(Jalv* const jalv,
                   const ControlID* control,
                   const float value)
{
  static int last_control_index = -1;
  static float last_control_value = 0.0;
  // Avoid printing repeated lines (i.e. SurgeXT's bypass control)
  if (control->control_index == last_control_index && value == last_control_value) return;
  fprintf(stdout, "%s %d#%s=%f\n", CTR_PREFIX, control->control_index, lilv_node_as_string(control->symbol), value);
  fflush(stdout);
  last_control_index = control->control_index;
  last_control_value = value;
}

void
jalv_print_control_path(Jalv* const jalv,
                   const ControlID* control,
                   const char* value)
{
  fprintf(stdout, "%s %d#%s=%s\n", CTR_PREFIX, control->control_index, lilv_node_as_string(control->symbol), value);
  fflush(stdout);
}

void
jalv_print_controls(Jalv* const jalv, bool writable, bool readable)
{
  for (size_t i = 0; i < jalv->controls.n_controls; ++i) {
    ControlID* const control = jalv->controls.controls[i];
    char *prefix;
	if (control->is_writable && writable) {
	  prefix = CTR_PREFIX;
	} else if (control->is_readable && readable) {
	  prefix = MON_PREFIX;
	} else continue;
    fprintf(stdout, "%s %d#%s=%f\n", prefix, control->control_index,
            lilv_node_as_string(control->symbol),
		    jalv_get_control(jalv, control));
  }
  fflush(stdout);
}

void
jalv_print_ports(Jalv* const jalv, bool writable, bool readable)
{
  for (size_t i = 0; i < jalv->controls.n_controls; ++i) {
	ControlID* const control = jalv->controls.controls[i];
	if (control->type == PORT) {
	  char *prefix;
	  if (control->is_writable && writable) {
	    prefix = CTR_PREFIX;
	  } else if (control->is_readable && readable) {
	    prefix = MON_PREFIX;
	  } else continue;
	  struct Port* const port = &jalv->ports[control->index];
	  fprintf(stdout, "%s %d#%s=%f\n", prefix, control->control_index,
	          lilv_node_as_string(control->symbol),
			  port->control);
	}
  }
  fflush(stdout);
}

int
jalv_print_preset(Jalv* const jalv,
                  const LilvNode* node,
                  const LilvNode* title,
                  void* data)
{
	fprintf(stdout, "%s %s (%s)\n", PRS_PREFIX, lilv_node_as_string(node), lilv_node_as_string(title));
	fflush(stdout);
	return 0;
}

void
jalv_print_preset_str(char *uri, char *name) {
	fprintf(stdout, "%s %s (%s)\n", PRS_PREFIX, uri, name);
	fflush(stdout);
}

//-----------------------------------------------------------------------------

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
