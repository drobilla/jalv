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

#ifndef ZIX_SHQ_H
#define ZIX_SHQ_H


#ifdef _WIN32
#    include <windows.h>
#else
#include <stdio.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#endif

#include "zix/common.h"

#ifdef __cplusplus
extern "C" {
#endif


#ifdef _WIN32

// TODO

#else

typedef struct {
	char name[PATH_MAX];
	int fd;
} ZixShq;


static inline void zix_shq_destroy(ZixShq* const shq);


static inline void zix_shq_clear(ZixShq* const shq)
{
	memset(shq, 0, sizeof(*shq));
	shq->fd = -1;
}


static inline ZixStatus zix_shq_init(ZixShq* const shq, const char* const name, const ZixMode mode, const char* const group)
{
	zix_shq_clear(shq);


	mode_t perm = 0;
	int oflags = 0;
	if ((mode & ZIX_MODE_RDWR) == ZIX_MODE_RDWR) {
		oflags = O_RDWR;
		/* user rw, group r */
		perm = 0640;
	} else if ((mode & ZIX_MODE_RDONLY) == ZIX_MODE_RDONLY) {
		oflags = O_RDONLY;
		/* user r, group rw , polling needs read perm so group is given rw*/
		perm = 0460;
	} else if ((mode & ZIX_MODE_WRONLY) == ZIX_MODE_WRONLY) {
		oflags = O_WRONLY;
		/* user w, group r */
		perm = 0240;
	} else {
		zix_shq_destroy(shq);
		return ZIX_STATUS_BAD_ARG;
	}

	STRCAT(shq_path, ZIX_SHM_PATH, name);

	if ((mode & ZIX_MODE_CREATE) == ZIX_MODE_CREATE) {
		/* reset umask to use provided access rights explicitly */
		const mode_t old_umask = umask(0);
		const int err = mkfifo(shq_path, perm);
		umask(old_umask);

		if (err < 0) {
			perror("mkfifo() failed");
			zix_shq_destroy(shq);
			return ZIX_STATUS_ERROR;
		}
		/* name is only required for removing the shq file and
		 * the shq file should only be removed by the creator
		 */
		strncpy(shq->name, shq_path, sizeof(shq->name));
	}



	if ((mode & ZIX_MODE_NONBLOCKING) == ZIX_MODE_NONBLOCKING) {
		oflags |= O_NONBLOCK;
	}

	shq->fd = open(shq_path, oflags);
	if (shq->fd < 0) {
		perror("shq_open() failed");
		zix_shq_destroy(shq);
		return ZIX_STATUS_ERROR;
	}

	if ((mode & ZIX_MODE_CREATE) == ZIX_MODE_CREATE) {
		const ZixStatus ret = zix_group_set(shq->fd, group);
		if (ret != ZIX_STATUS_SUCCESS && ret != ZIX_STATUS_EMPTY) {
			zix_shq_destroy(shq);
			return ZIX_STATUS_ERROR;
		}
	}

	return ZIX_STATUS_SUCCESS;
}


static inline bool zix_shq_valid(const ZixShq* const shq)
{
	return (shq->fd >= 0);
}


static inline int zix_shq_handle(const ZixShq* const shq)
{
	return shq->fd;
}


static inline ZixStatus zix_shq_write(const ZixShq* const shq, const void* const data, const size_t size)
{
	if (!zix_shq_valid(shq)) {
		return ZIX_STATUS_BAD_ARG;
	}

	if (size > PIPE_BUF) {
		ERR("write() of %lu bytes is possibly not atomic. Writes of more than %u bytes are not guaranteed to be atomic", size, PIPE_BUF);
		return ZIX_STATUS_BAD_ARG;
	}

	const ssize_t written_bytes = write(shq->fd, data, size);
	if (written_bytes < 0) {
		perror("write() failed");
		return ZIX_STATUS_ERROR;
	} else if ((size_t)written_bytes != size) {
		ERR("write(%s, %lu) failed. Less (%ld) bytes written\n", shq->name, size, written_bytes);
		return ZIX_STATUS_ERROR;
	}

	return ZIX_STATUS_SUCCESS;
}


static inline ZixStatus zix_shq_wait_for_internal(const ZixShq* const shq, const short int events, const int timeout)
{
	struct pollfd fds;
	fds.fd = shq->fd;
	fds.events = events;
	const int ret = poll(&fds, 1, timeout);
	if (ret < 0) {
		perror("poll() failed");
		return ZIX_STATUS_ERROR;
	} else if (ret == 0) {
		return ZIX_STATUS_EMPTY;
	}

	if (fds.revents & events) {
		return ZIX_STATUS_SUCCESS;
	} else {
		/* There is a poll event but not the requested one.
		 * Therefore it could only be an error poll event
		 */
		return ZIX_STATUS_ERROR;
	}

}


static inline ZixStatus zix_shq_wait_for_data(const ZixShq* const shq, const int timeout)
{
	return zix_shq_wait_for_internal(shq, POLLIN, timeout);
}

static inline ZixStatus zix_shq_wait_for_closed(const ZixShq* const shq, const int timeout)
{
	return zix_shq_wait_for_internal(shq, POLLHUP, timeout);
}


static inline ZixStatus zix_shq_read(const ZixShq* const shq, void* const data, const size_t size)
{
	if (!zix_shq_valid(shq)) {
		ERR("The used shared queue is not valid (fd %d data %p size %lu)", shq->fd, data, size);
		return ZIX_STATUS_BAD_ARG;
	}

	if (size > PIPE_BUF) {
		ERR("read() of %lu bytes is possibly not atomic. Reads of more than %u bytes are not guaranteed to be atomic", size, PIPE_BUF);
		return ZIX_STATUS_BAD_ARG;
	}

	const ssize_t read_bytes = read(shq->fd, data, size);
	if (read_bytes < 0) {
		if (errno == EAGAIN) {
			return ZIX_STATUS_EMPTY;
		}
		perror("read() failed");
		return ZIX_STATUS_ERROR;
	} else if (read_bytes == 0) {
		/* The opposite side FIFO is not open */
		return ZIX_STATUS_UNAVAILABLE;
	} else if ((size_t)read_bytes != size) {
		ERR("read(%s, %lu) failed. Less (%ld) bytes read\n", shq->name, size, read_bytes);
		return ZIX_STATUS_ERROR;
	}

	return ZIX_STATUS_SUCCESS;
}


static inline void zix_shq_destroy(ZixShq* const shq)
{
	if (!shq)
		return;

	if (shq->fd >= 0) {
		if (close(shq->fd) < 0) {
			perror("close() failed");
		}
	}

	/* only unlink if we were the creator */
	if (shq->name[0] != 0) {
		if (unlink(shq->name) < 0) {
			perror("shm_unlink() failed");
		}
	}

	zix_shq_clear(shq);
}

#endif

#endif // ZIX_SHQ_H

