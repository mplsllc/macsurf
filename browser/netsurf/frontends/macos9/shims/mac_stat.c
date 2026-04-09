/*
 * MacSurf — Mac OS 9 frontend for NetSurf
 * mac_stat.c — POSIX stat/fstat/access shim using Carbon File Manager
 *
 * This file is part of MacSurf, built on the NetSurf engine.
 * Licensed under GPL v2.
 */

#include "mac_stat.h"
#include <string.h>

/*
 * Ensure struct stat is available — MSL's stat.h provides it on CW8.
 * The prefix file should also include it, but belt-and-suspenders.
 */
#ifdef __MWERKS__
#include <stat.h>
#endif

#ifdef __MACOS9__
#include <Files.h>
#include <DateTimeUtils.h>
#endif

#define MAC_EPOCH_OFFSET_I 2082844800L

#ifdef __MACOS9__

int mac_stat(const char *path, struct stat *buf)
{
	FSRef ref;
	FSCatalogInfo info;
	OSErr err;
	UInt32 mac_secs;

	if (buf == NULL)
		return -1;

	memset(buf, 0, sizeof(*buf));

	err = FSPathMakeRef((const UInt8 *)path, &ref, NULL);
	if (err != noErr)
		return -1;

	err = FSGetCatalogInfo(&ref,
			       kFSCatInfoDataSizes |
			       kFSCatInfoContentMod |
			       kFSCatInfoNodeFlags,
			       &info, NULL, NULL, NULL);
	if (err != noErr)
		return -1;

	/* Size (data fork logical size) */
	buf->st_size = (off_t)info.dataLogicalSize;

	/* Modification time: Mac epoch (1904) → Unix epoch (1970) */
	mac_secs = (UInt32)(info.contentModDate.lowSeconds);
	buf->st_mtime = (long)mac_secs - MAC_EPOCH_OFFSET_I;

	/* Mode: directory vs regular file */
	if (info.nodeFlags & kFSNodeIsDirectoryMask)
		buf->st_mode = S_IFDIR | 0755;
	else
		buf->st_mode = S_IFREG | 0644;

	return 0;
}

int mac_fstat(int fd, struct stat *buf)
{
	FSIORefNum refnum;
	FSRef ref;
	SInt64 fork_size;
	FSCatalogInfo info;
	UInt32 mac_secs;
	OSErr err;

	if (buf == NULL)
		return -1;

	memset(buf, 0, sizeof(*buf));

	if (mac_fd_get_refnum(fd, &refnum, &ref) != 0)
		return -1;

	/* Get fork size */
	err = FSGetForkSize(refnum, &fork_size);
	if (err != noErr)
		return -1;

	buf->st_size = (off_t)fork_size;
	buf->st_mode = S_IFREG | 0644;

	/* Try to get mod time from the FSRef */
	err = FSGetCatalogInfo(&ref,
			       kFSCatInfoContentMod | kFSCatInfoNodeFlags,
			       &info, NULL, NULL, NULL);
	if (err == noErr) {
		mac_secs = (UInt32)(info.contentModDate.lowSeconds);
		buf->st_mtime = (long)mac_secs - MAC_EPOCH_OFFSET_I;

		if (info.nodeFlags & kFSNodeIsDirectoryMask)
			buf->st_mode = S_IFDIR | 0755;
	}

	return 0;
}

int mac_access(const char *path, int mode)
{
	FSRef ref;
	OSErr err;

	(void)mode; /* No Unix permissions on OS 9 */

	err = FSPathMakeRef((const UInt8 *)path, &ref, NULL);
	return (err == noErr) ? 0 : -1;
}

#else /* Linux stubs */

int mac_stat(const char *path, struct stat *buf)
{
	(void)path;
	(void)buf;
	return -1;
}

int mac_fstat(int fd, struct stat *buf)
{
	(void)fd;
	(void)buf;
	return -1;
}

int mac_access(const char *path, int mode)
{
	(void)path;
	(void)mode;
	return -1;
}

#endif /* __MACOS9__ */
