/*
 * Stub nsutils/ns_unistd.h for Mac OS 9 syntax checking.
 * Real libnsutils is not yet built for this target.
 */

#ifndef NSUTILS_UNISTD_H
#define NSUTILS_UNISTD_H

#include <stddef.h>
#include <sys/ns_types.h>

static inline ssize_t nsu_pread(int fd, void *buf, size_t count, off_t offset)
{
	(void)fd; (void)buf; (void)count; (void)offset;
	return -1;
}

static inline ssize_t nsu_pwrite(int fd, const void *buf, size_t count, off_t offset)
{
	(void)fd; (void)buf; (void)count; (void)offset;
	return -1;
}

#endif
