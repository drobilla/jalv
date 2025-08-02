// Copyright 2007-2025 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "comm.h"
#include "control.h"
#include "frontend.h"
#include "jalv.h"
#include "jalv_config.h"
#include "log.h"
#include "options.h"
#include "port.h"
#include "state.h"
#include "string_utils.h"
#include "types.h"

#include <lilv/lilv.h>
#include <lv2/ui/ui.h>
#include <zix/attributes.h>
#include <zix/sem.h>

#if USE_SUIL
#  include <suil/suil.h>
#endif

#ifdef _WIN32
#  include <synchapi.h>
#else
#  include <time.h>
#endif

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  int status;     ///< Status code (non-zero on error)
  int n_controls; ///< Number of control values given
  int a;          ///< Argument index
} OptionsState;

static int
print_usage(const char* name, bool error)
{
  FILE* const os = error ? stderr : stdout;
  fprintf(os, "Usage: %s [OPTION...] PLUGIN_URI\n", name);
  fprintf(os,
          "Run an LV2 plugin as a Jack application.\n"
          "  -b SIZE     Buffer size for plugin <=> UI communication\n"
          "  -c SYM=VAL  Set control value (like \"vol=1.4\")\n"
          "  -d          Dump plugin <=> UI communication\n"
          "  -h          Display this help and exit\n"
          "  -i          Ignore keyboard input, run non-interactively\n"
          "  -l DIR      Load state from save directory\n"
          "  -n NAME     JACK client name\n"
          "  -p          Print control output changes to stdout\n"
          "  -s          Show plugin UI if possible\n"
          "  -t          Print debug trace messages\n"
          "  -U URI      Load the UI with the given URI\n"
          "  -V          Display version information and exit\n"
          "  -x          Exit if the requested JACK client name is taken\n");
  return error ? 1 : JALV_EARLY_EXIT_STATUS;
}

static int
print_version(void)
{
  printf("jalv " JALV_VERSION " <http://drobilla.net/software/jalv>\n");
  printf("Copyright 2011-2024 David Robillard <d@drobilla.net>\n"
         "License ISC: <https://spdx.org/licenses/ISC>.\n"
         "This is free software; you are free to change and redistribute it."
         "\nThere is NO WARRANTY, to the extent permitted by law.\n");
  return JALV_EARLY_EXIT_STATUS;
}

static void
print_control_port(const Jalv* const     jalv,
                   const JalvPort* const port,
                   const float           value)
{
  const LilvNode* sym = lilv_port_get_symbol(jalv->plugin, port->lilv_port);
  jalv_log(JALV_LOG_INFO, "%s = %f\n", lilv_node_as_string(sym), value);
}

void
jalv_frontend_port_event(Jalv*       jalv,
                         uint32_t    port_index,
                         uint32_t    buffer_size,
                         uint32_t    protocol,
                         const void* buffer)
{
#if USE_SUIL
  if (jalv->ui_instance) {
    suil_instance_port_event(
      jalv->ui_instance, port_index, buffer_size, protocol, buffer);
  }
#else
  (void)buffer_size;
#endif

  if (!protocol && jalv->opts.print_controls) {
    assert(buffer_size == sizeof(float));
    print_control_port(jalv, &jalv->ports[port_index], *(float*)buffer);
  }
}

static char*
parse_argument(OptionsState* const state,
               const int           argc,
               char** const        argv,
               const char          opt)
{
  if (state->a + 1 == argc) {
    fprintf(stderr, "%s: option requires an argument -- '%c'\n", argv[0], opt);
    state->status = 1;
    return argv[state->a]; // whatever, won't be used
  }

  return argv[++state->a];
}

