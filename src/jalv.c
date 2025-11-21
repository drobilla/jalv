// Copyright 2007-2025 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "jalv.h"

#include "backend.h"
#include "comm.h"
#include "control.h"
#include "dumper.h"
#include "features.h"
#include "frontend.h"
#include "jalv_config.h"
#include "log.h"
#include "macros.h"
#include "mapper.h"
#include "nodes.h"
#include "options.h"
#include "patch.h"
#include "port.h"
#include "process.h"
#include "process_setup.h"
#include "settings.h"
#include "state.h"
#include "string_utils.h"
#include "types.h"
#include "urids.h"
#include "worker.h"

#include <lilv/lilv.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/atom/util.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/core/lv2.h>
#include <lv2/data-access/data-access.h>
#include <lv2/instance-access/instance-access.h>
#include <lv2/log/log.h>
#include <lv2/patch/patch.h>
#include <lv2/state/state.h>
#include <lv2/ui/ui.h>
#include <lv2/urid/urid.h>
#include <lv2/worker/worker.h>
#include <serd/serd.h>
#include <zix/allocator.h>
#include <zix/filesystem.h>
#include <zix/ring.h>
#include <zix/sem.h>
#include <zix/status.h>

#if USE_SUIL
#  include <suil/suil.h>
#endif

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
   Size factor for UI ring buffers.

   The ring size is a few times the size of an event output to give the UI a
   chance to keep up.  Experiments with Ingen, which can highly saturate its
   event output, led me to this value.  It really ought to be enough for
   anybody(TM).
*/
#define N_BUFFER_CYCLES 16

/// These features have no data
static const LV2_Feature static_features[] = {
  {LV2_STATE__loadDefaultState, NULL},
  {LV2_BUF_SIZE__powerOf2BlockLength, NULL},
  {LV2_BUF_SIZE__fixedBlockLength, NULL},
  {LV2_BUF_SIZE__boundedBlockLength, NULL}};

/// Return true iff Jalv supports the given feature
static bool
feature_is_supported(const Jalv* jalv, const char* uri)
{
  if (!strcmp(uri, "http://lv2plug.in/ns/lv2core#isLive") ||
      !strcmp(uri, "http://lv2plug.in/ns/lv2core#inPlaceBroken")) {
    return true;
  }

  for (const LV2_Feature* const* f = jalv->feature_list; *f; ++f) {
    if (!strcmp(uri, (*f)->URI)) {
      return true;
    }
  }
  return false;
}

/**
   Create a port structure from data description.

   This is called before plugin and Jack instantiation.  The remaining
   instance-specific setup (e.g. buffers) is done later in activate_port().
*/
static int
create_port(Jalv* jalv, uint32_t port_index)
{
  JalvPort* const port = &jalv->ports[port_index];

  port->lilv_port = lilv_plugin_get_port_by_index(jalv->plugin, port_index);
  port->index     = port_index;
  port->flow      = FLOW_UNKNOWN;

  JalvProcessPort* const pport = &jalv->process.ports[port_index];
  if (jalv_process_port_init(&jalv->process.ports[port_index],
                             &jalv->nodes,
                             jalv->plugin,
                             port->lilv_port)) {
    return 1;
  }

  port->type = pport->type;
  port->flow = pport->flow;

  if (lilv_port_is_a(
        jalv->plugin, port->lilv_port, jalv->nodes.lv2_ControlPort)) {
    const bool hidden = !jalv->opts.show_hidden &&
                        lilv_port_has_property(jalv->plugin,
                                               port->lilv_port,
                                               jalv->nodes.pprops_notOnGUI);

    if (!hidden) {
      add_control(&jalv->controls,
                  new_port_control(jalv->plugin,
                                   port->lilv_port,
                                   port->index,
                                   jalv->settings.sample_rate,
                                   &jalv->nodes,
                                   &jalv->forge));
    }
  }

  // Store index if this is the designated control input port
  if (jalv->process.control_in == UINT32_MAX && pport->is_primary &&
      port->flow == FLOW_INPUT && port->type == TYPE_EVENT) {
    jalv->process.control_in = port_index;
  }

  // Update maximum buffer sizes
  const uint32_t buf_size = pport->buf_size;
  jalv->opts.ring_size = MAX(jalv->opts.ring_size, buf_size * N_BUFFER_CYCLES);
  if (port->flow == FLOW_INPUT) {
    jalv->process.process_msg_size =
      MAX(jalv->process.process_msg_size, buf_size);
  } else if (port->flow == FLOW_OUTPUT) {
    jalv->ui_msg_size = MAX(jalv->ui_msg_size, buf_size);
  }

  return 0;
}

