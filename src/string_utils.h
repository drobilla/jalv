// Copyright 2007-2022 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_STRING_UTILS_H
#define JALV_STRING_UTILS_H

#include "attributes.h"

// String utilities
JALV_BEGIN_DECLS

/// Return a newly allocated copy of a string
char*
jalv_strdup(const char* str);

JALV_END_DECLS

#endif // JALV_STRING_UTILS_H
