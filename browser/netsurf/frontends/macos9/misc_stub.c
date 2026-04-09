/*
 * MacSurf — misc_stub.c
 * Stub implementations for miscellaneous NetSurf subsystems
 * not needed on Mac OS 9.
 * Licensed under GPL v2.
 */

#include <stddef.h>
#include "utils/errors.h"

struct nsurl;
struct bitmap;
struct cert_chain;
struct download_context;
struct gui_download_window;
struct llcache_handle;
struct image_cache_parameters;

/* Hotlist / global history */
nserror hotlist_update_url(struct nsurl *url) { return NSERROR_OK; }
nserror global_history_add(struct nsurl *url) { return NSERROR_OK; }

/* Content text search */
void content_textsearch_destroy(void *ts) {}

/* Image cache */
nserror image_cache_init(const struct image_cache_parameters *p)
{
	return NSERROR_OK;
}

nserror image_cache_fini(void) { return NSERROR_OK; }

/* Page info */
nserror page_info_fini(void) { return NSERROR_OK; }

/* System colours */
nserror ns_system_colour_init(void) { return NSERROR_OK; }
void ns_system_colour_finalize(void) {}

/* User agent */
void free_user_agent_string(void) {}

/* Search web */
nserror search_web_init(const char *provider) { return NSERROR_OK; }
void search_web_finalise(void) {}

/* Certificate chains */
nserror cert_chain_alloc(unsigned long n, struct cert_chain **chain)
{
	*chain = NULL;
	return NSERROR_OK;
}

nserror cert_chain_free(struct cert_chain *chain) { return NSERROR_OK; }

nserror cert_chain_to_query(struct cert_chain *chain, struct nsurl *url,
		struct nsurl **query_url)
{
	*query_url = NULL;
	return NSERROR_NOT_FOUND;
}

/* Download context */
nserror download_context_create(struct llcache_handle *llcache,
		struct gui_download_window *window)
{
	return NSERROR_OK;
}

void download_context_destroy(void *ctx) {}

/* DOM namespace */
nserror dom_namespace_initialise(void) { return NSERROR_OK; }
void dom_namespace_finalise(void) {}

/* lwc iteration */
void lwc_iterate_strings(void (*cb)(void *str, void *pw), void *pw) {}

/* Page info */
nserror page_info_init(void) { return NSERROR_OK; }

/* Certificate chain */
unsigned long cert_chain_size(const struct cert_chain *chain)
{
	(void)chain;
	return 0;
}

nserror cert_chain_dup(const struct cert_chain *src,
		struct cert_chain **dst_out)
{
	(void)src;
	*dst_out = NULL;
	return NSERROR_OK;
}

/* Fetch */
void fetch_abort(void *f) { (void)f; }
nserror fetcher_init(void) { return NSERROR_OK; }

/* Content handler init stubs */
nserror textplain_init(void) { return NSERROR_OK; }
nserror image_init(void) { return NSERROR_OK; }
nserror html_init(void) { return NSERROR_OK; }
nserror nscss_init(void) { return NSERROR_OK; }

/* System colours */
nserror nscolour_update(void) { return NSERROR_OK; }

/* IDNA */
nserror idna_encode(const char *host, unsigned long len,
		char **ace_host, unsigned long *ace_len)
{
	(void)host; (void)len;
	*ace_host = NULL;
	if (ace_len) *ace_len = 0;
	return NSERROR_NOT_FOUND;
}

/* PDF save */
nserror save_pdf(const char *path) { (void)path; return NSERROR_OK; }

/* html_get_id_offset — declared in html/html.h */
int html_get_id_offset(void *h, void *frag_id, int *x, int *y)
{
	(void)h; (void)frag_id;
	if (x) *x = 0;
	if (y) *y = 0;
	return 0;
}
