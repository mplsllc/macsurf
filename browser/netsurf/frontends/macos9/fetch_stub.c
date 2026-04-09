/*
 * MacSurf — fetch_stub.c
 * Stub implementations for NetSurf fetch subsystem.
 * Licensed under GPL v2.
 */

#include <stddef.h>
#include "utils/errors.h"

struct nsurl;
struct fetch;
struct fetch_multipart_data;
struct llcache_handle;

/* fetch core */
void *fetch_start(struct nsurl *url, void *referer,
		void (*callback)(int msg, struct nsurl *url,
			const void *data, unsigned long len, void *pw),
		void *pw, int only_2xx, const char *post_urlenc,
		const struct fetch_multipart_data *post_multipart,
		int verifiable, struct nsurl *parent,
		const char *content_type_override)
{
	return NULL;
}

int fetch_can_fetch(struct nsurl *url) { return 0; }
long fetch_http_code(void *f) { return 0; }
void fetch_quit(void) {}
void fetcher_quit(void) {}

/* multipart data */
struct fetch_multipart_data *fetch_multipart_data_new_kv(
		const char *name, const char *value)
{
	return NULL;
}

struct fetch_multipart_data *fetch_multipart_data_find(
		struct fetch_multipart_data *list, const char *name)
{
	return NULL;
}

struct fetch_multipart_data *fetch_multipart_data_clone(
		const struct fetch_multipart_data *list)
{
	return NULL;
}

void fetch_multipart_data_destroy(struct fetch_multipart_data *list) {}
