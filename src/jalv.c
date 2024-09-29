// Copyright 2007-2022 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "backend.h"
#include "control.h"
#include "frontend.h"
#include "jalv_config.h"
#include "jalv_internal.h"
#include "log.h"
#include "lv2_evbuf.h"
#include "nodes.h"
#include "port.h"
#include "state.h"
#include "types.h"
#include "urids.h"
#include "worker.h"

#include "lilv/lilv.h"
#include "lv2/atom/atom.h"
#include "lv2/atom/forge.h"
#include "lv2/atom/util.h"
#include "lv2/buf-size/buf-size.h"
#include "lv2/core/lv2.h"
#include "lv2/data-access/data-access.h"
#include "lv2/instance-access/instance-access.h"
#include "lv2/log/log.h"
#include "lv2/midi/midi.h"
#include "lv2/options/options.h"
#include "lv2/parameters/parameters.h"
#include "lv2/patch/patch.h"
#include "lv2/port-groups/port-groups.h"
#include "lv2/port-props/port-props.h"
#include "lv2/presets/presets.h"
#include "lv2/resize-port/resize-port.h"
#include "lv2/state/state.h"
#include "lv2/time/time.h"
#include "lv2/ui/ui.h"
#include "lv2/urid/urid.h"
#include "lv2/worker/worker.h"
#include "serd/serd.h"
#include "sratom/sratom.h"
#include "symap.h"
#include "zix/attributes.h"
#include "zix/ring.h"
#include "zix/sem.h"

#if USE_SUIL
#  include "suil/suil.h"
#endif

#if defined(_WIN32)
#  include <io.h> // for _mktemp
#  define snprintf _snprintf
#elif defined(__APPLE__)
#  include <unistd.h> // for mkdtemp on Darwin
#endif

#include <assert.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef MIN
#  define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

#ifndef MAX
#  define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

#ifndef ARRAY_SIZE
#  define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif

#ifndef MSG_BUFFER_SIZE
#  define MSG_BUFFER_SIZE 1024
#endif

/**
   Size factor for UI ring buffers.

   The ring size is a few times the size of an event output to give the UI a
   chance to keep up.  Experiments with Ingen, which can highly saturate its
   event output, led me to this value.  It really ought to be enough for
   anybody(TM).
*/
#define N_BUFFER_CYCLES 16

static ZixSem* exit_sem = NULL; ///< Exit semaphore used by signal handler

static LV2_URID
map_uri(LV2_URID_Map_Handle handle, const char* uri)
{
  Jalv* jalv = (Jalv*)handle;
  zix_sem_wait(&jalv->symap_lock);
  const LV2_URID id = symap_map(jalv->symap, uri);
  zix_sem_post(&jalv->symap_lock);
  return id;
}

static const char*
unmap_uri(LV2_URID_Unmap_Handle handle, LV2_URID urid)
{
  Jalv* jalv = (Jalv*)handle;
  zix_sem_wait(&jalv->symap_lock);
  const char* uri = symap_unmap(jalv->symap, urid);
  zix_sem_post(&jalv->symap_lock);
  return uri;
}

/// These features have no data
static const LV2_Feature static_features[] = {
  {LV2_STATE__loadDefaultState, NULL},
  {LV2_BUF_SIZE__powerOf2BlockLength, NULL},
  {LV2_BUF_SIZE__fixedBlockLength, NULL},
  {LV2_BUF_SIZE__boundedBlockLength, NULL}};

/// Return true iff Jalv supports the given feature
static bool
feature_is_supported(Jalv* jalv, const char* uri)
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

/// Abort and exit on error
static void
die(const char* msg)
{
  jalv_log(JALV_LOG_ERR, "%s\n", msg);
  exit(EXIT_FAILURE);
}

/**
   Create a port structure from data description.

   This is called before plugin and Jack instantiation.  The remaining
   instance-specific setup (e.g. buffers) is done later in activate_port().
*/
static void
create_port(Jalv* jalv, uint32_t port_index, float default_value)
{
  struct Port* const port = &jalv->ports[port_index];

  port->lilv_port = lilv_plugin_get_port_by_index(jalv->plugin, port_index);
  port->sys_port  = NULL;
  port->evbuf     = NULL;
  port->buf_size  = 0;
  port->index     = port_index;
  port->control   = 0.0f;
  port->flow      = FLOW_UNKNOWN;

  const bool optional = lilv_port_has_property(
    jalv->plugin, port->lilv_port, jalv->nodes.lv2_connectionOptional);

  // Set the port flow (input or output)
  if (lilv_port_is_a(
        jalv->plugin, port->lilv_port, jalv->nodes.lv2_InputPort)) {
    port->flow = FLOW_INPUT;
  } else if (lilv_port_is_a(
               jalv->plugin, port->lilv_port, jalv->nodes.lv2_OutputPort)) {
    port->flow = FLOW_OUTPUT;
  } else if (!optional) {
    die("Mandatory port has unknown type (neither input nor output)");
  }

  const bool hidden = !jalv->opts.show_hidden &&
                      lilv_port_has_property(jalv->plugin,
                                             port->lilv_port,
                                             jalv->nodes.pprops_notOnGUI);

  // Set control values
  if (lilv_port_is_a(
        jalv->plugin, port->lilv_port, jalv->nodes.lv2_ControlPort)) {
    port->type    = TYPE_CONTROL;
    port->control = isnan(default_value) ? 0.0f : default_value;
    if (!hidden) {
      add_control(&jalv->controls,
                  new_port_control(jalv->world,
                                   jalv->plugin,
                                   port->lilv_port,
                                   port->index,
                                   jalv->sample_rate,
                                   &jalv->nodes,
                                   &jalv->forge));
    }
  } else if (lilv_port_is_a(
               jalv->plugin, port->lilv_port, jalv->nodes.lv2_AudioPort)) {
    port->type = TYPE_AUDIO;
#if USE_JACK_METADATA
  } else if (lilv_port_is_a(
               jalv->plugin, port->lilv_port, jalv->nodes.lv2_CVPort)) {
    port->type = TYPE_CV;
#endif
  } else if (lilv_port_is_a(
               jalv->plugin, port->lilv_port, jalv->nodes.atom_AtomPort)) {
    port->type = TYPE_EVENT;
  } else if (!optional) {
    die("Mandatory port has unknown data type");
  }

  LilvNode* min_size =
    lilv_port_get(jalv->plugin, port->lilv_port, jalv->nodes.rsz_minimumSize);
  if (min_size && lilv_node_is_int(min_size)) {
    port->buf_size = lilv_node_as_int(min_size);
    jalv->opts.buffer_size =
      MAX(jalv->opts.buffer_size, port->buf_size * N_BUFFER_CYCLES);
  }
  lilv_node_free(min_size);
}