/// Create port structures from data (via create_port()) for all ports
static int
jalv_create_ports(Jalv* jalv)
{
  const uint32_t n_ports = lilv_plugin_get_num_ports(jalv->plugin);

  jalv->num_ports         = n_ports;
  jalv->ports             = (JalvPort*)calloc(n_ports, sizeof(JalvPort));
  jalv->process.num_ports = n_ports;
  jalv->process.ports =
    (JalvProcessPort*)calloc(n_ports, sizeof(JalvProcessPort));

  // Allocate control port buffers array and set to default values
  jalv->process.controls_buf = (float*)calloc(n_ports, sizeof(float));
  lilv_plugin_get_port_ranges_float(
    jalv->plugin, NULL, NULL, jalv->process.controls_buf);

  for (uint32_t i = 0; i < jalv->num_ports; ++i) {
    if (create_port(jalv, i)) {
      return 1;
    }
  }

  return 0;
}

/**
   Get a port structure by symbol.

   TODO: Build an index to make this faster, currently O(n) which may be
   a problem when restoring the state of plugins with many ports.
*/
JalvPort*
jalv_port_by_symbol(Jalv* jalv, const char* sym)
{
  for (uint32_t i = 0; i < jalv->num_ports; ++i) {
    JalvPort* const port = &jalv->ports[i];
    const LilvNode* port_sym =
      lilv_port_get_symbol(jalv->plugin, port->lilv_port);

    if (!strcmp(lilv_node_as_string(port_sym), sym)) {
      return port;
    }
  }

  return NULL;
}

static Control*
jalv_control_by_symbol(Jalv* jalv, const char* sym)
{
  for (size_t i = 0; i < jalv->controls.n_controls; ++i) {
    if (!strcmp(lilv_node_as_string(jalv->controls.controls[i]->symbol), sym)) {
      return jalv->controls.controls[i];
    }
  }
  return NULL;
}

static void
jalv_create_controls(Jalv* jalv, bool writable)
{
  const LilvPlugin* plugin         = jalv->plugin;
  LilvWorld*        world          = jalv->world;
  LilvNode*         patch_writable = lilv_new_uri(world, LV2_PATCH__writable);
  LilvNode*         patch_readable = lilv_new_uri(world, LV2_PATCH__readable);

  LilvNodes* properties =
    lilv_world_find_nodes(world,
                          lilv_plugin_get_uri(plugin),
                          writable ? patch_writable : patch_readable,
                          NULL);
  LILV_FOREACH (nodes, p, properties) {
    const LilvNode* property = lilv_nodes_get(properties, p);
    Control*        record   = NULL;

    if (!writable &&
        lilv_world_ask(
          world, lilv_plugin_get_uri(plugin), patch_writable, property)) {
      // Find existing writable control
      for (size_t i = 0; i < jalv->controls.n_controls; ++i) {
        if (lilv_node_equals(jalv->controls.controls[i]->node, property)) {
          record              = jalv->controls.controls[i];
          record->is_readable = true;
          break;
        }
      }

      if (record) {
        continue;
      }
    }

    record = new_property_control(jalv->world,
                                  property,
                                  &jalv->nodes,
                                  jalv_mapper_urid_map(jalv->mapper),
                                  &jalv->forge);

    if (writable) {
      record->is_writable = true;
    } else {
      record->is_readable = true;
    }

    if (record->value_type) {
      add_control(&jalv->controls, record);
    } else {
      jalv_log(JALV_LOG_WARNING,
               "Parameter <%s> has unknown value type, ignored\n",
               lilv_node_as_string(record->node));
      free(record);
    }
  }
  lilv_nodes_free(properties);

  lilv_node_free(patch_readable);
  lilv_node_free(patch_writable);
}

static void
jalv_send_to_plugin(void* const       jalv_handle,
                    const uint32_t    port_index,
                    const uint32_t    buffer_size,
                    const uint32_t    protocol,
                    const void* const buffer)
{
  Jalv* const        jalv = (Jalv*)jalv_handle;
  JalvProcess* const proc = &jalv->process;
  ZixStatus          st   = ZIX_STATUS_SUCCESS;

  if (port_index >= jalv->num_ports) {
    jalv_log(JALV_LOG_ERR, "UI wrote to invalid port index %u\n", port_index);

  } else if (protocol == 0U) {
    if (buffer_size != sizeof(float)) {
      st = ZIX_STATUS_BAD_ARG;
    } else {
      const float value = *(const float*)buffer;
      st = jalv_write_control(proc->ui_to_plugin, port_index, value);
    }

  } else if (protocol == jalv->urids.atom_eventTransfer) {
    const LV2_Atom* const atom = (const LV2_Atom*)buffer;
    if (buffer_size < sizeof(LV2_Atom) ||
        (sizeof(LV2_Atom) + atom->size != buffer_size)) {
      st = ZIX_STATUS_BAD_ARG;
    } else {
      jalv_dump_atom(jalv->dumper, stdout, "UI => Plugin", atom, 36);
      st = jalv_write_event(
        proc->ui_to_plugin, port_index, atom->size, atom->type, atom + 1U);
    }

  } else {
    jalv_log(JALV_LOG_ERR,
             "UI wrote with unsupported protocol %u (%s)\n",
             protocol,
             jalv_mapper_unmap_uri(jalv->mapper, protocol));
  }

  if (st) {
    jalv_log(JALV_LOG_ERR,
             "Failed to write to plugin from UI (%s)\n",
             zix_strerror(st));
  }
}

