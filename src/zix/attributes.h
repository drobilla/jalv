// Copyright 2021 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef ZIX_ATTRIBUTES_H
#define ZIX_ATTRIBUTES_H

/**
   @addtogroup zix
   @{
*/

// ZIX_API must be used to decorate things in the public API
#ifndef ZIX_API
#  if defined(_WIN32) && !defined(ZIX_STATIC) && defined(ZIX_INTERNAL)
#    define ZIX_API __declspec(dllexport)
#  elif defined(_WIN32) && !defined(ZIX_STATIC)
#    define ZIX_API __declspec(dllimport)
#  elif defined(__GNUC__)
#    define ZIX_API __attribute__((visibility("default")))
#  else
#    define ZIX_API
#  endif
#endif

// GCC pure/const/malloc attributes
#ifdef __GNUC__
#  define ZIX_PURE_FUNC __attribute__((pure))
#  define ZIX_CONST_FUNC __attribute__((const))
#  define ZIX_MALLOC_FUNC __attribute__((malloc))
#else
#  define ZIX_PURE_FUNC
#  define ZIX_CONST_FUNC
#  define ZIX_MALLOC_FUNC
#endif

#define ZIX_PURE_API \
  ZIX_API            \
  ZIX_PURE_FUNC

#define ZIX_CONST_API \
  ZIX_API             \
  ZIX_CONST_FUNC

#define ZIX_MALLOC_API \
  ZIX_API              \
  ZIX_MALLOC_FUNC

// Printf-like format functions
#ifdef __GNUC__
#  define ZIX_LOG_FUNC(fmt, arg1) __attribute__((format(printf, fmt, arg1)))
#else
#  define ZIX_LOG_FUNC(fmt, arg1)
#endif

// Unused parameter macro to suppresses warnings and make it impossible to use
#if defined(__cplusplus)
#  define ZIX_UNUSED(name)
#elif defined(__GNUC__)
#  define ZIX_UNUSED(name) name##_unused __attribute__((__unused__))
#elif defined(_MSC_VER)
#  define ZIX_UNUSED(name) __pragma(warning(suppress : 4100)) name
#else
#  define ZIX_UNUSED(name) name
#endif

// Clang nullability annotations
#if defined(__clang__) && __clang_major__ >= 7
#  define ZIX_NONNULL _Nonnull
#  define ZIX_NULLABLE _Nullable
#  define ZIX_ALLOCATED _Null_unspecified
#else
#  define ZIX_NONNULL
#  define ZIX_NULLABLE
#  define ZIX_ALLOCATED
#endif

/**
   @}
*/

#endif /* ZIX_ATTRIBUTES_H */
