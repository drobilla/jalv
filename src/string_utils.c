// Copyright 2007-2022 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#include "string_utils.h"

#include <stdlib.h>
#include <string.h>

char*
jalv_strdup(const char* const str)
{
  const size_t len  = strlen(str);
  char*        copy = (char*)malloc(len + 1);
  memcpy(copy, str, len + 1);
  return copy;
}

char*
jalv_strjoin(const char* const a, const char* const b)
{
  const size_t a_len = strlen(a);
  const size_t b_len = strlen(b);
  char* const  out   = (char*)malloc(a_len + b_len + 1);

  memcpy(out, a, a_len);
  memcpy(out + a_len, b, b_len);
  out[a_len + b_len] = '\0';

  return out;
}
