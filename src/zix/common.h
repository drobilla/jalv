/*
  Copyright 2016 David Robillard <http://drobilla.net>

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

#ifndef ZIX_COMMON_H
#define ZIX_COMMON_H

#ifdef _WIN32
#    include <windows.h>
#else
#    include <stdio.h>
#    include <stdlib.h>
#    include <string.h>
#    include <unistd.h>
#    include <limits.h>
#    include <time.h>
#    include <ctype.h>
#    include <grp.h>
#    include <errno.h>
#endif

/**
   @addtogroup zix
   @{
*/

/** @cond */
#ifdef ZIX_SHARED
#    ifdef _WIN32
#        define ZIX_LIB_IMPORT __declspec(dllimport)
#        define ZIX_LIB_EXPORT __declspec(dllexport)
#    else
#        define ZIX_LIB_IMPORT __attribute__((visibility("default")))
#        define ZIX_LIB_EXPORT __attribute__((visibility("default")))
#    endif
#    ifdef ZIX_INTERNAL
#        define ZIX_API ZIX_LIB_EXPORT
#    else
#        define ZIX_API ZIX_LIB_IMPORT
#    endif
#    define ZIX_PRIVATE static
#elif defined(ZIX_INLINE)
#    define ZIX_API     static inline
#    define ZIX_PRIVATE static inline
#else
#    define ZIX_API
#    define ZIX_PRIVATE static
#endif
/** @endcond */

#ifdef _WIN32
// TODO
#else
// TODO check if valid for OSX
#define ZIX_SHM_PATH "/dev/shm/"
#endif

#ifdef __cplusplus
extern "C" {
#else
#    include <stdbool.h>
#endif

#ifdef __GNUC__
#define ZIX_UNUSED  __attribute__((__unused__))
#else
#define ZIX_UNUSED
#endif


#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif

#define STRCAT(DEST, STR1, STR2) \
	const size_t DEST##str1_size = strlen(STR1); \
	const size_t DEST##_size = strlen(STR1) + strlen(STR2) + 1; \
	char DEST[DEST##_size]; \
	strncpy(DEST, (STR1), DEST##_size); \
	DEST[DEST##_size - 1] = 0; \
	strncat(DEST, (STR2), DEST##_size - DEST##str1_size - 1);


typedef enum {
	ZIX_MODE_RDONLY = (1<<0),
	ZIX_MODE_WRONLY = (1<<1),
	ZIX_MODE_RDWR = (ZIX_MODE_RDONLY | ZIX_MODE_WRONLY),
	ZIX_MODE_CREATE = (1<<2),
	ZIX_MODE_NONBLOCKING = (1<<3),
} ZixMode;

typedef enum {
	ZIX_STATUS_SUCCESS,
	ZIX_STATUS_ERROR,
	ZIX_STATUS_NO_MEM,
	ZIX_STATUS_NOT_FOUND,
	ZIX_STATUS_EXISTS,
	ZIX_STATUS_BAD_ARG,
	ZIX_STATUS_BAD_PERMS,
	ZIX_STATUS_EMPTY,
	ZIX_STATUS_UNAVAILABLE,
} ZixStatus;

static inline const char*
zix_strerror(const ZixStatus status)
{
	switch (status) {
	case ZIX_STATUS_SUCCESS:
		return "Success";
	case ZIX_STATUS_ERROR:
		return "Unknown error";
	case ZIX_STATUS_NO_MEM:
		return "Out of memory";
	case ZIX_STATUS_NOT_FOUND:
		return "Not found";
	case ZIX_STATUS_EXISTS:
		return "Exists";
	case ZIX_STATUS_BAD_ARG:
		return "Bad argument";
	case ZIX_STATUS_BAD_PERMS:
		return "Bad permissions";
	case ZIX_STATUS_EMPTY:
		return "empty";
	case ZIX_STATUS_UNAVAILABLE:
		return "Unavailable";
	}
	return "Unknown error";
}


typedef unsigned long long int ZixTime;
static inline ZixTime zix_time_get_us(void);

#define INFO(FORMAT, ...) \
{ \
	const ZixTime us = zix_time_get_us(); \
	const ZixTime us2sec = (ZixTime)1e6; \
	fprintf(stderr, "[%lld.%06lld %s:%u] " FORMAT "\n", us/us2sec, us%us2sec, __func__, __LINE__, __VA_ARGS__); \
}

#define ERR(FORMAT, ...) INFO("ERR: " FORMAT, __VA_ARGS__)

#ifdef DEBUG
#define DBG INFO
#else
#define DBG(...)
#endif


/**
   Function for comparing two elements.
*/
typedef int (*ZixComparator)(const void* a, const void* b, void* user_data);

/**
   Function for testing equality of two elements.
*/
typedef bool (*ZixEqualFunc)(const void* a, const void* b);

/**
   Function to destroy an element.
*/
typedef void (*ZixDestroyFunc)(void* ptr);



static inline int zix_pid(void)
{
#ifdef WIN32
	return _getpid();
#else
	return getpid();
#endif
}

static inline size_t zix_round_up(const size_t value, const size_t boundary)
{
	const size_t elem_count = (value + boundary - 1) / boundary;
	return elem_count * boundary;
}


#ifndef _WIN32
static inline ZixStatus
zix_group_id(const char* const group, gid_t* const gid)
{
	*gid = -1;

	if (!group || strlen(group) == 0) {
		return ZIX_STATUS_EMPTY;
	}

	if (isdigit(*group)) {
		const long gid_long = atol(group);
		if (gid_long < 0 || gid_long > UINT_MAX) {
			ERR("Group '%s' was converted to group ID %ld. This is an invalid ID.", group, gid_long);
			return ZIX_STATUS_ERROR;
		}
		*gid = gid_long;
	} else {
		/* as mentioned in the manpage of getgrnam()
		 * errno has to be set to 0 when reading it
		 */
		errno = 0;
		const struct group* const g = getgrnam(group);
		if (!g) {
			perror("getgrnam() failed");
			return ZIX_STATUS_ERROR;
		}
		*gid = g->gr_gid;
	}

	return ZIX_STATUS_SUCCESS;
}
#endif

static inline ZixStatus
zix_group_set(const int fd, const char* const group)
{
#ifdef _WIN32
	INFO("User groups are not supported for this operating system. (fd %d, group %s)", fd, group);
#else
	gid_t gid = -1;
	ZixStatus ret = zix_group_id(group, &gid);
	if (ret != ZIX_STATUS_SUCCESS)
		return ret;

	if (fchown(fd, (uid_t)-1, gid) < 0) {
		perror("fchown() failed");
		return ZIX_STATUS_ERROR;
	}
#endif

	return ZIX_STATUS_SUCCESS;
}


static inline ZixTime
zix_time_get_us()
{
#ifdef WIN32
	// TODO not yet implemented
	return 0;
#else
	struct timespec time;

	if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) {
		perror("clock_gettime() failed");
		return 0;
	}

	return (ZixTime)time.tv_sec * 1e6 +
		   (ZixTime)time.tv_nsec / 1e3;
#endif
}

static inline void
zix_sleep_us(const unsigned int us)
{
#ifdef WIN32
	// TODO not yet implemented
	ERR("Not yet implemented (%u us)", us);
#else
	/* needs gnu11 */
	usleep(us);
#endif
}


/**
   @}
*/

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* ZIX_COMMON_H */