void
jalv_set_control(Jalv*          jalv,
                 const Control* control,
                 uint32_t       size,
                 LV2_URID       type,
                 const void*    body)
{
  if (control->type == PORT && type == jalv->forge.Float) {
    const float value = *(const float*)body;
    jalv_write_control(jalv->process.ui_to_plugin, control->id.index, value);
  } else if (control->type == PROPERTY &&
             jalv->process.control_in != UINT32_MAX) {
    LV2_Atom_Forge_Frame frame;
    lv2_atom_forge_set_buffer(&jalv->forge, jalv->ui_msg, jalv->ui_msg_size);

    lv2_atom_forge_object(&jalv->forge, &frame, 0, jalv->urids.patch_Set);
    lv2_atom_forge_key(&jalv->forge, jalv->urids.patch_property);
    lv2_atom_forge_urid(&jalv->forge, control->id.property);
    lv2_atom_forge_key(&jalv->forge, jalv->urids.patch_value);
    lv2_atom_forge_atom(&jalv->forge, size, type);
    lv2_atom_forge_write(&jalv->forge, body, size);

    const LV2_Atom* atom = lv2_atom_forge_deref(&jalv->forge, frame.ref);
    jalv_send_to_plugin(jalv,
                        jalv->process.control_in,
                        lv2_atom_total_size(atom),
                        jalv->urids.atom_eventTransfer,
                        atom);
  }
}

#if USE_SUIL
static uint32_t
jalv_ui_port_index(void* const controller, const char* symbol)
{
  Jalv* const     jalv = (Jalv*)controller;
  JalvPort* const port = jalv_port_by_symbol(jalv, symbol);

  return port ? port->index : LV2UI_INVALID_PORT_INDEX;
}
#endif

void
jalv_instantiate_ui(Jalv* jalv, const char* native_ui_type, void* parent)
{
#if USE_SUIL
  const LilvInstance* const instance = jalv->process.instance;

  jalv->ui_host =
    suil_host_new(jalv_send_to_plugin, jalv_ui_port_index, NULL, NULL);

  const LV2_Feature parent_feature = {LV2_UI__parent, parent};

  const LV2_Feature instance_feature = {LV2_INSTANCE_ACCESS_URI,
                                        lilv_instance_get_handle(instance)};

  const LV2_Feature data_feature = {LV2_DATA_ACCESS_URI,
                                    &jalv->features.ext_data};

  const LV2_Feature idle_feature = {LV2_UI__idleInterface, NULL};

  const LV2_Feature* ui_features[] = {&jalv->features.map_feature,
                                      &jalv->features.unmap_feature,
                                      &instance_feature,
                                      &data_feature,
                                      &jalv->features.log_feature,
                                      &parent_feature,
                                      &jalv->features.options_feature,
                                      &idle_feature,
                                      &jalv->features.request_value_feature,
                                      NULL};

  const char* bundle_uri  = lilv_node_as_uri(lilv_ui_get_bundle_uri(jalv->ui));
  const char* binary_uri  = lilv_node_as_uri(lilv_ui_get_binary_uri(jalv->ui));
  char*       bundle_path = lilv_file_uri_parse(bundle_uri, NULL);
  char*       binary_path = lilv_file_uri_parse(binary_uri, NULL);

  jalv->ui_instance =
    suil_instance_new(jalv->ui_host,
                      jalv,
                      native_ui_type,
                      lilv_node_as_uri(lilv_plugin_get_uri(jalv->plugin)),
                      lilv_node_as_uri(lilv_ui_get_uri(jalv->ui)),
                      lilv_node_as_uri(jalv->ui_type),
                      bundle_path,
                      binary_path,
                      ui_features);

  lilv_free(binary_path);
  lilv_free(bundle_path);
#else
  (void)jalv;
  (void)native_ui_type;
  (void)parent;
#endif
}

