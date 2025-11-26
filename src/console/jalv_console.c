// Copyright 2007-2025 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "../any_value.h"
#include "../control.h"
#include "../frontend.h"
#include "../jalv.h"
#include "../jalv_config.h"
#include "../options.h"
#include "../state.h"
#include "../string_utils.h"
#include "../types.h"

#include <lilv/lilv.h>
#include <lv2/atom/forge.h>
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

#if USE_POLL
#  include <poll.h>
#  include <unistd.h>
#endif

#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define CONSOLE_REFRESH_RATE 15

typedef enum {
  COMMAND_SUCCESS,
  COMMAND_ERROR,
  COMMAND_QUIT,
} CommandStatus;

typedef struct {
  int status;     ///< Status code (non-zero on error)
  int n_controls; ///< Number of control values given
  int a;          ///< Argument index
} OptionsState;

static int
print_usage(const char* name, bool error)
{
  FILE* const os = error ? stderr : stdout;
  fprintf(os, "Usage: %s [OPTION...] PLUGIN_STATE\n", name);
  fprintf(os,
          "Run an LV2 plugin as a Jack application.\n"
          "PLUGIN_STATE can be a plugin/preset URI, or a path.\n\n"
          "  -b SIZE     Buffer size for plugin <=> UI communication\n"
          "  -c SYM=VAL  Set control value (like \"vol=1.4\")\n"
          "  -d          Dump plugin <=> UI communication\n"
          "  -h          Display this help and exit\n"
          "  -i          Ignore keyboard input, run non-interactively\n"
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
  printf("Copyright 2011-2025 David Robillard <d@drobilla.net>\n"
         "License ISC: <https://spdx.org/licenses/ISC>.\n"
         "This is free software; you are free to change and redistribute it."
         "\nThere is NO WARRANTY, to the extent permitted by law.\n");
  return JALV_EARLY_EXIT_STATUS;
}

static void
print_control_value(const LV2_Atom_Forge* const forge,
                    const Control* const        control,
                    FILE* const                 stream)
{
  const char* const symbol = lilv_node_as_string(control->symbol);
  const void* const data   = (const char*)any_value_data(&control->value);
  fprintf(stream, "%s = ", symbol);
  if (control->value.type == forge->URI) {
    fprintf(stream, "<%s>", (const char*)data);
  } else if (control->value.type == forge->String ||
             control->value.type == forge->Path) {
    fprintf(stream, "\"%s\"", (const char*)data);
  } else if (control->value.type == forge->Bool) {
    int32_t value = 0;
    memcpy(&value, &control->value.value.number, sizeof(int32_t));
    fprintf(stream, "%s", value ? "true" : "false");
  } else if (control->value.type == forge->Double ||
             control->value.type == forge->Float) {
    fprintf(stream, "%f", any_value_number(&control->value, forge));
  } else if (control->value.type == forge->Int) {
    int32_t value = 0;
    memcpy(&value, &control->value.value.number, sizeof(int32_t));
    fprintf(stream, "%d", value);
  } else if (control->value.type == forge->Long) {
    int64_t value = 0;
    memcpy(&value, &control->value.value.number, sizeof(int64_t));
    fprintf(stream, "%" PRId64, value);
  } else {
    fprintf(stream, "?");
  }
  fprintf(stream, "\n");
}

void
jalv_frontend_control_changed(const Jalv* const    ZIX_UNUSED(jalv),
                              const Control* const ZIX_UNUSED(control))
{}

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

static void
add_control_argument(OptionsState* const state,
                     JalvOptions* const  opts,
                     const char* const   cmd,
                     char* const         arg)
{
  char** new_controls =
    (char**)realloc(opts->controls, (state->n_controls + 2) * sizeof(char*));
  if (!new_controls) {
    fprintf(stderr, "%s: Out of memory\n", cmd);
    state->status = 12;
  } else {
    opts->controls                      = new_controls;
    opts->controls[state->n_controls++] = arg;
    opts->controls[state->n_controls]   = NULL;
  }
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
    state->status = print_usage(cmd, false);
  } else if (opt[1] == 'V' || !strcmp(opt, "--version")) {
    state->status = print_version();
  } else if (opt[1] == 's') {
    opts->show_ui = true;
  } else if (opt[1] == 'p') {
    opts->print_controls = true;
  } else if (opt[1] == 'U') {
    opts->ui_uri = jalv_strdup(parse_argument(state, argc, argv, 'U'));
  } else if (opt[1] == 'b') {
    const char* const string = parse_argument(state, argc, argv, 'b');
    if (!state->status) {
      const long value = strtol(string, NULL, 10);
      if (value >= 2 && value <= 2147483648) {
        opts->ring_size = (uint32_t)value;
      } else {
        state->status = 1;
        fprintf(stderr, "%s: option value out of range -- 'b'\n", cmd);
      }
    }
  } else if (opt[1] == 'c') {
    add_control_argument(
      state, opts, cmd, parse_argument(state, argc, argv, 'c'));
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
    state->status = print_usage(argv[0], true);
  }

  return state->status;
}

