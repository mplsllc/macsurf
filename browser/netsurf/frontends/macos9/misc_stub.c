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

/* Hotlist / global history — no replacement yet, defer */
nserror hotlist_update_url(struct nsurl *url) { return NSERROR_OK; }
nserror global_history_add(struct nsurl *url) { return NSERROR_OK; }

/* content_textsearch_destroy — Phase 2: provided by content/textsearch.c */
/* image_cache_init / image_cache_fini — Phase 4 (image content handler is
 * gated; for now leave stubs in place since image handler not yet linked) */
nserror image_cache_init(const struct image_cache_parameters *p)
{
	return NSERROR_OK;
}

nserror image_cache_fini(void) { return NSERROR_OK; }

/* Page info */
nserror page_info_fini(void) { return NSERROR_OK; }

/* ns_system_colour_init / ns_system_colour_finalize — Phase 2: provided by
 * desktop/system_colour.c */

/* free_user_agent_string — Phase 2: provided by utils/useragent.c */

/* Search web */
nserror search_web_init(const char *provider) { return NSERROR_OK; }
void search_web_finalise(void) {}

/* cert_chain_alloc / cert_chain_free / cert_chain_to_query / cert_chain_size /
 * cert_chain_dup — Phase 2: provided by utils/ssl_certs.c */

/* Download context */
nserror download_context_create(struct llcache_handle *llcache,
		struct gui_download_window *window)
{
	return NSERROR_OK;
}

void download_context_destroy(void *ctx) {}

/* DOM namespace — libdom only provides _dom_namespace_initialise (private),
 * NetSurf core calls dom_namespace_initialise (public). Keep stub. */
nserror dom_namespace_initialise(void) { return NSERROR_OK; }
void dom_namespace_finalise(void) {}

/* lwc iteration — libwapcaplet doesn't provide an iterator in our shim layer */
void lwc_iterate_strings(void (*cb)(void *str, void *pw), void *pw) {}

/* Page info */
nserror page_info_init(void) { return NSERROR_OK; }

/* Fetch */
void fetch_abort(void *f) { (void)f; }
/* fetcher_init() now provided by macos9_fetcher_init.c — registers
 * the real OT-backed HTTP fetcher. Stub removed. */

/* Content handler init stubs — Phases 3+4 will replace these */
nserror textplain_init(void) { return NSERROR_OK; }
nserror image_init(void) { return NSERROR_OK; }
nserror html_init(void) { return NSERROR_OK; }
/* nscss_init — Phase 3: provided by content/handlers/css/css.c */

/* nscolour_update — Phase 2: provided by utils/nscolour.c */

/* idna_encode — Phase 2: provided by utils/idna.c */

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
