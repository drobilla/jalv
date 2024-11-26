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
