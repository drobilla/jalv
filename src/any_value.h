// Copyright 2007-2025 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_ANY_VALUE_H
#define JALV_ANY_VALUE_H

#include "attributes.h"

#include <lilv/lilv.h>
#include <lv2/atom/forge.h>

#include <stdint.h>

// Simple atom-like variant for storing any control value
JALV_BEGIN_DECLS

/**
   The value of a high-level control used by the frontend.

   The value body is stored inline, or dynamically allocated if it's larger
   than a pointer.  The all-zero struct is used as a null value.
*/
typedef struct {
  uint32_t size; ///< Size of value in bytes
  uint32_t type; ///< Value type URID
  union {
    uintptr_t number;  ///< Inline value if size < sizeof(void*)
    void*     pointer; ///< Pointer to value if size > sizeof(void*)
  } value;
} AnyValue;

/// Reset a value to zero, releasing any memory it owns
void
any_value_reset(AnyValue* any_value);

/// Set a value to a new value, allocating if needed
int
any_value_set(AnyValue*   any_value,
              uint32_t    value_size,
              uint32_t    value_type,
              const void* value_body);

/// Set a value from a document node, converting/allocating if needed
int
any_value_set_node(AnyValue*             any_value,
                   const LilvNode*       node,
                   const LV2_Atom_Forge* forge);

/// Return a pointer to the value body
const void*
any_value_data(const AnyValue* any_value);

/// Get a numeric value as a float, defaulting to zero
double
any_value_number(const AnyValue* any_value, const LV2_Atom_Forge* forge);

JALV_END_DECLS

#endif // JALV_ANY_VALUE_H