void
jalv_refresh_ui(Jalv* jalv)
{
  // Set initial control port values
  for (uint32_t i = 0; i < MIN(jalv->num_ports, jalv->controls.n_controls);
       ++i) {
    if (jalv->ports[i].type == TYPE_CONTROL) {
      jalv_frontend_set_control(jalv,
                                jalv->controls.controls[i],
                                sizeof(float),
                                jalv->forge.Float,
                                &jalv->process.controls_buf[i]);
    }
  }

  if (jalv->process.control_in != UINT32_MAX) {
    // Send patch:Get message for initial parameters/etc
    LV2_Atom_Forge_Frame frame;
    uint64_t             buf[4U] = {0U, 0U, 0U, 0U};
    lv2_atom_forge_set_buffer(&jalv->forge, (uint8_t*)buf, sizeof(buf));
    lv2_atom_forge_object(&jalv->forge, &frame, 0, jalv->urids.patch_Get);

    const LV2_Atom* atom = lv2_atom_forge_deref(&jalv->forge, frame.ref);
    jalv_send_to_plugin(jalv,
                        jalv->process.control_in,
                        lv2_atom_total_size(atom),
                        jalv->urids.atom_eventTransfer,
                        atom);
    lv2_atom_forge_pop(&jalv->forge, &frame);
  }
}

static void
property_changed(Jalv* const           jalv,
                 const LV2_URID        key,
                 const LV2_Atom* const value)
{
  const Control* const control = get_property_control(&jalv->controls, key);
  if (control) {
    jalv_frontend_set_control(
      jalv, control, value->size, value->type, value + 1);
  }
}

static void
ui_port_event(Jalv* const       jalv,
              const uint32_t    port_index,
              const uint32_t    buffer_size,
              const uint32_t    protocol,
              const void* const buffer)
{
#if USE_SUIL
  if (jalv->ui_instance) {
    suil_instance_port_event(
      jalv->ui_instance, port_index, buffer_size, protocol, buffer);
  }
#endif

  if (protocol == 0) {
    const Control* const control =
      get_port_control(&jalv->controls, port_index);
    if (control) {
      jalv_frontend_set_control(
        jalv, control, buffer_size, jalv->forge.Float, buffer);
    }
    return;
  }

  assert(protocol == jalv->urids.atom_eventTransfer);

  const LV2_Atom* atom = (const LV2_Atom*)buffer;
  if (lv2_atom_forge_is_object_type(&jalv->forge, atom->type)) {
    const LV2_Atom_Object* obj = (const LV2_Atom_Object*)buffer;
    if (obj->body.otype == jalv->urids.patch_Set) {
      const LV2_Atom_URID* property = NULL;
      const LV2_Atom*      value    = NULL;
      if (!patch_set_get(jalv, obj, &property, &value)) {
        property_changed(jalv, property->body, value);
      }
    } else if (obj->body.otype == jalv->urids.patch_Put) {
      const LV2_Atom_Object* body = NULL;
      if (!patch_put_get(jalv, obj, &body)) {
        LV2_ATOM_OBJECT_FOREACH (body, prop) {
          property_changed(jalv, prop->key, &prop->value);
        }
      }
    } else {
      jalv_log(JALV_LOG_ERR, "Unknown object type\n");
    }
  }
}

static int
update_error(Jalv* const jalv, const char* const message)
{
  jalv_log(JALV_LOG_ERR, "%s", message);
  jalv->updating = false;
  return 1;
}

int
jalv_update(Jalv* jalv)
{
  if (!jalv->world) {
    return 0; // Not opened yet
  }

  // Check quit flag and close if set
  if (!zix_sem_try_wait(&jalv->done)) {
    jalv_frontend_close(jalv);
    return -1;
  }

  jalv->updating = true;

  // Emit UI events
  ZixRing* const    ring   = jalv->process.plugin_to_ui;
  JalvMessageHeader header = {NO_MESSAGE, 0U};
  const size_t      space  = zix_ring_read_space(ring);
  for (size_t i = 0; i < space; i += sizeof(header) + header.size) {
    // Read message header (which includes the body size)
    if (zix_ring_read(ring, &header, sizeof(header)) != sizeof(header)) {
      return update_error(jalv, "Failed to read header from process ring\n");
    }

    // Read message body
    void* const body = jalv->ui_msg;
    if (zix_ring_read(ring, body, header.size) != header.size) {
      return update_error(jalv, "Failed to read message from process ring\n");
    }

    if (header.type == CONTROL_PORT_CHANGE) {
      const JalvControlChange* const msg = (const JalvControlChange*)body;
      ui_port_event(jalv, msg->port_index, sizeof(float), 0, &msg->value);
    } else if (header.type == EVENT_TRANSFER) {
      const JalvEventTransfer* const msg = (const JalvEventTransfer*)body;
      jalv_dump_atom(jalv->dumper, stdout, "Plugin => UI", &msg->atom, 35);
      ui_port_event(jalv,
                    msg->port_index,
                    sizeof(LV2_Atom) + msg->atom.size,
                    jalv->urids.atom_eventTransfer,
                    &msg->atom);
    } else if (header.type == LATENCY_CHANGE) {
      jalv_backend_recompute_latencies(jalv->backend);
    } else {
      return update_error(jalv, "Unknown message type in process ring\n");
    }
  }

  jalv->updating = false;
  return 1;
}

