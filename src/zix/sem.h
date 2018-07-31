/*
  Copyright 2012-2014 David Robillard <http://drobilla.net>

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

#ifndef ZIX_SEM_H
#define ZIX_SEM_H

#ifdef __APPLE__
#    include <mach/mach.h>
#elif defined(_WIN32)
#    include <limits.h>
#    include <windows.h>
#else
#    include <stdio.h>
#    include <sys/stat.h>
#    include <semaphore.h>
#    include <errno.h>
#    include <limits.h>
#    include <string.h>
#    include <fcntl.h>
#endif


#include "zix/common.h"

#ifdef _WIN32
// TODO
#else
// TODO check if valid for OSX
#define ZIX_SEM_NAME (ZIX_SHM_PATH"sem.")
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
   @addtogroup zix
   @{
   @name Semaphore
   @{
*/

/**
   A counting semaphore.

   This is an integer that is always positive, and has two main operations:
   increment (post) and decrement (wait).  If a decrement can not be performed
   (i.e. the value is 0) the caller will be blocked until another thread posts
   and the operation can succeed.

   Semaphores can be created with any starting value, but typically this will
   be 0 so the semaphore can be used as a simple signal where each post
   corresponds to one wait.

   Semaphores are very efficient (much moreso than a mutex/cond pair).  In
   particular, at least on Linux, post is async-signal-safe, which means it
   does not block and will not be interrupted.  If you need to signal from
   a realtime thread, this is the most appropriate primitive to use.
*/
typedef struct ZixSemImpl ZixSem;

/**
   Create and initialize `sem` to `initial`.
*/
static inline ZixStatus
zix_sem_init(ZixSem* sem, unsigned initial);

/**
   Destroy `sem`.
*/
static inline void
zix_sem_destroy(ZixSem* sem);

/**
   Increment (and signal any waiters).
   Realtime safe.
*/
static inline void
zix_sem_post(ZixSem* sem);

/**
   Wait until count is > 0, then decrement.
   Obviously not realtime safe.
*/
static inline ZixStatus
zix_sem_wait(ZixSem* sem);

/**
   Non-blocking version of wait().

   @return true if decrement was successful (lock was acquired).
*/
static inline bool
zix_sem_try_wait(ZixSem* sem);

/**
   @cond
*/

#ifdef __APPLE__

struct ZixSemImpl {
	semaphore_t sem;
};

static inline ZixStatus
zix_sem_init(ZixSem* sem, unsigned val)
{
	return semaphore_create(mach_task_self(), &sem->sem, SYNC_POLICY_FIFO, val)
		? ZIX_STATUS_ERROR : ZIX_STATUS_SUCCESS;
}

static inline void
zix_sem_destroy(ZixSem* sem)
{
	semaphore_destroy(mach_task_self(), sem->sem);
}

static inline void
zix_sem_post(ZixSem* sem)
{
	semaphore_signal(sem->sem);
}

static inline ZixStatus
zix_sem_wait(ZixSem* sem)
{
	if (semaphore_wait(sem->sem) != KERN_SUCCESS) {
		return ZIX_STATUS_ERROR;
	}
	return ZIX_STATUS_SUCCESS;
}

static inline bool
zix_sem_try_wait(ZixSem* sem)
{
	const mach_timespec_t zero = { 0, 0 };
	return semaphore_timedwait(sem->sem, zero) == KERN_SUCCESS;
}

#elif defined(_WIN32)

struct ZixSemImpl {
	HANDLE sem;
};

static inline ZixStatus
zix_sem_init(ZixSem* sem, unsigned initial)
{
	sem->sem = CreateSemaphore(NULL, initial, LONG_MAX, NULL);
	return (sem->sem) ? ZIX_STATUS_ERROR : ZIX_STATUS_SUCCESS;
}

static inline void
zix_sem_destroy(ZixSem* sem)
{
	CloseHandle(sem->sem);
}

static inline void
zix_sem_post(ZixSem* sem)
{
	ReleaseSemaphore(sem->sem, 1, NULL);
}

static inline ZixStatus
zix_sem_wait(ZixSem* sem)
{
	if (WaitForSingleObject(sem->sem, INFINITE) != WAIT_OBJECT_0) {
		return ZIX_STATUS_ERROR;
	}
	return ZIX_STATUS_SUCCESS;
}

static inline bool
zix_sem_try_wait(ZixSem* sem)
{
	return WaitForSingleObject(sem->sem, 0) == WAIT_OBJECT_0;
}

