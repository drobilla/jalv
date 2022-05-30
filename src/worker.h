// Copyright 2007-2016 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "jalv_internal.h"

#include "lilv/lilv.h"
#include "lv2/worker/worker.h"

#include <stdbool.h>
#include <stdint.h>

void
jalv_worker_init(JalvWorker*                 worker,
                 const LV2_Worker_Interface* iface,
                 bool                        threaded);

void
jalv_worker_finish(JalvWorker* worker);

void
jalv_worker_destroy(JalvWorker* worker);

LV2_Worker_Status
jalv_worker_schedule(LV2_Worker_Schedule_Handle handle,
                     uint32_t                   size,
                     const void*                data);

void
jalv_worker_emit_responses(JalvWorker* worker, LilvInstance* instance);