static bool
jalv_apply_control_arg(Jalv* jalv, const char* s)
{
  char  sym[256] = {'\0'};
  float val      = 0.0f;
  if (sscanf(s, "%240[^=]=%f", sym, &val) != 2) {
    jalv_log(JALV_LOG_WARNING, "Ignoring invalid value `%s'\n", s);
    return false;
  }

  const Control* control = jalv_control_by_symbol(jalv, sym);
  if (!control) {
    jalv_log(
      JALV_LOG_WARNING, "Ignoring value for unknown control `%s'\n", sym);
    return false;
  }

  jalv_set_control(jalv, control, sizeof(float), jalv->urids.atom_Float, &val);
  jalv_log(JALV_LOG_INFO, "%s = %f\n", sym, val);

  return true;
}

static void
init_feature(LV2_Feature* const dest, const char* const URI, void* data)
{
  dest->URI  = URI;
  dest->data = data;
}

static const LilvUI*
jalv_select_custom_ui(const Jalv* const jalv)
{
  const char* const native_ui_type_uri = jalv_frontend_ui_type();

  if (jalv->opts.ui_uri) {
    // Specific UI explicitly requested by user
    LilvNode*     uri = lilv_new_uri(jalv->world, jalv->opts.ui_uri);
    const LilvUI* ui  = lilv_uis_get_by_uri(jalv->uis, uri);

    lilv_node_free(uri);
    return ui;
  }

#if USE_SUIL
  if (native_ui_type_uri) {
    // Try to find an embeddable UI
    LilvNode* native_type = lilv_new_uri(jalv->world, native_ui_type_uri);

    LILV_FOREACH (uis, u, jalv->uis) {
      const LilvUI*   ui   = lilv_uis_get(jalv->uis, u);
      const char*     uri  = lilv_node_as_string(lilv_ui_get_uri(ui));
      const LilvNode* type = NULL;
      const bool      supported =
        lilv_ui_is_supported(ui, suil_ui_supported, native_type, &type);

      if (supported) {
        jalv_log(JALV_LOG_INFO, "Using UI <%s>\n", uri);
        lilv_node_free(native_type);
        return ui;
      }

      jalv_log(JALV_LOG_INFO, "Ignoring incompatible UI <%s>\n", uri);
    }

    lilv_node_free(native_type);
  }
#endif

  if (!native_ui_type_uri && jalv->opts.show_ui) {
    // Try to find a UI with ui:showInterface
    LILV_FOREACH (uis, u, jalv->uis) {
      const LilvUI*   ui      = lilv_uis_get(jalv->uis, u);
      const LilvNode* ui_node = lilv_ui_get_uri(ui);

      lilv_world_load_resource(jalv->world, ui_node);

      const bool supported = lilv_world_ask(jalv->world,
                                            ui_node,
                                            jalv->nodes.lv2_extensionData,
                                            jalv->nodes.ui_showInterface);

      lilv_world_unload_resource(jalv->world, ui_node);

      if (supported) {
        return ui;
      }
    }
  }

  return NULL;
}

static void
jalv_init_features(Jalv* const jalv)
{
  // urid:map
  init_feature(&jalv->features.map_feature,
               LV2_URID__map,
               jalv_mapper_urid_map(jalv->mapper));

  // urid:unmap
  init_feature(&jalv->features.unmap_feature,
               LV2_URID__unmap,
               jalv_mapper_urid_unmap(jalv->mapper));

  // state:makePath
  jalv->features.make_path.handle = jalv;
  jalv->features.make_path.path   = jalv_make_path;
  init_feature(&jalv->features.make_path_feature,
               LV2_STATE__makePath,
               &jalv->features.make_path);

  // worker:schedule (normal)
  jalv->features.sched.schedule_work = jalv_worker_schedule;
  init_feature(
    &jalv->features.sched_feature, LV2_WORKER__schedule, &jalv->features.sched);

  // worker:schedule (state)
  jalv->features.ssched.schedule_work = jalv_worker_schedule;
  init_feature(&jalv->features.state_sched_feature,
               LV2_WORKER__schedule,
               &jalv->features.ssched);

  // log:log
  jalv->features.llog.handle  = &jalv->log;
  jalv->features.llog.printf  = jalv_printf;
  jalv->features.llog.vprintf = jalv_vprintf;
  init_feature(&jalv->features.log_feature, LV2_LOG__log, &jalv->features.llog);

  // (options:options is initialized later by jalv_init_options())

  // state:threadSafeRestore
  init_feature(
    &jalv->features.safe_restore_feature, LV2_STATE__threadSafeRestore, NULL);

  // ui:requestValue
  jalv->features.request_value.handle = jalv;
  init_feature(&jalv->features.request_value_feature,
               LV2_UI__requestValue,
               &jalv->features.request_value);
}