/// Create port structures from data (via create_port()) for all ports
void
jalv_create_ports(Jalv* jalv)
{
  jalv->num_ports = lilv_plugin_get_num_ports(jalv->plugin);
  jalv->ports     = (struct Port*)calloc(jalv->num_ports, sizeof(struct Port));
  float* default_values =
    (float*)calloc(lilv_plugin_get_num_ports(jalv->plugin), sizeof(float));
  lilv_plugin_get_port_ranges_float(jalv->plugin, NULL, NULL, default_values);

  for (uint32_t i = 0; i < jalv->num_ports; ++i) {
    create_port(jalv, i, default_values[i]);
  }

  const LilvPort* control_input = lilv_plugin_get_port_by_designation(
    jalv->plugin, jalv->nodes.lv2_InputPort, jalv->nodes.lv2_control);
  if (control_input) {
    const uint32_t index = lilv_port_get_index(jalv->plugin, control_input);
    if (jalv->ports[index].type == TYPE_EVENT) {
      jalv->control_in = index;
    } else {
      jalv_log(JALV_LOG_WARNING,
               "Non-event port %u has lv2:control designation, ignored\n",
               index);
    }
  }

  free(default_values);
}

/// Allocate port buffers (only necessary for MIDI)
void
jalv_allocate_port_buffers(Jalv* jalv)
{
  const LV2_URID atom_Chunk = jalv->map.map(
    jalv->map.handle, lilv_node_as_string(jalv->nodes.atom_Chunk));

  const LV2_URID atom_Sequence = jalv->map.map(
    jalv->map.handle, lilv_node_as_string(jalv->nodes.atom_Sequence));

  for (uint32_t i = 0; i < jalv->num_ports; ++i) {
    struct Port* const port = &jalv->ports[i];
    if (port->type == TYPE_EVENT) {
      lv2_evbuf_free(port->evbuf);

      const size_t size = port->buf_size ? port->buf_size : jalv->midi_buf_size;

      port->evbuf = lv2_evbuf_new(size, atom_Chunk, atom_Sequence);

      lilv_instance_connect_port(
        jalv->instance, i, lv2_evbuf_get_buffer(port->evbuf));

      lv2_evbuf_reset(port->evbuf, port->flow == FLOW_INPUT);
    }
  }
}

/**
   Get a port structure by symbol.

   TODO: Build an index to make this faster, currently O(n) which may be
   a problem when restoring the state of plugins with many ports.
*/
struct Port*
jalv_port_by_symbol(Jalv* jalv, const char* sym)
{
  for (uint32_t i = 0; i < jalv->num_ports; ++i) {
    struct Port* const port = &jalv->ports[i];
    const LilvNode*    port_sym =
      lilv_port_get_symbol(jalv->plugin, port->lilv_port);

    if (!strcmp(lilv_node_as_string(port_sym), sym)) {
      return port;
    }
  }

  return NULL;
}

ControlID*
jalv_control_by_symbol(Jalv* jalv, const char* sym)
{
  for (size_t i = 0; i < jalv->controls.n_controls; ++i) {
    if (!strcmp(lilv_node_as_string(jalv->controls.controls[i]->symbol), sym)) {
      return jalv->controls.controls[i];
    }
  }
  return NULL;
}

void
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
    ControlID*      record   = NULL;

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

    record = new_property_control(
      jalv->world, property, &jalv->nodes, &jalv->map, &jalv->forge);

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

void
jalv_set_control(Jalv*            jalv,
                 const ControlID* control,
                 uint32_t         size,
                 LV2_URID         type,
                 const void*      body)
{
  if (control->type == PORT && type == jalv->forge.Float) {
    struct Port* port = &jalv->ports[control->index];
    port->control     = *(const float*)body;
  } else if (control->type == PROPERTY) {
    // Copy forge since it is used by process thread
    LV2_Atom_Forge       forge = jalv->forge;
    LV2_Atom_Forge_Frame frame;
    uint8_t              buf[MSG_BUFFER_SIZE];
    lv2_atom_forge_set_buffer(&forge, buf, sizeof(buf));

    lv2_atom_forge_object(&forge, &frame, 0, jalv->urids.patch_Set);
    lv2_atom_forge_key(&forge, jalv->urids.patch_property);
    lv2_atom_forge_urid(&forge, control->property);
    lv2_atom_forge_key(&forge, jalv->urids.patch_value);
    lv2_atom_forge_atom(&forge, size, type);
    lv2_atom_forge_write(&forge, body, size);

    const LV2_Atom* atom = lv2_atom_forge_deref(&forge, frame.ref);
    jalv_send_to_plugin(jalv,
                        jalv->control_in,
                        lv2_atom_total_size(atom),
                        jalv->urids.atom_eventTransfer,
                        atom);
  }
}

