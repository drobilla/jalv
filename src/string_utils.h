// Copyright 2007-2022 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef JALV_STRING_UTILS_H
#define JALV_STRING_UTILS_H

#include "attributes.h"

JALV_BEGIN_DECLS

/// Return a newly allocated copy of a string
char*
jalv_strdup(const char* str);

/// Return a newly allocated concatenation of two strings
char*
jalv_strjoin(const char* a, const char* b);

JALV_END_DECLS

#endif // JALV_STRING_UTILS_H
