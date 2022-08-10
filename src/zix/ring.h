// Copyright 2011-2022 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef ZIX_RING_H
#define ZIX_RING_H

#include "zix/allocator.h"
#include "zix/attributes.h"
#include "zix/common.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
   @addtogroup zix
   @{
   @name Ring
   @{
*/

/**
   A lock-free ring buffer.

   Thread-safe (with a few noted exceptions) for a single reader and single
   writer, and realtime-safe on both ends.
*/
typedef struct ZixRingImpl ZixRing;

/**
   A transaction for writing data in multiple parts.

   The simple zix_ring_write() can be used to write an atomic message that will
   immediately be visible to the reader, but transactions allow data to be
   written in several chunks before being "committed" and becoming readable.
   This can be useful for things like prefixing messages with a header without
   needing an allocated buffer to construct the "packet".
*/
typedef struct {
  uint32_t read_head;  ///< Read head at the start of the transaction
  uint32_t write_head; ///< Write head if the transaction were committed
} ZixRingTransaction;

/**
   Create a new ring.
   @param size Size in bytes (note this may be rounded up).

   At most `size` - 1 bytes may be stored in the ring at once.
*/
ZIX_MALLOC_API
ZixRing* ZIX_ALLOCATED
zix_ring_new(ZixAllocator* ZIX_NULLABLE allocator, uint32_t size);

/// Destroy a ring
ZIX_API
void
zix_ring_free(ZixRing* ZIX_NULLABLE ring);

/**
   Lock the ring data into physical memory.

   This function is NOT thread safe or real-time safe, but it should be called
   after zix_ring_new() to lock all ring memory to avoid page faults while
   using the ring.
*/
ZIX_API
void
zix_ring_mlock(ZixRing* ZIX_NONNULL ring);

/**
   Reset (empty) a ring.

   This function is NOT thread-safe, it may only be called when there is no
   reader or writer.
*/
ZIX_API
void
zix_ring_reset(ZixRing* ZIX_NONNULL ring);

/**
   Return the number of bytes of space available for reading.

   Reader only.
*/
ZIX_PURE_API
uint32_t
zix_ring_read_space(const ZixRing* ZIX_NONNULL ring);

/**
   Return the number of bytes of space available for writing.

   Writer only.
*/
ZIX_PURE_API
uint32_t
zix_ring_write_space(const ZixRing* ZIX_NONNULL ring);

/**
   Return the capacity (the total write space when empty).

   This function returns a constant for any given ring, and may (but usually
   shouldn't) be called anywhere.
*/
ZIX_PURE_API
uint32_t
zix_ring_capacity(const ZixRing* ZIX_NONNULL ring);

/**
   Read from the ring without advancing the read head.

   Reader only.
*/
ZIX_API
uint32_t
zix_ring_peek(ZixRing* ZIX_NONNULL ring, void* ZIX_NONNULL dst, uint32_t size);

/**
   Read from the ring and advance the read head.

   Reader only.
*/
ZIX_API
uint32_t
zix_ring_read(ZixRing* ZIX_NONNULL ring, void* ZIX_NONNULL dst, uint32_t size);

/**
   Skip data in the ring (advance read head without reading).

   Reader only.
*/
ZIX_API
uint32_t
zix_ring_skip(ZixRing* ZIX_NONNULL ring, uint32_t size);

/**
   Write data to the ring.

   Writer only.
*/
ZIX_API
uint32_t
zix_ring_write(ZixRing* ZIX_NONNULL    ring,
               const void* ZIX_NONNULL src,
               uint32_t                size);

/**
   Begin a write.

   The returned transaction is initially empty.  Data can be written to it by
   calling zix_ring_amend_write() one or more times, then finishing with
   zix_ring_commit_write().

   Note that the returned "transaction" is not meant to be long-lived: a call
   to this function should be (more or less) immediately followed by calls to
   zix_ring_amend_write() then a call to zix_ring_commit_write().

   @param ring The ring to write data to.
   @return A new empty transaction.
*/
ZixRingTransaction
zix_ring_begin_write(ZixRing* ZIX_NONNULL ring);

/**
   Amend the current write with some data.

   The data is written immediately after the previously amended data, as if
   they were written contiguously with a single write call.  This data is not
   visible to the reader until zix_ring_commit_write() is called.

   If any call to this function returns an error, then the transaction is
   invalid and must not be committed.  No cleanup is necessary for an invalid
   transaction.  Any bytes written while attempting the transaction will remain
   in the free portion of the buffer and be overwritten by subsequent writes.

   @param ring The ring this transaction is writing to.
   @param tx The active transaction, from zix_ring_begin_write().
   @param src Pointer to the data to write.
   @param size Length of data to write in bytes.
   @return #ZIX_STATUS_NO_MEM or #ZIX_STATUS_SUCCESS.
*/
ZixStatus
zix_ring_amend_write(ZixRing* ZIX_NONNULL            ring,
                     ZixRingTransaction* ZIX_NONNULL tx,
                     const void* ZIX_NONNULL         src,
                     uint32_t                        size);

/**
   Commit the current write.

   This atomically updates the state of the ring, so that the reader will
   observe the data written during the transaction.

   This function usually shouldn't be called for any transaction which
   zix_ring_amend_write() returned an error for.

   @param ring The ring this transaction is writing to.
   @param tx The active transaction, from zix_ring_begin_write().
   @return #ZIX_STATUS_SUCCESS.
*/
ZixStatus
zix_ring_commit_write(ZixRing* ZIX_NONNULL                  ring,
                      const ZixRingTransaction* ZIX_NONNULL tx);

/**
   @}
   @}
*/

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ZIX_RING_H */
