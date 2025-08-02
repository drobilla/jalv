// Copyright 2007-2024 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "backend.h"
#include "jack_impl.h"
#include "jalv.h"
#include "log.h"
#include "types.h"

#include <jack/types.h>

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/// Internal Jack client initialization entry point
int
jack_initialize(jack_client_t* client, const char* load_init);

/// Internal Jack client finalization entry point
void
jack_finish(void* arg);

int
jack_initialize(jack_client_t* const client, const char* const load_init)
{
#ifndef E2BIG
#  define E2BIG 7
#endif
#ifndef ENOMEM
#  define ENOMEM 12
#endif

  const size_t args_len = strlen(load_init);
  if (args_len > JACK_LOAD_INIT_LIMIT) {
    jalv_log(JALV_LOG_ERR, "Too many arguments given\n");
    return E2BIG;
  }

  Jalv* const jalv = (Jalv*)calloc(1, sizeof(Jalv));
  if (!jalv) {
    return ENOMEM;
  }

  if (!(jalv->backend = jalv_backend_allocate())) {
    free(jalv);
    return ENOMEM;
  }

  jalv->backend->client             = client;
  jalv->backend->is_internal_client = true;

  // Build full command line with "program" name for building argv
  const size_t cmd_len = strlen("jalv ") + args_len;
  char* const  cmd     = (char*)calloc(cmd_len + 1, 1);
  memcpy(cmd, "jalv ", strlen("jalv ") + 1);
  memcpy(cmd + 5, load_init, args_len + 1);

  // Build argv
  int    argc = 0;
  char** argv = NULL;
  char*  tok  = cmd;
  int    err  = 0;
  for (size_t i = 0; !err && i <= cmd_len; ++i) {
    if (isspace(cmd[i]) || !cmd[i]) {
      char** const new_argv = (char**)realloc(argv, sizeof(char*) * ++argc);
      if (!new_argv) {
        err = ENOMEM;
        break;
      }

      argv           = new_argv;
      cmd[i]         = '\0';
      argv[argc - 1] = tok;
      tok            = cmd + i + 1;
    }
  }

  if (err || (err = jalv_open(jalv, &argc, &argv))) {
    jalv_close(jalv);
    free(jalv);
  } else {
    jalv_activate(jalv);
  }

  free(argv);
  free(cmd);
  return err;

#undef ENOMEM
#undef E2BIG
}

void
jack_finish(void* const arg)
{
  Jalv* const jalv = (Jalv*)arg;
  if (jalv) {
    jalv_deactivate(jalv);
    if (jalv_close(jalv)) {
      jalv_log(JALV_LOG_ERR, "Failed to close Jalv\n");
    }

    jalv_backend_free(jalv->backend);
    free(jalv);
  }
}
