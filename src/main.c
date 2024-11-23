// Copyright 2007-2024 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "backend.h"
#include "frontend.h"
#include "jalv.h"
#include "jalv_config.h"
#include "types.h"

#include <zix/attributes.h>
#include <zix/sem.h>

#if USE_SUIL
#endif

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ZixSem* exit_sem = NULL; ///< Exit semaphore used by signal handler

static void
signal_handler(int ZIX_UNUSED(sig))
{
  zix_sem_post(exit_sem);
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
  Jalv jalv;
  memset(&jalv, '\0', sizeof(Jalv));
  jalv.backend = jalv_backend_allocate();

  // Initialize application
  const int orc = jalv_open(&jalv, &argc, &argv);
  if (orc) {
    jalv_close(&jalv);
    return orc == JALV_EARLY_EXIT_STATUS ? EXIT_SUCCESS : EXIT_FAILURE;
  }

  // Set up signal handlers and activate audio processing
  setup_signals(&jalv);
  jalv_activate(&jalv);

  // Run UI (or prompt at console)
  jalv_frontend_open(&jalv);

  // Wait for finish signal from UI or signal handler
  zix_sem_wait(&jalv.done);

  // Deactivate audio processing and tear down application
  jalv_deactivate(&jalv);
  const int crc = jalv_close(&jalv);
  jalv_backend_free(jalv.backend);
  return crc;
}
