// Copyright 2018-2024 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_FEATURES_H
#define JALV_FEATURES_H

#include "attributes.h"

#include <lv2/core/lv2.h>
#include <lv2/data-access/data-access.h>
#include <lv2/log/log.h>
#include <lv2/options/options.h>
#include <lv2/state/state.h>
#include <lv2/ui/ui.h>
#include <lv2/worker/worker.h>

// LV2 feature support
JALV_BEGIN_DECLS

/// LV2 features and associated data to be passed to plugins
typedef struct {
  LV2_Feature                map_feature;
  LV2_Feature                unmap_feature;
  LV2_State_Make_Path        make_path;
  LV2_Feature                make_path_feature;
  LV2_Worker_Schedule        sched;
  LV2_Feature                sched_feature;
  LV2_Worker_Schedule        ssched;
  LV2_Feature                state_sched_feature;
  LV2_Log_Log                llog;
  LV2_Feature                log_feature;
  LV2_Options_Option         options[7];
  LV2_Feature                options_feature;
  LV2_Feature                safe_restore_feature;
  LV2UI_Request_Value        request_value;
  LV2_Feature                request_value_feature;
  LV2_Extension_Data_Feature ext_data;
} JalvFeatures;

JALV_END_DECLS

#endif // JALV_FEATURES_H