static void
jalv_init_ui_settings(Jalv* const jalv)
{
  const JalvOptions* const opts     = &jalv->opts;
  JalvSettings* const      settings = &jalv->settings;

  if (!settings->ring_size) {
    /* The UI ring is fed by plugin output ports (usually one), and the UI
       updates roughly once per cycle.  The ring size is a few times the size
       of the MIDI output to give the UI a chance to keep up. */
    settings->ring_size = settings->midi_buf_size * N_BUFFER_CYCLES;
  }

  if (opts->update_rate <= 0.0f) {
    // Calculate a reasonable UI update frequency
    settings->ui_update_hz = jalv_frontend_refresh_rate(jalv);
  }

  if (opts->scale_factor <= 0.0f) {
    // Calculate the monitor's scale factor
    settings->ui_scale_factor = jalv_frontend_scale_factor(jalv);
  }

  // The UI can only go so fast, clamp to reasonable limits
  settings->ui_update_hz = MAX(1.0f, MIN(60.0f, settings->ui_update_hz));
  settings->ring_size    = MAX(4096, settings->ring_size);
  jalv_log(JALV_LOG_INFO, "Comm buffers: %u bytes\n", settings->ring_size);
  jalv_log(JALV_LOG_INFO, "Update rate:  %.01f Hz\n", settings->ui_update_hz);
  jalv_log(JALV_LOG_INFO, "Scale factor: %.01f\n", settings->ui_scale_factor);
}

/// Find the initial state and set jalv->plugin
static LilvState*
open_plugin_state(Jalv* const         jalv,
                  LV2_URID_Map* const urid_map,
                  const char* const   load_arg)
{
  LilvWorld* const         world   = jalv->world;
  const LilvPlugins* const plugins = lilv_world_get_all_plugins(world);
  LilvState*               state   = NULL;

  if (!load_arg) {
    // No URI or path given, open plugin selector
    LilvNode* const plugin_uri = jalv_frontend_select_plugin(world);
    if (plugin_uri) {
      state = lilv_state_new_from_world(jalv->world, urid_map, plugin_uri);
      jalv->plugin = lilv_plugins_get_by_uri(plugins, plugin_uri);
      lilv_node_free(plugin_uri);
    }
  } else {
    // URI or path given as command-line argument
    const char* const arg = load_arg;
    if (serd_uri_string_has_scheme((const uint8_t*)arg)) {
      LilvNode* state_uri = lilv_new_uri(jalv->world, arg);
      state = lilv_state_new_from_world(jalv->world, urid_map, state_uri);
      lilv_node_free(state_uri);
    } else {
      state = lilv_state_new_from_file(jalv->world, urid_map, NULL, arg);
    }

    if (state) {
      jalv->plugin =
        lilv_plugins_get_by_uri(plugins, lilv_state_get_plugin_uri(state));
    } else {
      jalv_log(JALV_LOG_ERR, "Failed to load state \"%s\"\n", load_arg);
    }
  }

  return state;
}

static int
open_ui(Jalv* const jalv)
{
  if ((jalv->ui = jalv_select_custom_ui(jalv))) {
#if USE_SUIL
    const char* host_type_uri = jalv_frontend_ui_type();
    if (host_type_uri) {
      LilvNode* host_type = lilv_new_uri(jalv->world, host_type_uri);

      if (!lilv_ui_is_supported(
            jalv->ui, suil_ui_supported, host_type, &jalv->ui_type)) {
        jalv->ui = NULL;
      }

      lilv_node_free(host_type);
    }
#endif
  }

  if (jalv->ui) {
    jalv_log(JALV_LOG_INFO,
             "UI:           %s\n",
             lilv_node_as_uri(lilv_ui_get_uri(jalv->ui)));
  } else if (jalv->opts.ui_uri) {
    jalv_log(JALV_LOG_ERR, "Failed to find UI <%s>\n", jalv->opts.ui_uri);
    return -5;
  }

  return 0;
}

void
jalv_init(Jalv* const jalv, const int argc, char** const argv)
{
  jalv->args.argc = argc;
  jalv->args.argv = argv;

#if USE_SUIL
  suil_init(&jalv->args.argc, &jalv->args.argv, SUIL_ARG_NONE);
#endif
}