int
jalv_frontend_init(Jalv* const jalv)
{
  const int    argc = jalv->args.argc;
  char** const argv = jalv->args.argv;

  OptionsState state = {0, 0, 1};

  for (; state.a < argc && argv[state.a][0] == '-'; ++state.a) {
    const int r = parse_option(&state, &jalv->opts, argc, argv);
    if (r) {
      return r;
    }
  }

  jalv->args.argc -= state.a;
  jalv->args.argv += state.a;
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
    const Control* const control = jalv->controls.controls[i];
    if ((control->is_writable && writable) ||
        (control->is_readable && readable)) {
      print_control_value(&jalv->forge, control, stdout);
    }
  }

  fflush(stdout);
}

static int
print_preset(Jalv*           ZIX_UNUSED(jalv),
             const LilvNode* node,
             const LilvNode* title,
             void*           ZIX_UNUSED(data))
{
  printf("%s (%s)\n", lilv_node_as_string(node), lilv_node_as_string(title));
  return 0;
}

static int
is_symbol(const int c)
{
  return (c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'));
}

static int
set_control_from_string(Jalv* const                 jalv,
                        Control* const              control,
                        const char* const           string,
                        const LV2_Atom_Forge* const forge)
{
  const uint32_t type = control->value_type;

  char* endptr = NULL;

  if (type == forge->String || type == forge->Path) {
    if (string[0] != '"') {
      return 1;
    }

    // Scan until the closing '"'
    size_t            len   = 0U;
    const char* const start = &string[1];
    for (; start[len] != '"'; ++len) {
      if (start[len] == '\\') {
        if (start[++len] == '"') {
          ++len;
        } else {
          return COMMAND_ERROR;
        }
      }
    }

    // Copy just the string value to a terminated temporary and set that
    char* const value = (char*)calloc(len + 1U, 1U);
    assert(value);
    memcpy(value, start, len);
    jalv_set_control(jalv, control, len + 1U, type, value);
    free(value);
  }

  if (type == forge->Double || type == forge->Float) {
    const double value = strtod(string, &endptr);

    if (type == forge->Float) {
      const float fval = (float)value;
      return jalv_set_control(jalv, control, sizeof(fval), type, &fval);
    }

    return jalv_set_control(jalv, control, sizeof(value), type, &value);
  }

  if (type == forge->Long || type == forge->Int) {
    const long value = strtol(string, &endptr, 10);

    if (type == forge->Int) {
      const int ival = (int)value;
      return jalv_set_control(jalv, control, sizeof(ival), type, &ival);
    }

    return jalv_set_control(jalv, control, sizeof(value), type, &value);
  }

  if (type == forge->Bool) {
    bool value = false;
    if (!strncmp(string, "true", 4) && !isgraph(string[4])) {
      value = true;
    } else if (!strncmp(string, "false", 5) && !isgraph(string[5])) {
      return 1;
    }

    return jalv_set_control(jalv, control, sizeof(value), type, &value);
  }

  return 0.0;
}

static const char*
trim_command(char* const cmd)
{
  char* start = cmd;
  while (isspace(*start)) {
    ++start;
  }

  const size_t len = strlen(start);
  if (!len) {
    return start;
  }

  char* last = start + strlen(start) - 1U;
  while (last > start && isspace(*last)) {
    *last = '\0';
  }

  return start;
}

static CommandStatus
process_command(Jalv* const jalv, char* const command)
{
  CommandStatus st        = COMMAND_SUCCESS;
  char          sym[1024] = {0};

  const char* const cmd = trim_command(command);

  if (!strncmp(cmd, "help", 4)) {
    fprintf(stderr,
            "Commands:\n"
            "  help              Display this help message\n"
            "  controls          Print settable control values\n"
            "  monitors          Print output control values\n"
            "  presets           Print available presets\n"
            "  preset URI        Set preset\n"
            "  quit              Quit this program\n"
            "  set INDEX VALUE   Set control value by port index\n"
            "  set SYMBOL VALUE  Set control value by symbol\n");
  } else if (strcmp(cmd, "presets") == 0) {
    jalv_unload_presets(jalv);
    jalv_load_presets(jalv, print_preset, NULL);
  } else if (sscanf(cmd, "preset %1023[a-zA-Z0-9_:/-.#]", sym) == 1) {
    LilvNode* preset = lilv_new_uri(jalv->world, sym);
    lilv_world_load_resource(jalv->world, preset);
    jalv_apply_preset(jalv, preset);
    lilv_node_free(preset);
    print_controls(jalv, true, false);
  } else if (strcmp(cmd, "controls") == 0) {
    print_controls(jalv, true, false);
  } else if (strcmp(cmd, "monitors") == 0) {
    print_controls(jalv, false, true);
  } else if (strcmp(cmd, "quit") == 0) {
    st = COMMAND_QUIT;
  } else if (!strncmp(cmd, "set ", 4)) {
    size_t i = 4;
    while (isspace(cmd[i])) {
      ++i;
    }

    char*    endptr  = NULL;
    Control* control = NULL;
    if (isdigit(cmd[i])) { // set INDEX VALUE
      const long index = strtol(cmd + i, &endptr, 10);
      if (index >= 0 && index <= UINT32_MAX &&
          !(control = get_port_control(&jalv->controls, (uint32_t)index))) {
        fprintf(stderr, "error: no control port with index %ld\n", index);
        return COMMAND_ERROR;
      }
      i += (endptr - (cmd + i));
    } else if (is_symbol(cmd[i])) { // set SYMBOL VALUE
      const char* tok     = cmd + i;
      size_t      tok_len = 0U;
      while (isgraph(tok[tok_len]) && tok_len + 1U < sizeof(sym)) {
        ++tok_len;
        ++i;
      }

      memcpy(sym, tok, tok_len);
      if (!(control = get_named_control(&jalv->controls, sym))) {
        fprintf(stderr, "error: no control with symbol \"%s\"\n", sym);
        return COMMAND_ERROR;
      }
    } else {
      fprintf(stderr, "error: expected port index or symbol after \"set\"\n");
      return COMMAND_ERROR;
    }

    assert(control);

    while (isspace(cmd[i])) {
      ++i;
    }

    set_control_from_string(jalv, control, &cmd[i], &jalv->forge);
  }
  return st;
}

static bool
run_custom_ui(Jalv* jalv)
{
#if USE_SUIL
  const LV2UI_Idle_Interface* idle_iface = NULL;
  const LV2UI_Show_Interface* show_iface = NULL;
  if (jalv->ui && jalv->opts.show_ui) {
    jalv_instantiate_ui(jalv, jalv_frontend_ui_type(), NULL);
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
  return (float)CONSOLE_REFRESH_RATE;
}

float
jalv_frontend_scale_factor(const Jalv* ZIX_UNUSED(jalv))
{
  return 1.0f;
}

LilvNode*
jalv_frontend_select_plugin(LilvWorld* const world)
{
  (void)world;
  return NULL;
}

static void
print_prompt(FILE* const out)
{
  fprintf(out, "> ");
  fflush(out);
}

int
jalv_frontend_run(Jalv* jalv)
{
  if (jalv_open(jalv, jalv->args.argv[0])) {
    return 1;
  }

  jalv_activate(jalv);

  if (!run_custom_ui(jalv) && !jalv->opts.non_interactive) {
    print_prompt(stdout);

    CommandStatus st = (jalv_update(jalv) < 0) ? COMMAND_QUIT : COMMAND_SUCCESS;

    size_t len        = 0;
    char   line[1024] = {0};
    while (st != COMMAND_QUIT && zix_sem_try_wait(&jalv->done)) {
#if USE_POLL
      struct pollfd fds[1] = {{STDIN_FILENO, POLLIN, 0}};
      const int     rc     = poll(fds, 1, 1000 / CONSOLE_REFRESH_RATE);
      if (rc < 0) {
        st = COMMAND_QUIT;
      } else if (rc == 1) {
        const ssize_t n = read(STDIN_FILENO, line + len, 1);
        if (n == 1) {
          if (line[len] == '\n') {
            line[len] = '\0';
            st        = process_command(jalv, line);
            len       = 0;
            print_prompt(stdout);
          } else {
            ++len;
          }
        }
      }
#else
      (void)len;
      print_prompt(stdout);
      st = fgets(line, sizeof(line), stdin) ? process_command(jalv, line)
                                            : COMMAND_QUIT;
#endif

      if (st != COMMAND_QUIT) {
        jalv_update(jalv);
      }
    }
  } else {
    zix_sem_wait(&jalv->done);
  }

  return 0;
}

int
jalv_frontend_close(Jalv* ZIX_UNUSED(jalv))
{
  return 0;
}
