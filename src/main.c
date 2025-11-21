// Copyright 2007-2025 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "backend.h"
#include "frontend.h"
#include "jalv.h"
#include "jalv_config.h"
#include "types.h"

#include <zix/sem.h>

#include <signal.h>
#include <stdlib.h>

static ZixSem* exit_sem = NULL;

static void
signal_handler(const int sig)
{
  if (exit_sem && (sig == SIGINT || sig == SIGTERM)) {
    zix_sem_post(exit_sem);
  }
}

static void
setup_signals(Jalv* const jalv)
{
  exit_sem = &jalv->done;

#if !defined(_WIN32) && USE_SIGACTION
  struct sigaction action;
  sigemptyset(&action.sa_mask);
  action.sa_flags   = 0;
  action.sa_handler = signal_handler;
  sigaction(SIGINT, &action, NULL);
  sigaction(SIGTERM, &action, NULL);
#else
  // May not work in combination with fgets in the console interface
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);
#endif
}

int
main(int argc, char** argv)
{
  Jalv jalv = {.backend = jalv_backend_allocate()};
  jalv_init(&jalv, argc, argv);
  setup_signals(&jalv);

  int rc = jalv_frontend_init(&jalv);
  if (!rc) {
    rc = jalv_frontend_run(&jalv);
  }

  jalv_frontend_close(&jalv);
  jalv_backend_free(jalv.backend);
  return rc;
}