static int
parse_option(OptionsState* const state,
             JalvOptions* const  opts,
             const int           argc,
             char** const        argv)
{
  const char* const cmd = argv[0];
  const char* const opt = argv[state->a];

  if (opt[1] == 'h' || !strcmp(opt, "--help")) {
    return print_usage(cmd, false);
  }

  if (opt[1] == 'V' || !strcmp(opt, "--version")) {
    return print_version();
  }

  if (opt[1] == 's') {
    opts->show_ui = true;
  } else if (opt[1] == 'p') {
    opts->print_controls = true;
  } else if (opt[1] == 'U') {
    opts->ui_uri = jalv_strdup(parse_argument(state, argc, argv, 'U'));
  } else if (opt[1] == 'l') {
    opts->load = jalv_strdup(parse_argument(state, argc, argv, 'l'));
  } else if (opt[1] == 'b') {
    opts->ring_size = atoi(parse_argument(state, argc, argv, 'b'));
  } else if (opt[1] == 'c') {
    char* const arg = parse_argument(state, argc, argv, 'c');

    char** new_controls =
      (char**)realloc(opts->controls, (state->n_controls + 2) * sizeof(char*));
    if (!new_controls) {
      fprintf(stderr, "Out of memory\n");
      state->status = 12;
      return state->status;
    }

    opts->controls                      = new_controls;
    opts->controls[state->n_controls++] = arg;
    opts->controls[state->n_controls]   = NULL;
  } else if (opt[1] == 'i') {
    opts->non_interactive = true;
  } else if (opt[1] == 'd') {
    opts->dump = true;
  } else if (opt[1] == 't') {
    opts->trace = true;
  } else if (opt[1] == 'n') {
    free(opts->name);
    opts->name = jalv_strdup(parse_argument(state, argc, argv, 'n'));
  } else if (opt[1] == 'x') {
    opts->name_exact = 1;
  } else {
    fprintf(stderr, "%s: unknown option -- '%c'\n", cmd, opt[1]);
    return print_usage(argv[0], true);
  }

  return state->status;
}

int
jalv_frontend_init(JalvFrontendArgs* const args, JalvOptions* const opts)
{
  const int argc = *args->argc;
  char**    argv = *args->argv;

  OptionsState state = {0, 0, 1};

  for (; state.a < argc && argv[state.a][0] == '-'; ++state.a) {
    const int r = parse_option(&state, opts, argc, argv);
    if (r) {
      return r;
    }
  }

  *args->argc = *args->argc - state.a;
  *args->argv = *args->argv + state.a;
  return state.status;
}

const char*
jalv_frontend_ui_type(void)
{
  return NULL;
}

static void
print_controls(const Jalv* const jalv, const bool writable, const bool readable)
{
  for (size_t i = 0; i < jalv->controls.n_controls; ++i) {
    Control* const control = jalv->controls.controls[i];
    if (control->type == PORT && ((control->is_writable && writable) ||
                                  (control->is_readable && readable))) {
      jalv_log(JALV_LOG_INFO,
               "%s = %f\n",
               lilv_node_as_string(control->symbol),
               jalv->process.controls_buf[control->id.index]);
    }
  }

  fflush(stdout);
}

static int
jalv_print_preset(Jalv*           ZIX_UNUSED(jalv),
                  const LilvNode* node,
                  const LilvNode* title,
                  void*           ZIX_UNUSED(data))
{
  printf("%s (%s)\n", lilv_node_as_string(node), lilv_node_as_string(title));
  return 0;
}