int
jalv_open(Jalv* const jalv, const char* const load_arg)
{
  JalvSettings* const settings = &jalv->settings;

  settings->block_length    = 4096U;
  settings->midi_buf_size   = 1024U;
  settings->ring_size       = jalv->opts.ring_size;
  settings->ui_update_hz    = jalv->opts.update_rate;
  settings->ui_scale_factor = jalv->opts.scale_factor;

  // Load the LV2 world
  LilvWorld* const world = lilv_world_new();
  lilv_world_set_option(world, LILV_OPTION_OBJECT_INDEX, NULL);
  lilv_world_load_all(world);

  jalv->world       = world;
  jalv->mapper      = jalv_mapper_new();
  jalv->log.urids   = &jalv->urids;
  jalv->log.tracing = jalv->opts.trace;

  // Set up atom dumping for debugging if enabled
  LV2_URID_Map* const   urid_map   = jalv_mapper_urid_map(jalv->mapper);
  LV2_URID_Unmap* const urid_unmap = jalv_mapper_urid_unmap(jalv->mapper);
  if (jalv->opts.dump) {
    jalv->dumper = jalv_dumper_new(urid_map, urid_unmap);
  }

  zix_sem_init(&jalv->work_lock, 1);
  zix_sem_init(&jalv->done, 0);
  jalv_init_urids(jalv->mapper, &jalv->urids);
  jalv_init_nodes(world, &jalv->nodes);
  jalv_init_features(jalv);
  lv2_atom_forge_init(&jalv->forge, urid_map);

  // Create temporary directory for plugin state
  jalv->temp_dir = zix_create_temporary_directory(NULL, "jalvXXXXXX");
  if (!jalv->temp_dir) {
    jalv_log(JALV_LOG_WARNING, "Failed to create temporary state directory\n");
  }

  // Find the initial state (and thereby the plugin URI)
  LilvState* state = open_plugin_state(jalv, urid_map, load_arg);
  if (!state || !jalv->plugin) {
    return -2;
  }

  jalv_log(JALV_LOG_INFO,
           "Plugin:       %s\n",
           lilv_node_as_string(lilv_plugin_get_uri(jalv->plugin)));

  // Set client name from plugin name if the user didn't specify one
  jalv->plugin_name = lilv_plugin_get_name(jalv->plugin);
  if (!jalv->opts.name) {
    jalv->opts.name = jalv_strdup(lilv_node_as_string(jalv->plugin_name));
  }

  // Check for thread-safe state restore() method
  jalv->safe_restore =
    lilv_plugin_has_feature(jalv->plugin, jalv->nodes.state_threadSafeRestore);

  // Get a plugin UI
  jalv->uis = lilv_plugin_get_uis(jalv->plugin);
  if (!jalv->opts.generic_ui) {
    if (open_ui(jalv)) {
      return -5;
    }
  }

  // Initialize process thread
  const uint32_t update_frames =
    (uint32_t)(settings->sample_rate / settings->ui_update_hz);
  jalv_process_init(&jalv->process,
                    &jalv->urids,
                    jalv->mapper,
                    update_frames,
                    jalv->opts.trace);

  // Create workers if necessary
  if (lilv_plugin_has_extension_data(jalv->plugin,
                                     jalv->nodes.work_interface)) {
    jalv->process.worker        = jalv_worker_new(&jalv->work_lock, true);
    jalv->features.sched.handle = jalv->process.worker;
    if (jalv->safe_restore) {
      jalv->process.state_worker   = jalv_worker_new(&jalv->work_lock, false);
      jalv->features.ssched.handle = jalv->process.state_worker;
    }
  }

  // Open backend (to set the sample rate, among other thigns)
  if (jalv_backend_open(jalv->backend,
                        &jalv->urids,
                        &jalv->settings,
                        &jalv->process,
                        &jalv->done,
                        jalv->opts.name,
                        jalv->opts.name_exact)) {
    jalv_log(JALV_LOG_ERR, "Failed to connect to audio system\n");
    return -6;
  }

  jalv_log(
    JALV_LOG_INFO, "Sample rate:  %u Hz\n", (uint32_t)settings->sample_rate);
  jalv_log(JALV_LOG_INFO, "Block length: %u frames\n", settings->block_length);
  jalv_log(JALV_LOG_INFO, "MIDI buffers: %zu bytes\n", settings->midi_buf_size);

  // Create port structures
  if (jalv_create_ports(jalv)) {
    return -10;
  }

  // Create input and output control structures
  jalv_create_controls(jalv, true);
  jalv_create_controls(jalv, false);

  jalv_init_ui_settings(jalv);
  jalv_init_lv2_options(&jalv->features, &jalv->urids, settings);

  // Create Plugin => UI communication buffers
  jalv->ui_msg_size = MAX(jalv->ui_msg_size, settings->midi_buf_size);
  jalv->ui_msg      = zix_aligned_alloc(NULL, 8U, jalv->ui_msg_size);

  // Build feature list for passing to plugins
  const LV2_Feature* const features[] = {&jalv->features.map_feature,
                                         &jalv->features.unmap_feature,
                                         &jalv->features.sched_feature,
                                         &jalv->features.log_feature,
                                         &jalv->features.options_feature,
                                         &static_features[0],
                                         &static_features[1],
                                         &static_features[2],
                                         &static_features[3],
                                         NULL};

  jalv->feature_list = (const LV2_Feature**)calloc(1, sizeof(features));
  if (!jalv->feature_list) {
    jalv_log(JALV_LOG_ERR, "Failed to allocate feature list\n");
    return -7;
  }
  memcpy(jalv->feature_list, features, sizeof(features));

  // Check that any required features are supported
  LilvNodes* req_feats = lilv_plugin_get_required_features(jalv->plugin);
  LILV_FOREACH (nodes, f, req_feats) {
    const char* uri = lilv_node_as_uri(lilv_nodes_get(req_feats, f));
    if (!feature_is_supported(jalv, uri)) {
      jalv_log(JALV_LOG_ERR, "Feature %s is not supported\n", uri);
      return -8;
    }
  }
  lilv_nodes_free(req_feats);

  // Instantiate the plugin
  LilvInstance* const instance = lilv_plugin_instantiate(
    jalv->plugin, settings->sample_rate, jalv->feature_list);
  if (!instance) {
    jalv_log(JALV_LOG_ERR, "Failed to instantiate plugin\n");
    return -9;
  }

  // Point things to the instance that require it

  jalv->features.ext_data.data_access =
    lilv_instance_get_descriptor(instance)->extension_data;

  const LV2_Worker_Interface* worker_iface =
    (const LV2_Worker_Interface*)lilv_instance_get_extension_data(
      instance, LV2_WORKER__interface);

  jalv_worker_attach(jalv->process.worker, worker_iface, instance->lv2_handle);
  jalv_worker_attach(
    jalv->process.state_worker, worker_iface, instance->lv2_handle);
  jalv_log(JALV_LOG_INFO, "\n");

  // Allocate port buffers
  jalv_process_activate(
    &jalv->process, &jalv->urids, instance, &jalv->settings);

  // Apply loaded state to plugin instance if necessary
  if (state) {
    jalv_apply_state(jalv, state);
    lilv_state_free(state);
  }

  // Apply initial controls from command-line arguments
  if (jalv->opts.controls) {
    for (char** c = jalv->opts.controls; *c; ++c) {
      jalv_apply_control_arg(jalv, *c);
    }
  }

  // Create Jack ports and connect plugin ports to buffers
  for (uint32_t i = 0; i < jalv->num_ports; ++i) {
    jalv_backend_activate_port(jalv->backend, &jalv->process, i);
  }

  // Discover UI
  jalv->process.has_ui = jalv_frontend_discover(jalv);
  return 0;
}

