/*
 * MacSurf — Mac OS 9 frontend for NetSurf
 * mac_file_io.c — POSIX file I/O shim using Carbon File Manager
 *
 * This file is part of MacSurf, built on the NetSurf engine.
 * Licensed under GPL v2.
 */

#include "mac_file_io.h"
#include <stdarg.h>
#include <string.h>

#ifdef __MACOS9__
#include <Files.h>
#include <TextUtils.h>
#endif

#define MAC_FD_SLOT_FREE 0
#define MAC_FD_SLOT_USED 1

struct mac_fd_slot {
	int in_use;
#ifdef __MACOS9__
	FSIORefNum refnum;
	FSRef      fsref;
#endif
};

static struct mac_fd_slot fd_table[MAC_FD_TABLE_SIZE];

static int fd_alloc(void)
{
	int i;
	for (i = 0; i < MAC_FD_TABLE_SIZE; i++) {
		if (!fd_table[i].in_use) {
			fd_table[i].in_use = MAC_FD_SLOT_USED;
			return i;
		}
	}
	return -1;
}

static void fd_free(int fd)
{
	if (fd >= 0 && fd < MAC_FD_TABLE_SIZE)
		fd_table[fd].in_use = MAC_FD_SLOT_FREE;
}

#ifdef __MACOS9__

static const HFSUniStr255 dataForkName = { 0, { 0 } };

int mac_open(const char *path, int flags, ...)
{
	FSRef ref;
	FSRef parent_ref;
	OSErr err;
	SInt8 perm;
	FSIORefNum refnum;
	int fd;

	err = FSPathMakeRef((const UInt8 *)path, &ref, NULL);

	if (err != noErr && (flags & O_CREAT)) {
		/* File doesn't exist — create it.
		 * Find parent directory by truncating at last '/'. */
		char parent[1024];
		const char *slash;
		HFSUniStr255 uni_name;
		const char *name;
		size_t name_len;
		size_t i;

		strncpy(parent, path, sizeof(parent) - 1);
		parent[sizeof(parent) - 1] = '\0';

		slash = strrchr(parent, '/');
		if (slash == NULL)
			return -1;

		name = slash + 1;
		name_len = strlen(name);
		parent[slash - parent] = '\0';

		err = FSPathMakeRef((const UInt8 *)parent, &parent_ref, NULL);
		if (err != noErr)
			return -1;

		/* Convert filename to Unicode */
		if (name_len > 255)
			name_len = 255;
		uni_name.length = (UniCharCount)name_len;
		for (i = 0; i < name_len; i++)
			uni_name.unicode[i] = (UniChar)(unsigned char)name[i];

		err = FSCreateFileUnicode(&parent_ref, uni_name.length,
					  uni_name.unicode, kFSCatInfoNone,
					  NULL, &ref, NULL);
		if (err != noErr)
			return -1;
	} else if (err != noErr) {
		return -1;
	}

	/* Determine permission */
	if ((flags & O_RDWR) == O_RDWR)
		perm = fsRdWrPerm;
	else if (flags & O_WRONLY)
		perm = fsWrPerm;
	else
		perm = fsRdPerm;

	err = FSOpenFork(&ref, dataForkName.length, dataForkName.unicode,
			 perm, &refnum);
	if (err != noErr)
		return -1;

	/* Handle O_TRUNC */
	if (flags & O_TRUNC) {
		FSSetForkSize(refnum, fsFromStart, 0);
	}

	fd = fd_alloc();
	if (fd < 0) {
		FSCloseFork(refnum);
		return -1;
	}

	fd_table[fd].refnum = refnum;
	fd_table[fd].fsref = ref;

	return fd;
}

int mac_close(int fd)
{
	OSErr err;

	if (fd < 0 || fd >= MAC_FD_TABLE_SIZE || !fd_table[fd].in_use)
		return -1;

	err = FSCloseFork(fd_table[fd].refnum);
	fd_free(fd);

	return (err == noErr) ? 0 : -1;
}

ssize_t mac_read(int fd, void *buf, size_t count)
{
	ByteCount actual;
	OSErr err;

	if (fd < 0 || fd >= MAC_FD_TABLE_SIZE || !fd_table[fd].in_use)
		return -1;

	err = FSReadFork(fd_table[fd].refnum, fsAtMark, 0,
			 (ByteCount)count, buf, &actual);

	if (err == noErr || err == eofErr)
		return (ssize_t)actual;

	return -1;
}

ssize_t mac_write(int fd, const void *buf, size_t count)
{
	ByteCount actual;
	OSErr err;

	if (fd < 0 || fd >= MAC_FD_TABLE_SIZE || !fd_table[fd].in_use)
		return -1;

	err = FSWriteFork(fd_table[fd].refnum, fsAtMark, 0,
			  (ByteCount)count, buf, &actual);

	if (err == noErr)
		return (ssize_t)actual;

	return -1;
}

int mac_unlink(const char *path)
{
	FSRef ref;
	OSErr err;

	err = FSPathMakeRef((const UInt8 *)path, &ref, NULL);
	if (err != noErr)
		return -1;

	err = FSDeleteObject(&ref);
	return (err == noErr) ? 0 : -1;
}

int mac_fd_get_refnum(int fd, void *out_refnum, void *out_fsref)
{
	if (fd < 0 || fd >= MAC_FD_TABLE_SIZE || !fd_table[fd].in_use)
		return -1;

	if (out_refnum != NULL)
		*(FSIORefNum *)out_refnum = fd_table[fd].refnum;
	if (out_fsref != NULL)
		*(FSRef *)out_fsref = fd_table[fd].fsref;

	return 0;
}

#else /* Linux stubs */

int mac_open(const char *path, int flags, ...)
{
	(void)path;
	(void)flags;
	return -1;
}

int mac_close(int fd)
{
	(void)fd;
	return -1;
}

ssize_t mac_read(int fd, void *buf, size_t count)
{
	(void)fd;
	(void)buf;
	(void)count;
	return -1;
}

ssize_t mac_write(int fd, const void *buf, size_t count)
{
	(void)fd;
	(void)buf;
	(void)count;
	return -1;
}

int mac_unlink(const char *path)
{
	(void)path;
	return -1;
}

int mac_fd_get_refnum(int fd, void *out_refnum, void *out_fsref)
{
	(void)fd;
	(void)out_refnum;
	(void)out_fsref;
	return -1;
}

#endif /* __MACOS9__ */