static void
jalv_process_command(Jalv* jalv, const char* cmd)
{
  char     sym[1024];
  uint32_t index = 0;
  float    value = 0.0f;
  if (!strncmp(cmd, "help", 4)) {
    fprintf(stderr,
            "Commands:\n"
            "  help              Display this help message\n"
            "  controls          Print settable control values\n"
            "  monitors          Print output control values\n"
            "  presets           Print available presets\n"
            "  preset URI        Set preset\n"
            "  set INDEX VALUE   Set control value by port index\n"
            "  set SYMBOL VALUE  Set control value by symbol\n"
            "  SYMBOL = VALUE    Set control value by symbol\n");
  } else if (strcmp(cmd, "presets\n") == 0) {
    jalv_unload_presets(jalv);
    jalv_load_presets(jalv, jalv_print_preset, NULL);
  } else if (sscanf(cmd, "preset %1023[a-zA-Z0-9_:/-.#]\n", sym) == 1) {
    LilvNode* preset = lilv_new_uri(jalv->world, sym);
    lilv_world_load_resource(jalv->world, preset);
    jalv_apply_preset(jalv, preset);
    lilv_node_free(preset);
    print_controls(jalv, true, false);
  } else if (strcmp(cmd, "controls\n") == 0) {
    print_controls(jalv, true, false);
  } else if (strcmp(cmd, "monitors\n") == 0) {
    print_controls(jalv, false, true);
  } else if (sscanf(cmd, "set %u %f", &index, &value) == 2) {
    if (index < jalv->num_ports) {
      jalv_write_control(jalv->process.ui_to_plugin, index, value);
      print_control_port(jalv, &jalv->ports[index], value);
    } else {
      fprintf(stderr, "error: port index out of range\n");
    }
  } else if (sscanf(cmd, "set %1023[a-zA-Z0-9_] %f", sym, &value) == 2 ||
             sscanf(cmd, "%1023[a-zA-Z0-9_] = %f", sym, &value) == 2) {
    const JalvPort* const port = jalv_port_by_symbol(jalv, sym);
    if (port) {
      jalv->process.controls_buf[port->index] = value;
      print_control_port(jalv, port, value);
    } else {
      fprintf(stderr, "error: no control named `%s'\n", sym);
    }
  } else {
    fprintf(stderr, "error: invalid command (try `help')\n");
  }
}

bool
jalv_frontend_discover(const Jalv* jalv)
{
  return jalv->opts.show_ui;
}

static bool
jalv_run_custom_ui(Jalv* jalv)
{
#if USE_SUIL
  const LV2UI_Idle_Interface* idle_iface = NULL;
  const LV2UI_Show_Interface* show_iface = NULL;
  if (jalv->ui && jalv->opts.show_ui) {
    jalv_ui_instantiate(jalv, jalv_frontend_ui_type(), NULL);
    idle_iface = (const LV2UI_Idle_Interface*)suil_instance_extension_data(
      jalv->ui_instance, LV2_UI__idleInterface);
    show_iface = (const LV2UI_Show_Interface*)suil_instance_extension_data(
      jalv->ui_instance, LV2_UI__showInterface);
  }

  if (show_iface && idle_iface) {
    show_iface->show(suil_instance_get_handle(jalv->ui_instance));

    // Drive idle interface until interrupted
    while (zix_sem_try_wait(&jalv->done)) {
      jalv_update(jalv);
      if (idle_iface->idle(suil_instance_get_handle(jalv->ui_instance))) {
        break;
      }

#  ifdef _WIN32
      Sleep(33);
#  else
      const struct timespec delay = {0, 33333000};
      nanosleep(&delay, NULL);
#  endif
    }

    show_iface->hide(suil_instance_get_handle(jalv->ui_instance));
    return true;
  }
#else
  (void)jalv;
#endif

  return false;
}

float
jalv_frontend_refresh_rate(const Jalv* ZIX_UNUSED(jalv))
{
  return 30.0f;
}

float
jalv_frontend_scale_factor(const Jalv* ZIX_UNUSED(jalv))
{
  return 1.0f;
}

LilvNode*
jalv_frontend_select_plugin(Jalv* jalv)
{
  (void)jalv;
  return NULL;
}

int
jalv_frontend_open(Jalv* jalv)
{
  // Print initial control values
  for (size_t i = 0; i < jalv->controls.n_controls; ++i) {
    Control* control = jalv->controls.controls[i];
    if (control->type == PORT && control->is_writable) {
      const JalvPort* const port = &jalv->ports[control->id.index];
      print_control_port(
        jalv, port, jalv->process.controls_buf[control->id.index]);
    }
  }

  if (!jalv_run_custom_ui(jalv) && !jalv->opts.non_interactive) {
    // Primitive command prompt for setting control values
    while (zix_sem_try_wait(&jalv->done)) {
      char line[1024];
      printf("> ");
      if (fgets(line, sizeof(line), stdin)) {
        jalv_process_command(jalv, line);
      } else {
        break;
      }
    }
  } else {
    zix_sem_wait(&jalv->done);
  }

  // Caller waits on the done sem, so increment it again to exit
  zix_sem_post(&jalv->done);

  return 0;
}

int
jalv_frontend_close(Jalv* jalv)
{
  zix_sem_post(&jalv->done);
  return 0;
}
