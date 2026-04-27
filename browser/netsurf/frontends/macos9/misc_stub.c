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

/* Hotlist / global history — real implementations now linked
 * (hotlist.c provides hotlist_update_url; global_history.c provides
 * global_history_add). Stubs removed. */

/* content_textsearch_destroy — Phase 2: provided by content/textsearch.c */
/* image_cache_init / image_cache_fini — Phase 4 (image content handler is
 * gated; for now leave stubs in place since image handler not yet linked) */
nserror image_cache_init(const struct image_cache_parameters *p)
{
	return NSERROR_OK;
}

nserror image_cache_fini(void) { return NSERROR_OK; }

/* Page info — real implementation provided by desktop/page-info.c.
 * Stubs removed. */

/* ns_system_colour_init / ns_system_colour_finalize — Phase 2: provided by
 * desktop/system_colour.c */

/* free_user_agent_string — Phase 2: provided by utils/useragent.c */

/* Search web */
nserror search_web_init(const char *provider) { return NSERROR_OK; }
void search_web_finalise(void) {}

/* cert_chain_alloc / cert_chain_free / cert_chain_to_query / cert_chain_size /
 * cert_chain_dup — Phase 2: provided by utils/ssl_certs.c */

/* Download context — real implementation provided by desktop/download.c.
 * Stubs removed. */

/* DOM namespace — libdom only provides _dom_namespace_initialise (private),
 * NetSurf core calls dom_namespace_initialise (public). Keep stub. */
nserror dom_namespace_initialise(void) { return NSERROR_OK; }
void dom_namespace_finalise(void) {}

/* lwc iteration — libwapcaplet doesn't provide an iterator in our shim layer */
void lwc_iterate_strings(void (*cb)(void *str, void *pw), void *pw) {}

/* Fetch */
void fetch_abort(void *f) { (void)f; }
/* fetcher_init() now provided by macos9_fetcher_init.c — registers
 * the real OT-backed HTTP fetcher. Stub removed. */

/* Content handler init stubs — Phases 3+4 will replace these */
nserror textplain_init(void) { return NSERROR_OK; }
nserror image_init(void) { return NSERROR_OK; }
/* html_init — Phase 4: provided by content/handlers/html/html.c */
/* nscss_init — Phase 3: provided by content/handlers/css/css.c */

/* nscolour_update — Phase 2: provided by utils/nscolour.c */

/* idna_encode — Phase 2: provided by utils/idna.c */

/* PDF save */
nserror save_pdf(const char *path) { (void)path; return NSERROR_OK; }

/* nsutils base64 — used only by ssl_certs.c for cert query strings;
 * MacSurf doesn't fetch HTTPS in-browser (proxy strips TLS), so the
 * cert chain query never fires. Stub returns BAD_INPUT. */
typedef int nsuerror_t_;
int nsu_base64_encode_url(const unsigned char *input, unsigned long input_length,
		unsigned char **output, unsigned long *output_length)
{
	(void)input; (void)input_length; (void)output; (void)output_length;
	return 2; /* NSUERROR_BAD_INPUT */
}

/* html_get_id_offset — Phase 4: provided by content/handlers/html/html.c */
