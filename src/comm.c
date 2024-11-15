// Copyright 2007-2024 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "comm.h"

#include "control.h"

#include "lv2/atom/atom.h"
#include "lv2/urid/urid.h"

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
                 const uint32_t    protocol,
                 const uint32_t    size,
                 const LV2_URID    type,
                 const void* const body)
{
  // TODO: Be more discriminate about what to send

  typedef struct {
    ControlChange change;
    LV2_Atom      atom;
  } Header;

  const Header header = {{port_index, protocol, sizeof(LV2_Atom) + size},
                         {size, type}};

  return jalv_write_split_message(target, &header, sizeof(header), body, size);
}

ZixStatus
jalv_write_control(ZixRing* const target,
                   const uint32_t port_index,
                   const float    value)
{
  const ControlChange header = {port_index, 0, sizeof(value)};

  return jalv_write_split_message(
    target, &header, sizeof(header), &value, sizeof(value));
}
