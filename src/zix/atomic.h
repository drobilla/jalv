/*
  Copyright 2017 Timo Wischer <twischer@de.adit-jv.com>

  Permission to use, copy, modify, and/or distribute this software for any
  purpose with or without fee is hereby granted, provided that the above
  copyright notice and this permission notice appear in all copies.

  THIS SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

#ifndef ZIX_ATOMIC_H
#define ZIX_ATOMIC_H

/* All these atomic instructions include a full memory barrier */
#ifndef __STDC_NO_ATOMICS__
#include <stdatomic.h>

#    define ZIX_ATOMIC_WRITE(VARP, VAL)    atomic_store((VARP), (VAL))
#    define ZIX_ATOMIC_EXCHANGE(VARP, VAL) atomic_exchange((VARP), (VAL))
#    define ZIX_ATOMIC_READ(VARP)          atomic_load((VARP))
#    define ZIX_ATOMIC_ADD(VARP, VAL)      atomic_fetch_add((VARP), (VAL))

typedef _Atomic float atomic_float;
#else
#    pragma message("warning: No atomic load/store/add, possible SMP bugs")
#    define ZIX_ATOMIC_WRITE(VARP, VAL)    (*(VARP)) =  (VAL)
#    define ZIX_ATOMIC_EXCHANGE(VARP, VAL) (*(VARP)); (*(VARP)) =  (VAL)
#    define ZIX_ATOMIC_READ(VARP)          (*(VARP))
#    define ZIX_ATOMIC_ADD(VARP, VAL)      (*(VARP)) = (*(VARP)) + (VAL)

typedef volatile bool atomic_bool;
typedef volatile int atomic_int;
typedef volatile unsigned int atomic_uint;
typedef volatile float atomic_float;
#endif

#endif // ZIX_ATOMIC_H