#if USE_SUIL
static uint32_t
jalv_ui_port_index(void* const controller, const char* symbol)
{
  Jalv* const  jalv = (Jalv*)controller;
  struct Port* port = jalv_port_by_symbol(jalv, symbol);

  return port ? port->index : LV2UI_INVALID_PORT_INDEX;
}
#endif

void
jalv_ui_instantiate(Jalv* jalv, const char* native_ui_type, void* parent)
{
#if USE_SUIL
  jalv->ui_host =
    suil_host_new(jalv_send_to_plugin, jalv_ui_port_index, NULL, NULL);

  const LV2_Feature parent_feature = {LV2_UI__parent, parent};

  const LV2_Feature instance_feature = {
    LV2_INSTANCE_ACCESS_URI, lilv_instance_get_handle(jalv->instance)};

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

bool
jalv_ui_is_resizable(Jalv* jalv)
{
  if (!jalv->ui) {
    return false;
  }

  const LilvNode* s   = lilv_ui_get_uri(jalv->ui);
  LilvNode*       p   = lilv_new_uri(jalv->world, LV2_CORE__optionalFeature);
  LilvNode*       fs  = lilv_new_uri(jalv->world, LV2_UI__fixedSize);
  LilvNode*       nrs = lilv_new_uri(jalv->world, LV2_UI__noUserResize);

  LilvNodes* fs_matches  = lilv_world_find_nodes(jalv->world, s, p, fs);
  LilvNodes* nrs_matches = lilv_world_find_nodes(jalv->world, s, p, nrs);

  lilv_nodes_free(nrs_matches);
  lilv_nodes_free(fs_matches);
  lilv_node_free(nrs);
  lilv_node_free(fs);
  lilv_node_free(p);

  return !fs_matches && !nrs_matches;
}

static void
jalv_send_control_to_plugin(Jalv* const jalv,
                            uint32_t    port_index,
                            uint32_t    buffer_size,
                            const void* buffer)
{
  if (buffer_size != sizeof(float)) {
    jalv_log(JALV_LOG_ERR, "UI wrote invalid control size %u\n", buffer_size);

  } else {
    jalv_write_control(
      jalv, jalv->ui_to_plugin, port_index, *(const float*)buffer);
  }
}

static void
jalv_send_event_to_plugin(Jalv* const jalv,
                          uint32_t    port_index,
                          uint32_t    buffer_size,
                          const void* buffer)
{
  const LV2_Atom* const atom = (const LV2_Atom*)buffer;

  if (buffer_size < sizeof(LV2_Atom)) {
    jalv_log(JALV_LOG_ERR, "UI wrote impossible atom size\n");

  } else if (sizeof(LV2_Atom) + atom->size != buffer_size) {
    jalv_log(JALV_LOG_ERR, "UI wrote corrupt atom size\n");

  } else {
    jalv_dump_atom(jalv, stdout, "UI => Plugin", atom, 36);
    jalv_write_event(
      jalv, jalv->ui_to_plugin, port_index, atom->size, atom->type, atom + 1U);
  }
}

void
jalv_send_to_plugin(void* const jalv_handle,
                    uint32_t    port_index,
                    uint32_t    buffer_size,
                    uint32_t    protocol,
                    const void* buffer)
{
  Jalv* const jalv = (Jalv*)jalv_handle;

  if (port_index >= jalv->num_ports) {
    jalv_log(JALV_LOG_ERR, "UI wrote to invalid port index %u\n", port_index);

  } else if (protocol == 0U) {
    jalv_send_control_to_plugin(jalv, port_index, buffer_size, buffer);

  } else if (protocol == jalv->urids.atom_eventTransfer) {
    jalv_send_event_to_plugin(jalv, port_index, buffer_size, buffer);

  } else {
    jalv_log(JALV_LOG_ERR,
             "UI wrote with unsupported protocol %u (%s)\n",
             protocol,
             unmap_uri(jalv, protocol));
  }
}

void
jalv_apply_ui_events(Jalv* jalv, uint32_t nframes)
{
  if (!jalv->has_ui) {
    return;
  }

  ControlChange ev    = {0U, 0U, 0U};
  const size_t  space = zix_ring_read_space(jalv->ui_to_plugin);
  for (size_t i = 0; i < space; i += sizeof(ev) + ev.size) {
    if (zix_ring_read(jalv->ui_to_plugin, &ev, sizeof(ev)) != sizeof(ev)) {
      jalv_log(JALV_LOG_ERR, "Failed to read header from UI ring buffer\n");
      break;
    }

    struct {
      union {
        LV2_Atom atom;
        float    control;
      } head;
      uint8_t body[MSG_BUFFER_SIZE];
    } buffer;

    if (zix_ring_read(jalv->ui_to_plugin, &buffer, ev.size) != ev.size) {
      jalv_log(JALV_LOG_ERR, "Failed to read from UI ring buffer\n");
      break;
    }

    assert(ev.index < jalv->num_ports);
    struct Port* const port = &jalv->ports[ev.index];
    if (ev.protocol == 0) {
      assert(ev.size == sizeof(float));
      port->control = buffer.head.control;
    } else if (ev.protocol == jalv->urids.atom_eventTransfer) {
      LV2_Evbuf_Iterator    e    = lv2_evbuf_end(port->evbuf);
      const LV2_Atom* const atom = &buffer.head.atom;
      lv2_evbuf_write(
        &e, nframes, 0, atom->type, atom->size, LV2_ATOM_BODY_CONST(atom));
    } else {
      jalv_log(
        JALV_LOG_ERR, "Unknown control change protocol %u\n", ev.protocol);
    }
  }
}

void
jalv_init_ui(Jalv* jalv)
{
  // Set initial control port values
  for (uint32_t i = 0; i < jalv->num_ports; ++i) {
    if (jalv->ports[i].type == TYPE_CONTROL) {
      jalv_ui_port_event(jalv, i, sizeof(float), 0, &jalv->ports[i].control);
    }
  }

  if (jalv->control_in != (uint32_t)-1) {
    // Send patch:Get message for initial parameters/etc
    LV2_Atom_Forge       forge = jalv->forge;
    LV2_Atom_Forge_Frame frame;
    uint8_t              buf[MSG_BUFFER_SIZE];
    lv2_atom_forge_set_buffer(&forge, buf, sizeof(buf));
    lv2_atom_forge_object(&forge, &frame, 0, jalv->urids.patch_Get);

    const LV2_Atom* atom = lv2_atom_forge_deref(&forge, frame.ref);
    jalv_send_to_plugin(jalv,
                        jalv->control_in,
                        lv2_atom_total_size(atom),
                        jalv->urids.atom_eventTransfer,
                        atom);
    lv2_atom_forge_pop(&forge, &frame);
  }
}

static int
jalv_write_control_change(Jalv* const       jalv,
                          ZixRing* const    target,
                          const void* const header,
                          const uint32_t    header_size,
                          const void* const body,
                          const uint32_t    body_size)
{
  ZixRingTransaction tx = zix_ring_begin_write(target);
  if (zix_ring_amend_write(target, &tx, header, header_size) ||
      zix_ring_amend_write(target, &tx, body, body_size)) {
    jalv_log(JALV_LOG_ERR,
             target == jalv->plugin_to_ui ? "Plugin => UI buffer overflow"
                                          : "UI => Plugin buffer overflow");
    return -1;
  }

  zix_ring_commit_write(target, &tx);
  return 0;
}

int
jalv_write_event(Jalv* const       jalv,
                 ZixRing* const    target,
                 const uint32_t    port_index,
                 const uint32_t    size,
                 const LV2_URID    type,
                 const void* const body)
{
  // TODO: Be more discriminate about what to send

  typedef struct {
    ControlChange change;
    LV2_Atom      atom;
  } Header;

  const Header header = {
    {port_index, jalv->urids.atom_eventTransfer, sizeof(LV2_Atom) + size},
    {size, type}};

  return jalv_write_control_change(
    jalv, target, &header, sizeof(header), body, size);
}

int
jalv_write_control(Jalv* const    jalv,
                   ZixRing* const target,
                   const uint32_t port_index,
                   const float    value)
{
  const ControlChange header = {port_index, 0, sizeof(value)};

  return jalv_write_control_change(
    jalv, target, &header, sizeof(header), &value, sizeof(value));
}

void
jalv_dump_atom(Jalv* const           jalv,
               FILE* const           stream,
               const char* const     label,
               const LV2_Atom* const atom,
               const int             color)
{
  if (jalv->opts.dump) {
    char* const str = sratom_to_turtle(jalv->sratom,
                                       &jalv->unmap,
                                       "jalv:",
                                       NULL,
                                       NULL,
                                       atom->type,
                                       atom->size,
                                       LV2_ATOM_BODY_CONST(atom));

    jalv_ansi_start(stream, color);
    fprintf(stream, "\n# %s (%u bytes):\n%s\n", label, atom->size, str);
    jalv_ansi_reset(stream);
    free(str);
  }
}

bool
jalv_run(Jalv* jalv, uint32_t nframes)
{
  // Read and apply control change events from UI
  jalv_apply_ui_events(jalv, nframes);

  // Run plugin for this cycle
  lilv_instance_run(jalv->instance, nframes);

  // Process any worker replies and end the cycle
  LV2_Handle handle = lilv_instance_get_handle(jalv->instance);
  jalv_worker_emit_responses(jalv->state_worker, handle);
  jalv_worker_emit_responses(jalv->worker, handle);
  jalv_worker_end_run(jalv->worker);

  // Check if it's time to send updates to the UI
  jalv->event_delta_t += nframes;
  bool     send_ui_updates = false;
  uint32_t update_frames   = (uint32_t)(jalv->sample_rate / jalv->ui_update_hz);
  if (jalv->has_ui && (jalv->event_delta_t > update_frames)) {
    send_ui_updates     = true;
    jalv->event_delta_t = 0;
  }

  return send_ui_updates;
}

int
jalv_update(Jalv* jalv)
{
  // Check quit flag and close if set
  if (!zix_sem_try_wait(&jalv->done)) {
    jalv_frontend_close(jalv);
    return 0;
  }

  // Emit UI events
  ControlChange ev;
  const size_t  space = zix_ring_read_space(jalv->plugin_to_ui);
  for (size_t i = 0; i + sizeof(ev) < space; i += sizeof(ev) + ev.size) {
    // Read event header to get the size
    zix_ring_read(jalv->plugin_to_ui, &ev, sizeof(ev));

    // Resize read buffer if necessary
    void* const buf = realloc(jalv->ui_event_buf, ev.size);
    if (!buf) {
      return 12;
    }

    jalv->ui_event_buf = buf;

    // Read event body
    zix_ring_read(jalv->plugin_to_ui, buf, ev.size);

    if (ev.protocol == jalv->urids.atom_eventTransfer) {
      jalv_dump_atom(jalv, stdout, "Plugin => UI", (const LV2_Atom*)buf, 35);
    }

    jalv_ui_port_event(jalv, ev.index, ev.size, ev.protocol, buf);

    if (ev.protocol == 0 && jalv->opts.print_controls) {
      jalv_print_control(jalv, &jalv->ports[ev.index], *(float*)buf);
    }
  }

  return 1;
}

static bool
jalv_apply_control_arg(Jalv* jalv, const char* s)
{
  char  sym[256];
  float val = 0.0f;
  if (sscanf(s, "%[^=]=%f", sym, &val) != 2) {
    jalv_log(JALV_LOG_WARNING, "Ignoring invalid value `%s'\n", s);
    return false;
  }

  ControlID* control = jalv_control_by_symbol(jalv, sym);
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
signal_handler(int ZIX_UNUSED(sig))
{
  zix_sem_post(exit_sem);
}

static void
init_feature(LV2_Feature* const dest, const char* const URI, void* data)
{
  dest->URI  = URI;
  dest->data = data;
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
      const LilvNode* type = NULL;
      const bool      supported =
        lilv_ui_is_supported(ui, suil_ui_supported, native_type, &type);

      if (supported) {
        lilv_node_free(native_type);
        return ui;
      }
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
jalv_init_urids(Symap* const symap, JalvURIDs* const urids)
{
#define MAP_URI(uri) symap_map(symap, (uri))

  urids->atom_Float           = MAP_URI(LV2_ATOM__Float);
  urids->atom_Int             = MAP_URI(LV2_ATOM__Int);
  urids->atom_Object          = MAP_URI(LV2_ATOM__Object);
  urids->atom_Path            = MAP_URI(LV2_ATOM__Path);
  urids->atom_String          = MAP_URI(LV2_ATOM__String);
  urids->atom_eventTransfer   = MAP_URI(LV2_ATOM__eventTransfer);
  urids->bufsz_maxBlockLength = MAP_URI(LV2_BUF_SIZE__maxBlockLength);
  urids->bufsz_minBlockLength = MAP_URI(LV2_BUF_SIZE__minBlockLength);
  urids->bufsz_sequenceSize   = MAP_URI(LV2_BUF_SIZE__sequenceSize);
  urids->log_Error            = MAP_URI(LV2_LOG__Error);
  urids->log_Trace            = MAP_URI(LV2_LOG__Trace);
  urids->log_Warning          = MAP_URI(LV2_LOG__Warning);
  urids->midi_MidiEvent       = MAP_URI(LV2_MIDI__MidiEvent);
  urids->param_sampleRate     = MAP_URI(LV2_PARAMETERS__sampleRate);
  urids->patch_Get            = MAP_URI(LV2_PATCH__Get);
  urids->patch_Put            = MAP_URI(LV2_PATCH__Put);
  urids->patch_Set            = MAP_URI(LV2_PATCH__Set);
  urids->patch_body           = MAP_URI(LV2_PATCH__body);
  urids->patch_property       = MAP_URI(LV2_PATCH__property);
  urids->patch_value          = MAP_URI(LV2_PATCH__value);
  urids->time_Position        = MAP_URI(LV2_TIME__Position);
  urids->time_bar             = MAP_URI(LV2_TIME__bar);
  urids->time_barBeat         = MAP_URI(LV2_TIME__barBeat);
  urids->time_beatUnit        = MAP_URI(LV2_TIME__beatUnit);
  urids->time_beatsPerBar     = MAP_URI(LV2_TIME__beatsPerBar);
  urids->time_beatsPerMinute  = MAP_URI(LV2_TIME__beatsPerMinute);
  urids->time_frame           = MAP_URI(LV2_TIME__frame);
  urids->time_speed           = MAP_URI(LV2_TIME__speed);
  urids->ui_scaleFactor       = MAP_URI(LV2_UI__scaleFactor);
  urids->ui_updateRate        = MAP_URI(LV2_UI__updateRate);

#undef MAP_URI
}

static void
jalv_init_nodes(LilvWorld* const world, JalvNodes* const nodes)
{
#define MAP_NODE(uri) lilv_new_uri(world, (uri))

  nodes->atom_AtomPort          = MAP_NODE(LV2_ATOM__AtomPort);
  nodes->atom_Chunk             = MAP_NODE(LV2_ATOM__Chunk);
  nodes->atom_Float             = MAP_NODE(LV2_ATOM__Float);
  nodes->atom_Path              = MAP_NODE(LV2_ATOM__Path);
  nodes->atom_Sequence          = MAP_NODE(LV2_ATOM__Sequence);
  nodes->lv2_AudioPort          = MAP_NODE(LV2_CORE__AudioPort);
  nodes->lv2_CVPort             = MAP_NODE(LV2_CORE__CVPort);
  nodes->lv2_ControlPort        = MAP_NODE(LV2_CORE__ControlPort);
  nodes->lv2_InputPort          = MAP_NODE(LV2_CORE__InputPort);
  nodes->lv2_OutputPort         = MAP_NODE(LV2_CORE__OutputPort);
  nodes->lv2_connectionOptional = MAP_NODE(LV2_CORE__connectionOptional);
  nodes->lv2_control            = MAP_NODE(LV2_CORE__control);
  nodes->lv2_default            = MAP_NODE(LV2_CORE__default);
  nodes->lv2_enumeration        = MAP_NODE(LV2_CORE__enumeration);
  nodes->lv2_extensionData      = MAP_NODE(LV2_CORE__extensionData);
  nodes->lv2_integer            = MAP_NODE(LV2_CORE__integer);
  nodes->lv2_maximum            = MAP_NODE(LV2_CORE__maximum);
  nodes->lv2_minimum            = MAP_NODE(LV2_CORE__minimum);
  nodes->lv2_name               = MAP_NODE(LV2_CORE__name);
  nodes->lv2_reportsLatency     = MAP_NODE(LV2_CORE__reportsLatency);
  nodes->lv2_sampleRate         = MAP_NODE(LV2_CORE__sampleRate);
  nodes->lv2_symbol             = MAP_NODE(LV2_CORE__symbol);
  nodes->lv2_toggled            = MAP_NODE(LV2_CORE__toggled);
  nodes->midi_MidiEvent         = MAP_NODE(LV2_MIDI__MidiEvent);
  nodes->pg_group               = MAP_NODE(LV2_PORT_GROUPS__group);
  nodes->pprops_logarithmic     = MAP_NODE(LV2_PORT_PROPS__logarithmic);
  nodes->pprops_notOnGUI        = MAP_NODE(LV2_PORT_PROPS__notOnGUI);
  nodes->pprops_rangeSteps      = MAP_NODE(LV2_PORT_PROPS__rangeSteps);
  nodes->pset_Preset            = MAP_NODE(LV2_PRESETS__Preset);
  nodes->pset_bank              = MAP_NODE(LV2_PRESETS__bank);
  nodes->rdfs_comment           = MAP_NODE(LILV_NS_RDFS "comment");
  nodes->rdfs_label             = MAP_NODE(LILV_NS_RDFS "label");
  nodes->rdfs_range             = MAP_NODE(LILV_NS_RDFS "range");
  nodes->rsz_minimumSize        = MAP_NODE(LV2_RESIZE_PORT__minimumSize);
  nodes->ui_showInterface       = MAP_NODE(LV2_UI__showInterface);
  nodes->work_interface         = MAP_NODE(LV2_WORKER__interface);
  nodes->work_schedule          = MAP_NODE(LV2_WORKER__schedule);
  nodes->end                    = NULL;

#undef MAP_NODE
}

static void
jalv_init_env(SerdEnv* const env)
{
  serd_env_set_prefix_from_strings(
    env, (const uint8_t*)"patch", (const uint8_t*)LV2_PATCH_PREFIX);
  serd_env_set_prefix_from_strings(
    env, (const uint8_t*)"time", (const uint8_t*)LV2_TIME_PREFIX);
  serd_env_set_prefix_from_strings(
    env, (const uint8_t*)"xsd", (const uint8_t*)LILV_NS_XSD);
}

static void
jalv_init_features(Jalv* const jalv)
{
  // urid:map
  jalv->map.handle = jalv;
  jalv->map.map    = map_uri;
  init_feature(&jalv->features.map_feature, LV2_URID__map, &jalv->map);

  // urid:unmap
  jalv->unmap.handle = jalv;
  jalv->unmap.unmap  = unmap_uri;
  init_feature(&jalv->features.unmap_feature, LV2_URID__unmap, &jalv->unmap);

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
jalv_init_options(Jalv* const jalv)
{
  const LV2_Options_Option options[ARRAY_SIZE(jalv->features.options)] = {
    {LV2_OPTIONS_INSTANCE,
     0,
     jalv->urids.param_sampleRate,
     sizeof(float),
     jalv->urids.atom_Float,
     &jalv->sample_rate},
    {LV2_OPTIONS_INSTANCE,
     0,
     jalv->urids.bufsz_minBlockLength,
     sizeof(int32_t),
     jalv->urids.atom_Int,
     &jalv->block_length},
    {LV2_OPTIONS_INSTANCE,
     0,
     jalv->urids.bufsz_maxBlockLength,
     sizeof(int32_t),
     jalv->urids.atom_Int,
     &jalv->block_length},
    {LV2_OPTIONS_INSTANCE,
     0,
     jalv->urids.bufsz_sequenceSize,
     sizeof(int32_t),
     jalv->urids.atom_Int,
     &jalv->midi_buf_size},
    {LV2_OPTIONS_INSTANCE,
     0,
     jalv->urids.ui_updateRate,
     sizeof(float),
     jalv->urids.atom_Float,
     &jalv->ui_update_hz},
    {LV2_OPTIONS_INSTANCE,
     0,
     jalv->urids.ui_scaleFactor,
     sizeof(float),
     jalv->urids.atom_Float,
     &jalv->ui_scale_factor},
    {LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, NULL}};

  memcpy(jalv->features.options, options, sizeof(jalv->features.options));

  init_feature(&jalv->features.options_feature,
               LV2_OPTIONS__options,
               (void*)jalv->features.options);
}

static void
jalv_init_display(Jalv* const jalv)
{
  if (!jalv->opts.update_rate) {
    // Calculate a reasonable UI update frequency
    jalv->ui_update_hz = jalv_frontend_refresh_rate(jalv);
  } else {
    // Use user-specified UI update rate
    jalv->ui_update_hz = jalv->opts.update_rate;
    jalv->ui_update_hz = MAX(1.0f, jalv->ui_update_hz);
  }

  if (!jalv->opts.scale_factor) {
    // Calculate the monitor's scale factor
    jalv->ui_scale_factor = jalv_frontend_scale_factor(jalv);
  } else {
    // Use user-specified UI scale factor
    jalv->ui_scale_factor = jalv->opts.scale_factor;
  }

  // The UI can only go so fast, clamp to reasonable limits
  jalv->ui_update_hz     = MIN(60, jalv->ui_update_hz);
  jalv->opts.buffer_size = MAX(4096, jalv->opts.buffer_size);
  jalv_log(JALV_LOG_INFO, "Comm buffers: %u bytes\n", jalv->opts.buffer_size);
  jalv_log(JALV_LOG_INFO, "Update rate:  %.01f Hz\n", jalv->ui_update_hz);
  jalv_log(JALV_LOG_INFO, "Scale factor: %.01f\n", jalv->ui_scale_factor);
}

int
jalv_open(Jalv* const jalv, int* argc, char*** argv)
{
#if USE_SUIL
  suil_init(argc, argv, SUIL_ARG_NONE);
#endif

  // Parse command-line arguments
  int ret = 0;
  if ((ret = jalv_frontend_init(argc, argv, &jalv->opts))) {
    jalv_close(jalv);
    return ret;
  }

  // Load the LV2 world
  LilvWorld* const world = lilv_world_new();
  lilv_world_load_all(world);

  jalv->world         = world;
  jalv->env           = serd_env_new(NULL);
  jalv->symap         = symap_new();
  jalv->block_length  = 4096U;
  jalv->midi_buf_size = 1024U;
  jalv->play_state    = JALV_PAUSED;
  jalv->bpm           = 120.0f;
  jalv->control_in    = (uint32_t)-1;
  jalv->log.urids     = &jalv->urids;
  jalv->log.tracing   = jalv->opts.trace;

  zix_sem_init(&jalv->symap_lock, 1);
  zix_sem_init(&jalv->work_lock, 1);
  zix_sem_init(&jalv->done, 0);
  zix_sem_init(&jalv->paused, 0);

  jalv_init_env(jalv->env);
  jalv_init_urids(jalv->symap, &jalv->urids);
  jalv_init_nodes(world, &jalv->nodes);
  jalv_init_features(jalv);
  lv2_atom_forge_init(&jalv->forge, &jalv->map);

  // Set up atom reading and writing environment
  jalv->sratom = sratom_new(&jalv->map);
  sratom_set_env(jalv->sratom, jalv->env);

  // Create temporary directory for plugin state
#ifdef _WIN32
  jalv->temp_dir = jalv_strdup("jalvXXXXXX");
  _mktemp(jalv->temp_dir);
#else
  char* templ    = jalv_strdup("/tmp/jalv-XXXXXX");
  jalv->temp_dir = jalv_strjoin(mkdtemp(templ), "/");
  free(templ);
#endif

  // Get plugin URI from loaded state or command line
  LilvState* state      = NULL;
  LilvNode*  plugin_uri = NULL;
  if (jalv->opts.load) {
    struct stat info;
    stat(jalv->opts.load, &info);
    if ((info.st_mode & S_IFMT) == S_IFDIR) {
      char* path = jalv_strjoin(jalv->opts.load, "/state.ttl");
      state = lilv_state_new_from_file(jalv->world, &jalv->map, NULL, path);
      free(path);
    } else {
      state = lilv_state_new_from_file(
        jalv->world, &jalv->map, NULL, jalv->opts.load);
    }
    if (!state) {
      jalv_log(JALV_LOG_ERR, "Failed to load state from %s\n", jalv->opts.load);
      jalv_close(jalv);
      return -2;
    }
    plugin_uri = lilv_node_duplicate(lilv_state_get_plugin_uri(state));
  } else if (*argc > 1) {
    plugin_uri = lilv_new_uri(world, (*argv)[*argc - 1]);
  }

  if (!plugin_uri) {
    plugin_uri = jalv_frontend_select_plugin(jalv);
  }

  if (!plugin_uri) {
    jalv_log(JALV_LOG_ERR, "Missing plugin URI, try lv2ls to list plugins\n");
    jalv_close(jalv);
    return -3;
  }

  // Find plugin
  const char* const        plugin_uri_str = lilv_node_as_string(plugin_uri);
  const LilvPlugins* const plugins        = lilv_world_get_all_plugins(world);
  jalv_log(JALV_LOG_INFO, "Plugin:       %s\n", plugin_uri_str);
  jalv->plugin = lilv_plugins_get_by_uri(plugins, plugin_uri);
  lilv_node_free(plugin_uri);
  if (!jalv->plugin) {
    jalv_log(JALV_LOG_ERR, "Failed to find plugin\n");
    jalv_close(jalv);
    return -4;
  }

  // Create workers if necessary
  if (lilv_plugin_has_extension_data(jalv->plugin,
                                     jalv->nodes.work_interface)) {
    jalv->worker                = jalv_worker_new(&jalv->work_lock, true);
    jalv->features.sched.handle = jalv->worker;
    if (jalv->safe_restore) {
      jalv->state_worker           = jalv_worker_new(&jalv->work_lock, false);
      jalv->features.ssched.handle = jalv->state_worker;
    }
  }

  // Load preset, if specified
  if (jalv->opts.preset) {
    LilvNode* preset = lilv_new_uri(jalv->world, jalv->opts.preset);

    jalv_load_presets(jalv, NULL, NULL);
    state        = lilv_state_new_from_world(jalv->world, &jalv->map, preset);
    jalv->preset = state;
    lilv_node_free(preset);
    if (!state) {
      jalv_log(JALV_LOG_ERR, "Failed to find preset <%s>\n", jalv->opts.preset);
      jalv_close(jalv);
      return -5;
    }
  }

  // Check for thread-safe state restore() method
  LilvNode* state_threadSafeRestore =
    lilv_new_uri(jalv->world, LV2_STATE__threadSafeRestore);
  if (lilv_plugin_has_feature(jalv->plugin, state_threadSafeRestore)) {
    jalv->safe_restore = true;
  }
  lilv_node_free(state_threadSafeRestore);

  if (!state) {
    // Not restoring state, load the plugin as a preset to get default
    state = lilv_state_new_from_world(
      jalv->world, &jalv->map, lilv_plugin_get_uri(jalv->plugin));
  }

  // Get a plugin UI
  jalv->uis = lilv_plugin_get_uis(jalv->plugin);
  if (!jalv->opts.generic_ui) {
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

      if (jalv->ui) {
        jalv_log(JALV_LOG_INFO,
                 "UI:           %s\n",
                 lilv_node_as_uri(lilv_ui_get_uri(jalv->ui)));
      }
    }
  }

  // Create port and control structures
  jalv_create_ports(jalv);
  jalv_create_controls(jalv, true);
  jalv_create_controls(jalv, false);

  if (!(jalv->backend = jalv_backend_init(jalv))) {
    jalv_log(JALV_LOG_ERR, "Failed to connect to audio system\n");
    jalv_close(jalv);
    return -6;
  }

  jalv_log(JALV_LOG_INFO, "Sample rate:  %u Hz\n", (uint32_t)jalv->sample_rate);
  jalv_log(JALV_LOG_INFO, "Block length: %u frames\n", jalv->block_length);
  jalv_log(JALV_LOG_INFO, "MIDI buffers: %zu bytes\n", jalv->midi_buf_size);

  if (jalv->opts.buffer_size == 0) {
    /* The UI ring is fed by plugin output ports (usually one), and the UI
       updates roughly once per cycle.  The ring size is a few times the size
       of the MIDI output to give the UI a chance to keep up.  The UI should be
       able to keep up with 4 cycles, and tests show this works for me, but
       this value might need increasing to avoid overflows. */
    jalv->opts.buffer_size = jalv->midi_buf_size * N_BUFFER_CYCLES;
  }

  jalv_init_display(jalv);
  jalv_init_options(jalv);

  // Create Plugin <=> UI communication buffers
  jalv->ui_to_plugin = zix_ring_new(NULL, jalv->opts.buffer_size);
  jalv->plugin_to_ui = zix_ring_new(NULL, jalv->opts.buffer_size);
  zix_ring_mlock(jalv->ui_to_plugin);
  zix_ring_mlock(jalv->plugin_to_ui);

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
    jalv_close(jalv);
    return -7;
  }
  memcpy(jalv->feature_list, features, sizeof(features));

  // Check that any required features are supported
  LilvNodes* req_feats = lilv_plugin_get_required_features(jalv->plugin);
  LILV_FOREACH (nodes, f, req_feats) {
    const char* uri = lilv_node_as_uri(lilv_nodes_get(req_feats, f));
    if (!feature_is_supported(jalv, uri)) {
      jalv_log(JALV_LOG_ERR, "Feature %s is not supported\n", uri);
      jalv_close(jalv);
      return -8;
    }
  }
  lilv_nodes_free(req_feats);

  // Instantiate the plugin
  jalv->instance = lilv_plugin_instantiate(
    jalv->plugin, jalv->sample_rate, jalv->feature_list);
  if (!jalv->instance) {
    jalv_log(JALV_LOG_ERR, "Failed to instantiate plugin\n");
    jalv_close(jalv);
    return -9;
  }

  // Point things to the instance that require it

  jalv->features.ext_data.data_access =
    lilv_instance_get_descriptor(jalv->instance)->extension_data;

  const LV2_Worker_Interface* worker_iface =
    (const LV2_Worker_Interface*)lilv_instance_get_extension_data(
      jalv->instance, LV2_WORKER__interface);

  jalv_worker_start(jalv->worker, worker_iface, jalv->instance->lv2_handle);
  jalv_worker_start(
    jalv->state_worker, worker_iface, jalv->instance->lv2_handle);

  jalv_log(JALV_LOG_INFO, "\n");
  if (!jalv->buf_size_set) {
    jalv_allocate_port_buffers(jalv);
  }

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
    jalv_backend_activate_port(jalv, i);
  }

  // Print initial control values
  for (size_t i = 0; i < jalv->controls.n_controls; ++i) {
    ControlID* control = jalv->controls.controls[i];
    if (control->type == PORT && control->is_writable) {
      struct Port* port = &jalv->ports[control->index];
      jalv_print_control(jalv, port, port->control);
    }
  }

  // Activate plugin
  lilv_instance_activate(jalv->instance);

  // Discover UI
  jalv->has_ui = jalv_frontend_discover(jalv);

  // Activate audio backend
  jalv_backend_activate(jalv);
  jalv->play_state = JALV_RUNNING;

  return 0;
}

int
jalv_close(Jalv* const jalv)
{
  // Terminate the worker
  jalv_worker_exit(jalv->worker);

  // Deactivate audio
  if (jalv->backend) {
    jalv_backend_deactivate(jalv);
    jalv_backend_close(jalv);
  }

  // Free event port buffers
  for (uint32_t i = 0; i < jalv->num_ports; ++i) {
    if (jalv->ports[i].evbuf) {
      lv2_evbuf_free(jalv->ports[i].evbuf);
    }
  }

  // Destroy the worker
  jalv_worker_free(jalv->worker);
  jalv_worker_free(jalv->state_worker);

  // Deactivate plugin
#if USE_SUIL
  suil_instance_free(jalv->ui_instance);
#endif
  if (jalv->instance) {
    lilv_instance_deactivate(jalv->instance);
    lilv_instance_free(jalv->instance);
  }

  // Clean up
  lilv_state_free(jalv->preset);
  free(jalv->ports);
  zix_ring_free(jalv->ui_to_plugin);
  zix_ring_free(jalv->plugin_to_ui);
  for (LilvNode** n = (LilvNode**)&jalv->nodes; *n; ++n) {
    lilv_node_free(*n);
  }
  symap_free(jalv->symap);
  zix_sem_destroy(&jalv->symap_lock);
#if USE_SUIL
  suil_host_free(jalv->ui_host);
#endif

  for (unsigned i = 0; i < jalv->controls.n_controls; ++i) {
    ControlID* const control = jalv->controls.controls[i];
    lilv_node_free(control->node);
    lilv_node_free(control->symbol);
    lilv_node_free(control->label);
    lilv_node_free(control->group);
    lilv_node_free(control->min);
    lilv_node_free(control->max);
    lilv_node_free(control->def);
    free(control);
  }
  free(jalv->controls.controls);

  sratom_free(jalv->sratom);
  serd_env_free(jalv->env);
  lilv_uis_free(jalv->uis);
  lilv_world_free(jalv->world);

  zix_sem_destroy(&jalv->done);

  remove(jalv->temp_dir);
  free(jalv->temp_dir);
  free(jalv->ui_event_buf);
  free(jalv->feature_list);

  free(jalv->opts.name);
  free(jalv->opts.load);
  free(jalv->opts.controls);

  return 0;
}

int
main(int argc, char** argv)
{
  Jalv jalv;
  memset(&jalv, '\0', sizeof(Jalv));

  if (jalv_open(&jalv, &argc, &argv)) {
    return EXIT_FAILURE;
  }

  // Set up signal handlers
  setup_signals(&jalv);

  // Run UI (or prompt at console)
  jalv_frontend_open(&jalv);

  // Wait for finish signal from UI or signal handler
  zix_sem_wait(&jalv.done);

  return jalv_close(&jalv);
}