int
jalv_activate(Jalv* const jalv)
{
  jalv->process.run_state = JALV_RUNNING;

  if (jalv->backend) {
    if (jalv->process.worker) {
      jalv_worker_launch(jalv->process.worker);
    }
    lilv_instance_activate(jalv->process.instance);
    jalv_backend_activate(jalv->backend);
  }

  return 0;
}

int
jalv_deactivate(Jalv* const jalv)
{
  if (jalv->backend) {
    jalv_backend_deactivate(jalv->backend);
  }
  if (jalv->process.instance) {
    lilv_instance_deactivate(jalv->process.instance);
  }
  if (jalv->process.worker) {
    jalv_worker_exit(jalv->process.worker);
  }

  jalv->process.run_state = JALV_PAUSED;
  return 0;
}

int
jalv_close(Jalv* const jalv)
{
  // Stop audio processing, free event port buffers, and close backend
  jalv_deactivate(jalv);
  jalv_process_deactivate(&jalv->process);
  if (jalv->backend) {
    jalv_backend_close(jalv->backend);
  }

  // Free UI and plugin instances
#if USE_SUIL
  suil_instance_free(jalv->ui_instance);
#endif
  if (jalv->process.instance) {
    lilv_instance_free(jalv->process.instance);
  }

  // Clean up
  lilv_state_free(jalv->preset);
  lilv_node_free(jalv->plugin_name);
  free(jalv->ports);
  jalv_process_cleanup(&jalv->process);
  free(jalv->process.ports);
  zix_aligned_free(NULL, jalv->ui_msg);
  free(jalv->process.controls_buf);
  jalv_free_nodes(&jalv->nodes);
#if USE_SUIL
  suil_host_free(jalv->ui_host);
#endif

  for (unsigned i = 0; i < jalv->controls.n_controls; ++i) {
    free_control(jalv->controls.controls[i]);
  }
  free(jalv->controls.controls);

  jalv_dumper_free(jalv->dumper);
  lilv_uis_free(jalv->uis);
  jalv_mapper_free(jalv->mapper);
  lilv_world_free(jalv->world);

  zix_sem_destroy(&jalv->done);

  if (jalv->temp_dir) {
    // Remove temporary state directory
    const ZixStatus zst = zix_remove(jalv->temp_dir);
    if (zst) {
      jalv_log(JALV_LOG_WARNING,
               "Failed to remove temporary directory %s (%s)\n",
               jalv->temp_dir,
               zix_strerror(zst));
    }
  }

  zix_free(NULL, jalv->temp_dir);
  free(jalv->feature_list);

  free(jalv->opts.name);
  free(jalv->opts.controls);

  return 0;
}
