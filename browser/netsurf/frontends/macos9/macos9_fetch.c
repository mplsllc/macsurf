/*
 * MacSurf — Mac OS 9 frontend for NetSurf
 * macos9_fetch.c — All gui_fetch_table callbacks
 *
 * This file is part of MacSurf, built on the NetSurf engine.
 * Licensed under GPL v2.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "utils/errors.h"
#include "utils/log.h"
#include "netsurf/fetch.h"

#include "macos9/macos9.h"

/* Simple extension-to-MIME mapping */
static const char *
macos9_fetch_filetype(const char *unix_path)
{
	size_t len;
	const char *ext;

	if (unix_path == NULL) {
		return "application/octet-stream";
	}

	len = strlen(unix_path);

	/* Find last dot */
	ext = strrchr(unix_path, '.');
	if (ext == NULL) {
		return "application/octet-stream";
	}
	ext++; /* skip the dot */

	if (strcasecmp(ext, "html") == 0 || strcasecmp(ext, "htm") == 0) {
		return "text/html";
	}
	if (strcasecmp(ext, "css") == 0) {
		return "text/css";
	}
	if (strcasecmp(ext, "js") == 0) {
		return "application/javascript";
	}
	if (strcasecmp(ext, "png") == 0) {
		return "image/png";
	}
	if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0) {
		return "image/jpeg";
	}
	if (strcasecmp(ext, "gif") == 0) {
		return "image/gif";
	}
	if (strcasecmp(ext, "bmp") == 0) {
		return "image/bmp";
	}
	if (strcasecmp(ext, "ico") == 0) {
		return "image/x-icon";
	}
	if (strcasecmp(ext, "svg") == 0) {
		return "image/svg+xml";
	}
	if (strcasecmp(ext, "txt") == 0) {
		return "text/plain";
	}
	if (strcasecmp(ext, "xml") == 0) {
		return "text/xml";
	}

	(void)len;
	return "application/octet-stream";
}

static struct nsurl *
macos9_fetch_get_resource_url(const char *path)
{
	/* TODO: map resource paths to file:/// URLs */
	return NULL;
}

static char *
macos9_fetch_mimetype(const char *ro_path)
{
	const char *type = macos9_fetch_filetype(ro_path);
	return strdup(type);
}

/* Field order: filetype, get_resource_url, get_resource_data,
 * release_resource_data, mimetype, socket_open, socket_close
 * (see include/netsurf/fetch.h) */
struct gui_fetch_table macos9_fetch_table = {
	macos9_fetch_filetype,
	macos9_fetch_get_resource_url,
	NULL,				/* get_resource_data */
	NULL,				/* release_resource_data */
	macos9_fetch_mimetype,
	NULL,				/* socket_open */
	NULL				/* socket_close */
};
