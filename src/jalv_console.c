// Copyright 2007-2022 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "control.h"
#include "frontend.h"
#include "jalv_config.h"
#include "jalv_internal.h"
#include "log.h"
#include "options.h"
#include "port.h"
#include "state.h"
#include "types.h"

#include "lilv/lilv.h"
#include "lv2/ui/ui.h"
#include "zix/attributes.h"
#include "zix/sem.h"

#if USE_SUIL
#  include "suil/suil.h"
#endif

#ifdef _WIN32
#  include <synchapi.h>
#else
#  include <unistd.h>
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
print_usage(const char* name, bool error)
{
  FILE* const os = error ? stderr : stdout;
  fprintf(os, "Usage: %s [OPTION...] PLUGIN_URI\n", name);
  fprintf(os,
          "Run an LV2 plugin as a Jack application.\n"
          "  -b SIZE      Buffer size for plugin <=> UI communication\n"
          "  -c SYM=VAL   Set control value (e.g. \"vol=1.4\")\n"
          "  -d           Dump plugin <=> UI communication\n"
          "  -h           Display this help and exit\n"
          "  -i           Ignore keyboard input, run non-interactively\n"
          "  -l DIR       Load state from save directory\n"
          "  -n NAME      JACK client name\n"
          "  -p           Print control output changes to stdout\n"
          "  -s           Show plugin UI if possible\n"
          "  -t           Print trace messages from plugin\n"
          "  -U URI       Load the UI with the given URI\n"
          "  -V           Display version information and exit\n"
          "  -x           Exit if the requested JACK client name is taken.\n");
  return error ? 1 : 0;
}

static int
print_version(void)
{
  printf("jalv " JALV_VERSION " <http://drobilla.net/software/jalv>\n");
  printf("Copyright 2011-2022 David Robillard <d@drobilla.net>.\n"
         "License ISC: <https://spdx.org/licenses/ISC>.\n"
         "This is free software; you are free to change and redistribute it."
         "\nThere is NO WARRANTY, to the extent permitted by law.\n");
  return 1;
}

void
jalv_ui_port_event(Jalv*       jalv,
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
  (void)jalv;
  (void)port_index;
  (void)buffer_size;
  (void)protocol;
  (void)buffer;
#endif
}

int
jalv_frontend_init(int* argc, char*** argv, JalvOptions* opts)
{
  int n_controls = 0;
  int a          = 1;

  opts->preset_path = jalv_get_working_dir();

  for (; a < *argc && (*argv)[a][0] == '-'; ++a) {
    if ((*argv)[a][1] == 'h') {
      return print_usage((*argv)[0], true);
    }

    if ((*argv)[a][1] == 'V') {
      return print_version();
    }

    if ((*argv)[a][1] == 's') {
      opts->show_ui = true;
    } else if ((*argv)[a][1] == 'p') {
      opts->print_controls = true;
    } else if ((*argv)[a][1] == 'U') {
      if (++a == *argc) {
        fprintf(stderr, "Missing argument for -U\n");
        return 1;
      }
      opts->ui_uri = jalv_strdup((*argv)[a]);
    } else if ((*argv)[a][1] == 'l') {
      if (++a == *argc) {
        fprintf(stderr, "Missing argument for -l\n");
        return 1;
      }
      opts->load = jalv_strdup((*argv)[a]);
    } else if ((*argv)[a][1] == 'b') {
      if (++a == *argc) {
        fprintf(stderr, "Missing argument for -b\n");
        return 1;
      }
      opts->buffer_size = atoi((*argv)[a]);
    } else if ((*argv)[a][1] == 'c') {
      if (++a == *argc) {
        fprintf(stderr, "Missing argument for -c\n");
        return 1;
      }
      opts->controls =
        (char**)realloc(opts->controls, (++n_controls + 1) * sizeof(char*));
      opts->controls[n_controls - 1] = (*argv)[a];
      opts->controls[n_controls]     = NULL;
    } else if ((*argv)[a][1] == 'i') {
      opts->non_interactive = true;
    } else if ((*argv)[a][1] == 'd') {
      opts->dump = true;
    } else if ((*argv)[a][1] == 't') {
      opts->trace = true;
    } else if ((*argv)[a][1] == 'n') {
      if (++a == *argc) {
        fprintf(stderr, "Missing argument for -n\n");
        return 1;
      }
      free(opts->name);
      opts->name = jalv_strdup((*argv)[a]);
    } else if ((*argv)[a][1] == 'x') {
      opts->name_exact = 1;
    } else {
      fprintf(stderr, "Unknown option %s\n", (*argv)[a]);
      return print_usage((*argv)[0], true);
    }
  }

  return 0;
}

const char*
jalv_frontend_ui_type(void)
{
  return NULL;
}

bool
jalv_frontend_discover(Jalv* jalv)
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
      usleep(33333);
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
jalv_frontend_refresh_rate(Jalv* ZIX_UNUSED(jalv))
{
  return 30.0f;
}

float
jalv_frontend_scale_factor(Jalv* ZIX_UNUSED(jalv))
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
  if (!jalv_run_custom_ui(jalv) && !jalv->opts.non_interactive) {
    init_cli_thread(jalv);
    while (zix_sem_try_wait(&jalv->done)) {
      jalv_update(jalv);
      usleep(30000);
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

//-----------------------------------------------------------------------------

void update_ui_title(Jalv* jalv)
{
}

void update_ui_presets(Jalv* jalv)
{
}