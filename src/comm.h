// Copyright 2007-2024 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_COMM_H
#define JALV_COMM_H

#include "attributes.h"

#include "lv2/urid/urid.h"
#include "zix/ring.h"
#include "zix/status.h"

#include <stdint.h>

JALV_BEGIN_DECLS

// Communication between the audio and main threads via rings

/**
   Write a message in two parts to a ring.

   This is used to conveniently write a message with a fixed-size header and
   possibly variably-sized body in a single call.

   @param target Communication ring (jalv->plugin_to_ui or jalv->ui_to_plugin).
   @param header Pointer to start of header data.
   @param header_size Size of header in bytes.
   @param body Pointer to start of body data.
   @param body_size Size of body in bytes.
   @return 0 on success, non-zero on failure (overflow).
*/
ZixStatus
jalv_write_split_message(ZixRing*    target,
                         const void* header,
                         uint32_t    header_size,
                         const void* body,
                         uint32_t    body_size);

/**
   Write a port event using the atom:eventTransfer protocol.

   This is used to transfer atoms between the plugin and UI via sequence ports.

   @param target Communication ring (jalv->plugin_to_ui or jalv->ui_to_plugin).
   @param port_index Index of the port this change is for.
   @param protocol Port protocol (0 for float control, or atom:eventTransfer).o
   @param size Size of body in bytes.
   @param type Atom type URID.
   @param body Atom body.
   @return 0 on success, non-zero on failure (overflow).
*/
ZixStatus
jalv_write_event(ZixRing*    target,
                 uint32_t    port_index,
                 uint32_t    protocol,
                 uint32_t    size,
                 LV2_URID    type,
                 const void* body);

/**
   Write a control port change using the default (0) protocol.

   This is used to transfer control port value changes between the plugin and
   UI.

   @param target Communication ring (jalv->plugin_to_ui or jalv->ui_to_plugin).
   @param port_index Index of the port this change is for.
   @param value New control port value.
   @return 0 on success, non-zero on failure (overflow).
*/
ZixStatus
jalv_write_control(ZixRing* target, uint32_t port_index, float value);

JALV_END_DECLS

#endif // JALV_COMM_H
