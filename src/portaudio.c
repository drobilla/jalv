// Copyright 2007-2022 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "backend.h"
#include "jalv_internal.h"
#include "log.h"
#include "lv2_evbuf.h"
#include "port.h"
#include "types.h"

#include "lilv/lilv.h"
#include "lv2/atom/atom.h"

#include <portaudio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

struct JalvBackendImpl {
  PaStream* stream;
};

static int
pa_process_cb(const void*                     inputs,
              void*                           outputs,
              unsigned long                   nframes,
              const PaStreamCallbackTimeInfo* time,
              PaStreamCallbackFlags           flags,
              void*                           handle)
{
  (void)time;
  (void)flags;

  Jalv* jalv = (Jalv*)handle;

  // Prepare port buffers
  uint32_t in_index  = 0;
  uint32_t out_index = 0;
  for (uint32_t i = 0; i < jalv->num_ports; ++i) {
    struct Port* port = &jalv->ports[i];
    if (port->type == TYPE_AUDIO) {
      if (port->flow == FLOW_INPUT) {
        lilv_instance_connect_port(
          jalv->instance, i, ((float**)inputs)[in_index++]);
      } else if (port->flow == FLOW_OUTPUT) {
        lilv_instance_connect_port(
          jalv->instance, i, ((float**)outputs)[out_index++]);
      }
    } else if (port->type == TYPE_EVENT && port->flow == FLOW_INPUT) {
      lv2_evbuf_reset(port->evbuf, true);

      if (jalv->request_update) {
        // Plugin state has changed, request an update
        const LV2_Atom_Object get = {
          {sizeof(LV2_Atom_Object_Body), jalv->urids.atom_Object},
          {0, jalv->urids.patch_Get}};
        LV2_Evbuf_Iterator iter = lv2_evbuf_begin(port->evbuf);
        lv2_evbuf_write(
          &iter, 0, 0, get.atom.type, get.atom.size, LV2_ATOM_BODY(&get));
      }
    } else if (port->type == TYPE_EVENT) {
      // Clear event output for plugin to write to
      lv2_evbuf_reset(port->evbuf, false);
    }
  }
  jalv->request_update = false;

  // Run plugin for this cycle
  const bool send_ui_updates = jalv_run(jalv, nframes);

  // Deliver UI events
  for (uint32_t p = 0; p < jalv->num_ports; ++p) {
    struct Port* const port = &jalv->ports[p];
    if (port->flow == FLOW_OUTPUT && port->type == TYPE_EVENT) {
      for (LV2_Evbuf_Iterator i = lv2_evbuf_begin(port->evbuf);
           lv2_evbuf_is_valid(i);
           i = lv2_evbuf_next(i)) {
        // Get event from LV2 buffer
        uint32_t frames    = 0U;
        uint32_t subframes = 0U;
        uint32_t type      = 0U;
        uint32_t size      = 0U;
        void*    body      = NULL;
        lv2_evbuf_get(i, &frames, &subframes, &type, &size, &body);

        if (jalv->has_ui) {
          // Forward event to UI
          jalv_write_event(jalv, jalv->plugin_to_ui, p, size, type, body);
        }
      }
    } else if (send_ui_updates && port->flow == FLOW_OUTPUT &&
               port->type == TYPE_CONTROL) {
      jalv_write_control(jalv, jalv->plugin_to_ui, p, port->control);
    }
  }

  return paContinue;
}

static JalvBackend*
pa_error(const char* msg, PaError err)
{
  jalv_log(JALV_LOG_ERR, "%s (%s)\n", msg, Pa_GetErrorText(err));
  Pa_Terminate();
  return NULL;
}

JalvBackend*
jalv_backend_init(Jalv* jalv)
{
  PaStreamParameters inputParameters;
  PaStreamParameters outputParameters;
  PaStream*          stream = NULL;
  PaError            st     = paNoError;

  if ((st = Pa_Initialize())) {
    return pa_error("Failed to initialize audio system", st);
  }

  // Get default input and output devices
  inputParameters.device  = Pa_GetDefaultInputDevice();
  outputParameters.device = Pa_GetDefaultOutputDevice();
  if (inputParameters.device == paNoDevice) {
    return pa_error("No default input device", paDeviceUnavailable);
  }

  if (outputParameters.device == paNoDevice) {
    return pa_error("No default output device", paDeviceUnavailable);
  }

  const PaDeviceInfo* in_dev  = Pa_GetDeviceInfo(inputParameters.device);
  const PaDeviceInfo* out_dev = Pa_GetDeviceInfo(outputParameters.device);

  // Count number of input and output audio ports/channels
  inputParameters.channelCount  = 0;
  outputParameters.channelCount = 0;
  for (uint32_t i = 0; i < jalv->num_ports; ++i) {
    if (jalv->ports[i].type == TYPE_AUDIO) {
      if (jalv->ports[i].flow == FLOW_INPUT) {
        ++inputParameters.channelCount;
      } else if (jalv->ports[i].flow == FLOW_OUTPUT) {
        ++outputParameters.channelCount;
      }
    }
  }

  // Configure audio format
  inputParameters.sampleFormat               = paFloat32 | paNonInterleaved;
  inputParameters.suggestedLatency           = in_dev->defaultLowInputLatency;
  inputParameters.hostApiSpecificStreamInfo  = NULL;
  outputParameters.sampleFormat              = paFloat32 | paNonInterleaved;
  outputParameters.suggestedLatency          = out_dev->defaultLowOutputLatency;
  outputParameters.hostApiSpecificStreamInfo = NULL;

  // Open stream
  if ((st =
         Pa_OpenStream(&stream,
                       inputParameters.channelCount ? &inputParameters : NULL,
                       outputParameters.channelCount ? &outputParameters : NULL,
                       in_dev->defaultSampleRate,
                       paFramesPerBufferUnspecified,
                       0,
                       pa_process_cb,
                       jalv))) {
    return pa_error("Failed to open audio stream", st);
  }

  // Set audio parameters
  jalv->sample_rate = in_dev->defaultSampleRate;
  // jalv->block_length  = FIXME
  jalv->midi_buf_size = 4096;

  // Allocate and return opaque backend
  JalvBackend* backend = (JalvBackend*)calloc(1, sizeof(JalvBackend));
  backend->stream      = stream;
  return backend;
}

void
jalv_backend_close(Jalv* jalv)
{
  Pa_Terminate();
  free(jalv->backend);
  jalv->backend = NULL;
}

void
jalv_backend_activate(Jalv* jalv)
{
  const int st = Pa_StartStream(jalv->backend->stream);
  if (st != paNoError) {
    jalv_log(
      JALV_LOG_ERR, "Error starting audio stream (%s)\n", Pa_GetErrorText(st));
  }
}

void
jalv_backend_deactivate(Jalv* jalv)
{
  const int st = Pa_CloseStream(jalv->backend->stream);
  if (st != paNoError) {
    jalv_log(
      JALV_LOG_ERR, "Error closing audio stream (%s)\n", Pa_GetErrorText(st));
  }
}

void
jalv_backend_activate_port(Jalv* jalv, uint32_t port_index)
{
  struct Port* const port = &jalv->ports[port_index];

  if (port->type == TYPE_CONTROL) {
    lilv_instance_connect_port(jalv->instance, port_index, &port->control);
  }
}
