// Copyright 2012-2020 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef ZIX_THREAD_H
#define ZIX_THREAD_H

#include "zix/common.h"

#ifdef _WIN32
#  include <windows.h>
#else
#  include <errno.h>
#  include <pthread.h>
#endif

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
   @addtogroup zix
   @{
   @name Thread
   @{
*/

#ifdef _WIN32
typedef HANDLE ZixThread;
#else
typedef pthread_t ZixThread;
#endif

/**
   Initialize `thread` to a new thread.

   The thread will immediately be launched, calling `function` with `arg`
   as the only parameter.
*/
static inline ZixStatus
zix_thread_create(ZixThread* thread,
                  size_t     stack_size,
                  void* (*function)(void*),
                  void* arg);

/// Join `thread` (block until `thread` exits)
static inline ZixStatus
zix_thread_join(ZixThread thread, void** retval);

#ifdef _WIN32

static inline ZixStatus
zix_thread_create(ZixThread* thread,
                  size_t     stack_size,
                  void* (*function)(void*),
                  void* arg)
{
  *thread = CreateThread(
    NULL, stack_size, (LPTHREAD_START_ROUTINE)function, arg, 0, NULL);
  return *thread ? ZIX_STATUS_SUCCESS : ZIX_STATUS_ERROR;
}

static inline ZixStatus
zix_thread_join(ZixThread thread, void** retval)
{
  (void)retval;

  return WaitForSingleObject(thread, INFINITE) ? ZIX_STATUS_SUCCESS
                                               : ZIX_STATUS_ERROR;
}

#else /* !defined(_WIN32) */

static inline ZixStatus
zix_thread_create(ZixThread* thread,
                  size_t     stack_size,
                  void* (*function)(void*),
                  void* arg)
{
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, stack_size);

  const int ret = pthread_create(thread, NULL, function, arg);
  pthread_attr_destroy(&attr);

  switch (ret) {
  case EAGAIN:
    return ZIX_STATUS_NO_MEM;
  case EINVAL:
    return ZIX_STATUS_BAD_ARG;
  case EPERM:
    return ZIX_STATUS_BAD_PERMS;
  }

  return ret ? ZIX_STATUS_ERROR : ZIX_STATUS_SUCCESS;
}

static inline ZixStatus
zix_thread_join(ZixThread thread, void** retval)
{
  return pthread_join(thread, retval) ? ZIX_STATUS_ERROR : ZIX_STATUS_SUCCESS;
}

#endif

/**
   @}
   @}
*/

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ZIX_THREAD_H */
