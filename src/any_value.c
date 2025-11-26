// Copyright 2025 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "any_value.h"

#include <lilv/lilv.h>

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

void
any_value_reset(AnyValue* const any_value)
{
  if (any_value->size > sizeof(void*)) {
    free(any_value->value.pointer);
  }

  any_value->size         = 0U;
  any_value->type         = 0U;
  any_value->value.number = 0U;
}

int
any_value_set(AnyValue* const   any_value,
              const uint32_t    value_size,
              const uint32_t    value_type,
              const void* const value_body)
{
  if (any_value->size > sizeof(void*) && value_size < sizeof(void*)) {
    any_value_reset(any_value);
  }

  const bool is_dynamic = value_size > sizeof(void*);
  if (is_dynamic) {
    if (value_size > any_value->size) {
      // Grow allocation to accommodate new value
      void* const new_data = realloc(any_value->value.pointer, value_size);
      if (new_data) {
        any_value->value.pointer = new_data;
      } else {
        any_value_reset(any_value);
        return 1;
      }
    }

    memcpy(any_value->value.pointer, value_body, value_size);
  } else {
    assert(value_size <= sizeof(any_value->value.number));
    memcpy(&any_value->value.number, value_body, value_size);
  }

  any_value->size = value_size;
  any_value->type = value_type;
  return 0;
}

int
any_value_set_node(AnyValue* const             any_value,
                   const LilvNode* const       node,
                   const LV2_Atom_Forge* const forge)
{
  if (lilv_node_is_uri(node)) {
    const char* const string = lilv_node_as_string(node);
    const size_t      length = strlen(string);
    if (!strncmp(string, "file:", 5U)) {
      char* const  path        = lilv_node_get_path(node, NULL);
      const size_t path_length = strlen(path);
      const int    rc =
        any_value_set(any_value, path_length + 1U, forge->Path, path);
      lilv_free(path);
      return rc;
    }

    return any_value_set(any_value, length + 1U, forge->URI, string);
  }

  if (lilv_node_is_string(node)) {
    const char* const string = lilv_node_as_string(node);
    const size_t      length = strlen(string);
    return any_value_set(any_value, length + 1U, forge->String, string);
  }

  if (lilv_node_is_float(node)) {
    const float value = lilv_node_as_float(node);
    return any_value_set(any_value, sizeof(float), forge->Float, &value);
  }

  if (lilv_node_is_int(node)) {
    const int value = lilv_node_as_int(node);
    return any_value_set(any_value, sizeof(int), forge->Int, &value);
  }

  if (lilv_node_is_bool(node)) {
    const bool value = lilv_node_as_bool(node);
    return any_value_set(any_value, sizeof(bool), forge->Bool, &value);
  }

  return 1;
}

const void*
any_value_data(const AnyValue* const any_value)
{
  return (any_value->size > sizeof(void*)) ? any_value->value.pointer
                                           : &any_value->value.number;
}

double
any_value_number(const AnyValue* const       any_value,
                 const LV2_Atom_Forge* const forge)
{
  if (any_value->type == forge->Double) {
    double value = 0.0;
    memcpy(&value, &any_value->value.number, sizeof(double));
    return value;
  }

  if (any_value->type == forge->Float) {
    float value = 0.0f;
    memcpy(&value, &any_value->value.number, sizeof(float));
    return value;
  }

  if (any_value->type == forge->Int || any_value->type == forge->Bool) {
    int32_t value = 0;
    memcpy(&value, &any_value->value.number, sizeof(int32_t));
    return (double)value;
  }

  if (any_value->type == forge->Long) {
    int64_t value = 0;
    memcpy(&value, &any_value->value.number, sizeof(int64_t));
    return (double)value;
  }

  return 0.0;
}
