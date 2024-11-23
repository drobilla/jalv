// Copyright 2007-2024 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "backend.h"
#include "comm.h"
#include "log.h"
#include "lv2_evbuf.h"
#include "process.h"
#include "settings.h"
#include "types.h"
#include "urids.h"

#include <lilv/lilv.h>
#include <zix/attributes.h>
#include <zix/sem.h>

#include <portaudio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct JalvBackendImpl {
  PaStream* stream;
};

static int
process_silent(JalvProcess* const  proc,
               void* const         outputs,
               const unsigned long nframes)
{
  for (uint32_t i = 0; i < proc->num_ports; ++i) {
    memset(((float**)outputs)[i], '\0', nframes * sizeof(float));
  }

  return jalv_bypass(proc, nframes);
}

static int
process_cb(const void*                     inputs,
           void*                           outputs,
           unsigned long                   nframes,
           const PaStreamCallbackTimeInfo* time,
           PaStreamCallbackFlags           flags,
           void*                           handle)
{
  (void)time;
  (void)flags;

  JalvProcess* const proc = (JalvProcess*)handle;

  // If execution is paused, emit silence and return
  if (proc->run_state == JALV_PAUSED) {
    return process_silent(proc, outputs, nframes);
  }

  // Prepare port buffers
  uint32_t in_index  = 0;
  uint32_t out_index = 0;
  for (uint32_t i = 0; i < proc->num_ports; ++i) {
    JalvProcessPort* const port = &proc->ports[i];
    if (port->type == TYPE_AUDIO) {
      if (port->flow == FLOW_INPUT) {
        lilv_instance_connect_port(
          proc->instance, i, ((float**)inputs)[in_index++]);
      } else if (port->flow == FLOW_OUTPUT) {
        lilv_instance_connect_port(
          proc->instance, i, ((float**)outputs)[out_index++]);
      }
    } else if (port->type == TYPE_EVENT) {
      lv2_evbuf_reset(port->evbuf, port->flow == FLOW_INPUT);
    }
  }

  // Run plugin for this cycle
  const bool send_ui_updates = jalv_run(proc, nframes);

  // Deliver UI events
  for (uint32_t p = 0; p < proc->num_ports; ++p) {
    JalvProcessPort* const port = &proc->ports[p];
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

        if (proc->has_ui) {
          // Forward event to UI
          jalv_write_event(proc->plugin_to_ui, p, size, type, body);
        }
      }
    } else if (send_ui_updates && port->flow == FLOW_OUTPUT &&
               port->type == TYPE_CONTROL) {
      jalv_write_control(proc->plugin_to_ui, p, proc->controls_buf[p]);
    }
  }

  return paContinue;
}

static int
setup_error(const char* msg, PaError err)
{
  jalv_log(JALV_LOG_ERR, "%s (%s)\n", msg, Pa_GetErrorText(err));
  Pa_Terminate();
  return 1;
}

JalvBackend*
jalv_backend_allocate(void)
{
  return (JalvBackend*)calloc(1, sizeof(JalvBackend));
}

void
jalv_backend_free(JalvBackend* const backend)
{
  free(backend);
}

int
jalv_backend_open(JalvBackend* const     backend,
                  const JalvURIDs* const ZIX_UNUSED(urids),
                  JalvSettings* const    settings,
                  JalvProcess* const     proc,
                  ZixSem* const          ZIX_UNUSED(done),
                  const char* const      ZIX_UNUSED(name),
                  const bool             ZIX_UNUSED(exact_name))
{
  PaStreamParameters inputParameters;
  PaStreamParameters outputParameters;
  PaStream*          stream = NULL;
  PaError            st     = paNoError;

  if ((st = Pa_Initialize())) {
    return setup_error("Failed to initialize audio system", st);
  }

  // Get default input and output devices
  inputParameters.device  = Pa_GetDefaultInputDevice();
  outputParameters.device = Pa_GetDefaultOutputDevice();
  if (inputParameters.device == paNoDevice) {
    return setup_error("No default input device", paDeviceUnavailable);
  }

  if (outputParameters.device == paNoDevice) {
    return setup_error("No default output device", paDeviceUnavailable);
  }

  const PaDeviceInfo* in_dev  = Pa_GetDeviceInfo(inputParameters.device);
  const PaDeviceInfo* out_dev = Pa_GetDeviceInfo(outputParameters.device);

  // Count number of input and output audio ports/channels
  inputParameters.channelCount  = 0;
  outputParameters.channelCount = 0;
  for (uint32_t i = 0; i < proc->num_ports; ++i) {
    if (proc->ports[i].type == TYPE_AUDIO) {
      if (proc->ports[i].flow == FLOW_INPUT) {
        ++inputParameters.channelCount;
      } else if (proc->ports[i].flow == FLOW_OUTPUT) {
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
                       process_cb,
                       proc))) {
    return setup_error("Failed to open audio stream", st);
  }

  // Set audio parameters
  settings->sample_rate = in_dev->defaultSampleRate;
  // settings->block_length  = FIXME
  settings->midi_buf_size = 4096;

  backend->stream = stream;
  return 0;
}

void
jalv_backend_close(JalvBackend* const backend)
{
  if (backend) {
    PaError st = paNoError;
    if (backend->stream && (st = Pa_CloseStream(backend->stream))) {
      jalv_log(JALV_LOG_ERR, "Error closing audio (%s)\n", Pa_GetErrorText(st));
    }

    if ((st = Pa_Terminate())) {
      jalv_log(
        JALV_LOG_ERR, "Error terminating audio (%s)\n", Pa_GetErrorText(st));
    }
  }
}

void
jalv_backend_activate(JalvBackend* const backend)
{
  const PaError st = Pa_StartStream(backend->stream);
  if (st != paNoError) {
    jalv_log(JALV_LOG_ERR, "Error starting audio (%s)\n", Pa_GetErrorText(st));
  }
}

void
jalv_backend_deactivate(JalvBackend* const backend)
{
  const PaError st = Pa_StopStream(backend->stream);
  if (st != paNoError) {
    jalv_log(JALV_LOG_ERR, "Error stopping audio (%s)\n", Pa_GetErrorText(st));
  }
}

void
jalv_backend_activate_port(JalvBackend* const ZIX_UNUSED(backend),
                           JalvProcess* const proc,
                           const uint32_t     port_index)
{
  JalvProcessPort* const port = &proc->ports[port_index];

  if (port->type == TYPE_CONTROL) {
    lilv_instance_connect_port(
      proc->instance, port_index, &proc->controls_buf[port_index]);
  }
}

void
jalv_backend_recompute_latencies(JalvBackend* const ZIX_UNUSED(backend))
{}
