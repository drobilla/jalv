/*
  Copyright 2017 Timo Wischer <twischer@de.adit-jv.com>

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

#ifndef ZIX_SHM_H
#define ZIX_SHM_H


#ifdef _WIN32
#    include <windows.h>
#else
#    include <stdio.h>
#    include <stdbool.h>
#    include <errno.h>
#    include <fcntl.h>
#    include <unistd.h>
#    include <limits.h>
#    include <string.h>
#    include <sys/file.h>
#    include <sys/stat.h>
#    include <sys/mman.h>
#endif

#include "zix/common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
   @addtogroup zix
   @{
   @name Shared memory
   @{
*/

#ifdef _WIN32
typedef HANDLE ZixShm
#else
typedef struct {
	char name[NAME_MAX];
	int fd;
	bool is_file_locked;
	size_t mapped_size;
	void* shmp;
} ZixShm;
#endif

#ifdef _WIN32

// TODO

#else
static inline ZixStatus zix_shm_init_done(ZixShm* const shm);
static inline ZixStatus zix_shm_destroy(ZixShm* const shm);


/**
 * @brief zix_shm_init
 * @param shm
 * @param name
 * @param mode
 * @param size
 * @param keep_shm_lock true the shm will be locked and has to be unlocked by calling zix_shm_init_done()
 * @return
 */
static inline ZixStatus zix_shm_init(ZixShm* const shm, const char* const name, const ZixMode mode, const size_t size, const char* const group, const bool keep_shm_lock)
{
	shm->fd = -1;
	shm->is_file_locked = false;
	shm->mapped_size = 0;
	shm->shmp = NULL;
	shm->name[0] = 0;

	int oflags = 0;
	int protection = 0;
	mode_t perm = 0;
	if ((mode & ZIX_MODE_RDWR) == ZIX_MODE_RDWR) {
		oflags = O_RDWR;
		protection = PROT_READ | PROT_WRITE;
		/* user rw, group r */
		perm = S_IRUSR | S_IWUSR | S_IRGRP;
	} else if ((mode & ZIX_MODE_RDONLY) == ZIX_MODE_RDONLY) {
		oflags = O_RDONLY;
		protection = PROT_READ;
		/* user r, group w */
		perm = S_IRUSR | S_IWGRP;
	} else if ((mode & ZIX_MODE_WRONLY) == ZIX_MODE_WRONLY) {
		oflags = O_WRONLY;
		protection = PROT_WRITE;
		/* user w, group r */
		perm = S_IWUSR | S_IRGRP;
	} else {
		return ZIX_STATUS_BAD_ARG;
	}

	if ((mode & ZIX_MODE_CREATE) == ZIX_MODE_CREATE) {
		/* fail when try to create but already exists */
		oflags |= O_CREAT | O_EXCL;
	}

	if ((mode & ZIX_MODE_NONBLOCKING) == ZIX_MODE_NONBLOCKING) {
		oflags |= O_NONBLOCK;
	}

	/* reset umask to use provided access rights explicitly */
	const mode_t old_umask = umask(0);
	shm->fd = shm_open(name, oflags, perm);
	umask(old_umask);
	if (shm->fd < 0) {
		if (errno == EEXIST) {
			return ZIX_STATUS_EXISTS;
		}
		perror("shm_open() failed");
		return ZIX_STATUS_ERROR;
	}

	if ((mode & ZIX_MODE_CREATE) == ZIX_MODE_CREATE) {
		/* name is only required for removing the shm file and
		 * the shm file should only be removed by the creator
		 */
		strncpy(shm->name, name, sizeof(shm->name));
	}

	/* lock shm to guarantee that no one is accessing it
	 * before it was initalizes successfully
	 */
	if (flock(shm->fd, LOCK_EX) < 0) {
		perror("flock() locking failed");
		zix_shm_destroy(shm);
		return ZIX_STATUS_ERROR;
	}
	shm->is_file_locked = true;

	if ((mode & ZIX_MODE_CREATE) == ZIX_MODE_CREATE) {
		const ZixStatus ret = zix_group_set(shm->fd, group);
		if (ret != ZIX_STATUS_SUCCESS && ret != ZIX_STATUS_EMPTY) {
			zix_shm_destroy(shm);
			return ZIX_STATUS_ERROR;
		}

		/* round up to page boundary
		 * _SC_PAGE_SIZE must not be less than 1. See manpage sysconf3
		 */
		const size_t psz = sysconf(_SC_PAGE_SIZE);
		shm->mapped_size = zix_round_up(size, psz);

		/* only the creater should resize the shm */
		if (ftruncate(shm->fd, shm->mapped_size) < 0) {
			perror("ftruncate() failed");
			zix_shm_destroy(shm);
			return ZIX_STATUS_ERROR;
		}
	} else {
		struct stat sbuf;
		if (fstat(shm->fd, &sbuf) < 0) {
			perror("fstat() failed");
			zix_shm_destroy(shm);
			return ZIX_STATUS_ERROR;
		}

		if (sbuf.st_size <= 0) {
			ERR("Shared memory '%s' was not yet initalized successfully. Possibly tried to access too early.", name);
			zix_shm_destroy(shm);
			return ZIX_STATUS_EMPTY;
		}

		shm->mapped_size = sbuf.st_size;
	}


	void* const shmp = mmap(NULL, shm->mapped_size, protection, MAP_SHARED, shm->fd, 0);
	if (shmp == MAP_FAILED) {
		perror("mmap() failed");
		zix_shm_destroy(shm);
		return ZIX_STATUS_NO_MEM;
	}

	if ((mode & ZIX_MODE_CREATE) == ZIX_MODE_CREATE) {
		memset(shmp, 0, shm->mapped_size);
	}
	shm->shmp = shmp;

	if (!keep_shm_lock) {
		if (zix_shm_init_done(shm) != ZIX_STATUS_SUCCESS) {
			return ZIX_STATUS_ERROR;
		}
	}

	return ZIX_STATUS_SUCCESS;
}


