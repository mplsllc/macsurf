/*
 * MacSurf — Mac OS 9 frontend for NetSurf
 * mac_dirent.c — POSIX directory iteration shim using Carbon File Manager
 *
 * This file is part of MacSurf, built on the NetSurf engine.
 * Licensed under GPL v2.
 */

#include "mac_dirent.h"
#include <stdlib.h>
#include <string.h>

#ifdef __MACOS9__
#include <Files.h>
#include <TextEncodingConverter.h>
#endif

struct mac_dir {
#ifdef __MACOS9__
	FSIterator	iterator;
	FSRef		parent_ref;
#endif
	struct mac_dirent entry;
	int		exhausted;
};

#ifdef __MACOS9__

/*
 * Convert HFSUniStr255 to UTF-8 via TextEncoding Converter.
 * Returns 0 on success, -1 on failure.
 */
static int uni_to_utf8(const HFSUniStr255 *uni, char *out, size_t out_size)
{
	TECObjectRef converter;
	TextEncoding utf16_enc, utf8_enc;
	ByteCount src_read, dst_written;
	OSStatus err;

	utf16_enc = CreateTextEncoding(kTextEncodingUnicodeDefault,
				       kTextEncodingDefaultVariant,
				       kUnicode16BitFormat);
	utf8_enc  = CreateTextEncoding(kTextEncodingUnicodeDefault,
				       kTextEncodingDefaultVariant,
				       kUnicodeUTF8Format);

	err = TECCreateConverter(&converter, utf16_enc, utf8_enc);
	if (err != noErr)
		return -1;

	err = TECConvertText(converter,
			     (ConstTextPtr)uni->unicode,
			     uni->length * sizeof(UniChar),
			     &src_read,
			     (TextPtr)out,
			     (ByteCount)(out_size - 1),
			     &dst_written);

	TECDisposeConverter(converter);

	if (err != noErr && err != kTECOutputBufferFullStatus)
		return -1;

	out[dst_written] = '\0';
	return 0;
}

mac_DIR *mac_opendir(const char *path)
{
	FSRef ref;
	FSIterator iterator;
	OSErr err;
	mac_DIR *dir;

	err = FSPathMakeRef((const UInt8 *)path, &ref, NULL);
	if (err != noErr)
		return NULL;

	err = FSOpenIterator(&ref, kFSIterateFlat, &iterator);
	if (err != noErr)
		return NULL;

	dir = malloc(sizeof(*dir));
	if (dir == NULL) {
		FSCloseIterator(iterator);
		return NULL;
	}

	dir->iterator = iterator;
	dir->parent_ref = ref;
	dir->exhausted = 0;
	memset(&dir->entry, 0, sizeof(dir->entry));

	return dir;
}

struct mac_dirent *mac_readdir(mac_DIR *dir)
{
	FSCatalogInfo info;
	FSRef entry_ref;
	HFSUniStr255 uni_name;
	ItemCount actual_count;
	OSErr err;

	if (dir == NULL || dir->exhausted)
		return NULL;

	err = FSGetCatalogInfoBulk(dir->iterator, 1, &actual_count, NULL,
				   kFSCatInfoNodeFlags,
				   &info, &entry_ref, NULL, &uni_name);

	if (err == errFSNoMoreItems || actual_count == 0) {
		dir->exhausted = 1;
		return NULL;
	}

	if (err != noErr && err != errFSNoMoreItems)
		return NULL;

	/* Convert name to UTF-8 */
	if (uni_to_utf8(&uni_name, dir->entry.d_name,
			sizeof(dir->entry.d_name)) != 0)
		return NULL;

	/* Set entry type */
	if (info.nodeFlags & kFSNodeIsDirectoryMask)
		dir->entry.d_type = DT_DIR;
	else
		dir->entry.d_type = DT_REG;

	return &dir->entry;
}

int mac_closedir(mac_DIR *dir)
{
	OSErr err;

	if (dir == NULL)
		return -1;

	err = FSCloseIterator(dir->iterator);
	free(dir);

	return (err == noErr) ? 0 : -1;
}

#else /* Linux stubs */

mac_DIR *mac_opendir(const char *path)
{
	(void)path;
	return NULL;
}

struct mac_dirent *mac_readdir(mac_DIR *dir)
{
	(void)dir;
	return NULL;
}

int mac_closedir(mac_DIR *dir)
{
	(void)dir;
	return 0;
}

#endif /* __MACOS9__ */
