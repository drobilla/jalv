// Copyright 2007-2024 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "comm.h"

#include <lv2/urid/urid.h>
#include <zix/ring.h>
#include <zix/status.h>

ZixStatus
jalv_write_split_message(ZixRing* const    target,
                         const void* const header,
                         const uint32_t    header_size,
                         const void* const body,
                         const uint32_t    body_size)
{
  ZixRingTransaction tx = zix_ring_begin_write(target);

  ZixStatus st = ZIX_STATUS_SUCCESS;
  if (!(st = zix_ring_amend_write(target, &tx, header, header_size)) &&
      !(st = zix_ring_amend_write(target, &tx, body, body_size))) {
    st = zix_ring_commit_write(target, &tx);
  }

  return st;
}

ZixStatus
jalv_write_event(ZixRing* const    target,
                 const uint32_t    port_index,
                 const uint32_t    size,
                 const LV2_URID    type,
                 const void* const body)
{
  // TODO: Be more discriminate about what to send

  typedef struct {
    JalvMessageHeader message;
    JalvEventTransfer event;
  } Header;

  const Header header = {{EVENT_TRANSFER, sizeof(JalvEventTransfer) + size},
                         {port_index, {size, type}}};

  return jalv_write_split_message(target, &header, sizeof(header), body, size);
}

ZixStatus
jalv_write_control(ZixRing* const target,
                   const uint32_t port_index,
                   const float    value)
{
  typedef struct {
    JalvMessageHeader message;
    JalvControlChange control;
  } Message;

  const Message msg = {{CONTROL_PORT_CHANGE, sizeof(JalvControlChange)},
                       {port_index, value}};

  return zix_ring_write(target, &msg, sizeof(msg)) == sizeof(msg)
           ? ZIX_STATUS_SUCCESS
           : ZIX_STATUS_ERROR;
}
