// Copyright 2007-2024 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_JACK_IMPL_H
#define JALV_JACK_IMPL_H

#include "attributes.h"
#include "process.h"
#include "settings.h"
#include "urids.h"

#include <jack/types.h>
#include <zix/sem.h>

#include <stdbool.h>

// Definition of Jack backend structure (private to implementation)
JALV_BEGIN_DECLS

struct JalvBackendImpl {
  const JalvURIDs* urids;              ///< Application vocabulary
  JalvSettings*    settings;           ///< Run settings
  JalvProcess*     process;            ///< Process thread state
  ZixSem*          done;               ///< Shutdown semaphore
  jack_client_t*   client;             ///< Jack client
  bool             is_internal_client; ///< Running inside jackd
};

JALV_END_DECLS

#endif // JALV_JACK_IMPL_H
