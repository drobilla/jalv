// Copyright 2011-2022 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

/**
   @file symap.h API for Symap, a basic symbol map (string interner).

   Particularly useful for implementing LV2 URI mapping.

   @see <a href="http://lv2plug.in/ns/ext/urid">LV2 URID</a>
*/

#ifndef SYMAP_H
#define SYMAP_H

#include <stdint.h>

typedef struct SymapImpl Symap;

/// Create a new symbol map
Symap*
symap_new(void);

/// Free a symbol map
void
symap_free(Symap* map);

/// Map a string to a symbol ID if it is already mapped, otherwise return 0
uint32_t
symap_try_map(Symap* map, const char* sym);

/**
   Map a string to a symbol ID.

   Note that 0 is never a valid symbol ID.
*/
uint32_t
symap_map(Symap* map, const char* sym);

/**
   Unmap a symbol ID back to a symbol, or NULL if no such ID exists.

   Note that 0 is never a valid symbol ID.
*/
const char*
symap_unmap(Symap* map, uint32_t id);

#endif // SYMAP_H