#else  /* !defined(__APPLE__) && !defined(_WIN32) */

struct ZixSemImpl {
	bool is_valid;
	sem_t sem;
	sem_t* psem;
	/* only set if we are the creator */
	char name[PATH_MAX-3];
};


static inline void
zix_sem_clear(ZixSem* const sem)
{
	memset(sem, 0, sizeof(*sem));
}

static inline ZixStatus
zix_sem_create_internal(ZixSem* sem, const char* const name, const int oflag, const mode_t mode, unsigned initial, const char* group)
{
	gid_t gid = -1;

	zix_sem_clear(sem);

	char name_with_slash[sizeof(sem->name)] = "/";
	char group_ch_name[sizeof(sem->name) + strlen(ZIX_SEM_NAME)];
	strncpy(group_ch_name, ZIX_SEM_NAME, sizeof(group_ch_name));
	group_ch_name[sizeof(group_ch_name) - 1] = 0;
	strncat(group_ch_name, name, (sizeof(group_ch_name) - strlen(ZIX_SEM_NAME) - 1));
	const size_t space = sizeof(name_with_slash) - strlen(name_with_slash);
	strncat(name_with_slash, name, space);

	if (oflag & O_CREAT) {
		const ZixStatus ret = zix_group_id(group, &gid);
		if (ret != ZIX_STATUS_SUCCESS && ret != ZIX_STATUS_EMPTY) {
			return ret;
		}
	}


	/* reset umask to use provided access rights explicitly */
	const mode_t old_umask = umask(0);
	sem->psem = sem_open(name_with_slash, oflag, mode, initial);
	umask(old_umask);


	if (!sem->psem) {
		return ZIX_STATUS_ERROR;
	}

	sem->is_valid = true;

	if (oflag & O_CREAT) {
		strncpy(sem->name, name_with_slash, sizeof(sem->name));
		sem->name[sizeof(sem->name) - 1] = 0;
		if (chown(group_ch_name, (uid_t)-1, gid) < 0) {
			perror("chown() failed");
			return ZIX_STATUS_ERROR;
		}
	}

	return ZIX_STATUS_SUCCESS;
}

static inline ZixStatus
zix_sem_create(ZixSem* sem, const char* const name, unsigned initial, const char* group)
{
	return zix_sem_create_internal(sem, name, O_CREAT, (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP), initial, group);
}

static inline ZixStatus
zix_sem_open(ZixSem* sem, const char* const name)
{
	return zix_sem_create_internal(sem, name, 0, 0, 0, NULL);
}

static inline bool
zix_sem_valid(ZixSem* sem)
{
	if (!sem)
		return false;

	return sem->is_valid;
}

static inline ZixStatus
zix_sem_init(ZixSem* sem, unsigned initial)
{
	zix_sem_clear(sem);

	if (sem_init(&sem->sem, 0, initial) < 0) {
		perror("sem_init() failed");
		return ZIX_STATUS_ERROR;
	}
	sem->is_valid = true;

	return ZIX_STATUS_SUCCESS;
}

static inline void
zix_sem_destroy(ZixSem* sem)
{
	if (!zix_sem_valid(sem)) {
		return;
	}

	if (sem->psem) {
		sem_close(sem->psem);
		if (sem->name[0] != 0) {
			sem_unlink(sem->name);
		}
	} else {
		sem_destroy(&sem->sem);
	}

	zix_sem_clear(sem);
}

/**
 * @brief zix_sem_pointer only for internal use
 * @param sem
 * @return
 */
static inline sem_t*
zix_sem_pointer_internal(ZixSem* const sem)
{
	return sem->psem ? sem->psem : &sem->sem;
}

static inline void
zix_sem_post(ZixSem* sem)
{
	sem_post( zix_sem_pointer_internal(sem) );
}

static inline ZixStatus
zix_sem_wait(ZixSem* sem)
{
	while (  sem_wait( zix_sem_pointer_internal(sem) )  ) {
		if (errno != EINTR) {
			return ZIX_STATUS_ERROR;
		}
		/* Otherwise, interrupted, so try again. */
	}

	return ZIX_STATUS_SUCCESS;
}

static inline bool
zix_sem_try_wait(ZixSem* sem)
{
	return (  sem_trywait( zix_sem_pointer_internal(sem) ) == 0  );
}

#endif

/**
   @endcond
   @}
   @}
*/

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* ZIX_SEM_H */