static inline ZixStatus zix_shm_init_done(ZixShm* const shm)
{
	if (!shm->is_file_locked) {
		ERR("The shared memory '%s' file %d was not locked before. Therefore it is not valid to unlock it now.", shm->name, shm->fd);
		return ZIX_STATUS_ERROR;
	}

	if (flock(shm->fd, LOCK_UN) < 0) {
		perror("flock() unlocking failed");
		return ZIX_STATUS_ERROR;
	}

	shm->is_file_locked = false;

	return ZIX_STATUS_SUCCESS;
}


static inline void* zix_shm_pointer(const ZixShm* const shm)
{
	return shm->shmp;
}


static inline size_t zix_shm_size(const ZixShm* const shm)
{
	return shm->mapped_size;
}


static inline ZixStatus zix_shm_destroy(ZixShm* const shm)
{
	if (!shm)
		return ZIX_STATUS_SUCCESS;

	if (shm->shmp) {
		if (munmap(shm->shmp, shm->mapped_size) < 0) {
			perror("munmap() failed");
		}
		shm->shmp = NULL;
	}
	if (shm->fd >= 0) {
		/* Unlock the file if it is locked.
		 * This can only happen when JALV (the creator of the shm)
		 * destroys the SHM before zix_shm_init_done() was called.
		 */
		if (shm->is_file_locked) {
			zix_shm_init_done(shm);
		}

		if (close(shm->fd) < 0) {
			perror("close() failed");
		}
	}
	shm->fd = -1;

	/* only unlink if we were the creator.
	 * In an error case it is possible
	 * that the file was created
	 * but it was not opened successfully.
	 * Therefore the file has to be deleted
	 * independend of the file descriptor
	 */
	if (shm->name[0] != 0) {
		if (shm_unlink(shm->name) < 0) {
			perror("shm_unlink() failed");
		}
	}
	shm->name[0] = 0;

	return ZIX_STATUS_SUCCESS;
}

#endif

/**
   @endcond
   @}
   @}
*/

#endif // ZIX_SHM_H

