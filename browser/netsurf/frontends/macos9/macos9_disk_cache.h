/*
 * MacSurf - macos9_disk_cache.h
 *
 * Persistent on-disk body cache shared by the HTTP and HTTPS fetchers.
 * One file per cached body in the "MacSurf Cache" folder on the boot
 * Desktop; filename is a hash of the URL. See macos9_disk_cache.c for
 * the disk format and write discipline.
 *
 * Each fetcher owns its own in-memory capture buffer (size-bounded,
 * geometric growth) and calls macos9_cache_store() once the response
 * is complete. Cache lookups happen at fetch-start time and let the
 * fetcher short-circuit straight to FETCH_FINISHED without touching
 * the network.
 */

#ifndef MACOS9_DISK_CACHE_H
#define MACOS9_DISK_CACHE_H

#include <stddef.h>

/* Single-response cap. Bigger bodies are served live, not cached.
 * fixes985: 1MB -> 2MB, now that images and webfonts are cacheable again.
 * Deliberately not fixes665's 4MB: this cap also bounds the fetcher's
 * in-RAM capture buffer, which is held whole before the store, and 2MB
 * already covers essentially every image on the web while asking half as
 * much of a 128MB machine mid-page-load. The whole-directory bound is
 * CACHE_TOTAL_BUDGET in macos9_disk_cache.c. */
#define MACSURF_CACHE_MAX_BYTES (2L * 1024L * 1024L)

/* When non-zero, the next cache_lookup short-circuits to "miss" so
 * the Reload button forces a fresh fetch. cache_store clears the flag
 * after the new body is written, so subsequent sub-resource fetches
 * resume normal cache behaviour. Defined in macos9_disk_cache.c. */
extern int macsurf_http_skip_next_cache;

/* fixes981 — cap on the persisted freshness/validator header block.
 * Cache-Control + ETag + Last-Modified + Expires + Date + Age is typically
 * 150-250 bytes; a response whose block would exceed this keeps whatever fit
 * whole (a truncated header is worse than a missing one). */
#define MACSURF_CACHE_HDRS_MAX 512

/* Returns 1 if this (status, mime) pair is worth persisting. */
int macos9_cache_mime_eligible(int status, const char *mime);

/* fixes981 — copy the response headers llcache needs in order to reason
 * about freshness (Date, Age, Expires, Cache-Control) and to revalidate
 * (ETag, Last-Modified) out of a full header line. Appends "Name: value\r\n"
 * to dst when the line is one of those, ignores it otherwise. dst is always
 * NUL-terminated; a line that would overflow is skipped whole. Both fetchers
 * call this per header line while parsing a response they intend to cache. */
void macos9_cache_capture_hdr(const char *line, char *dst, size_t cap);

/* fixes981 — store/lookup carrying the freshness header block. The plain
 * macos9_cache_store / macos9_cache_lookup remain, as wrappers passing no
 * headers, so any caller that does not care is unaffected.
 *
 * On-disk format is unchanged for old files: the 24-byte header's last field
 * was written as zero and is now the header-block length, so a file written
 * by an older build reads back as "no headers" and still serves. */
void macos9_cache_store_hdrs(const char *url, int status, const char *mime,
		const char *hdrs, const char *body_ptr, long body_len);
int macos9_cache_lookup_hdrs(const char *url, char **body_out,
		long *body_len_out, char *mime_out, int mime_cap,
		int *status_out, char *hdrs_out, int hdrs_cap);

/* Try to satisfy a fetch from the on-disk cache. Returns 1 on hit,
 * 0 on miss / I/O error. On hit, *body_out is a malloc'd buffer the
 * caller must free; *body_len_out is the byte count; mime_out (cap
 * >= 128) receives the stored MIME string; *status_out is the HTTP
 * status from the cached response. */
int macos9_cache_lookup(const char *url, char **body_out,
                        long *body_len_out, char *mime_out,
                        int mime_cap, int *status_out);

/* Persist one response. body_ptr is body_len bytes of unmodified
 * response body. Errors are silent (best-effort). */
void macos9_cache_store(const char *url, int status, const char *mime,
                        const char *body_ptr, long body_len);

/* fixes238 — persistent dead-host list. The HTTPS fetcher's in-memory
 * blocklist (host:port that timed out or peer-closed) is wiped on app
 * restart, so the first attempt to fonts.googleapis.com (or any other
 * fingerprint-blocked host) pays the full no-progress timeout on every
 * cold launch. These helpers serialise that list to a plain-text file
 * in the cache folder so subsequent sessions skip the timeout entirely.
 *
 * Format: one "host:port" per line, terminated by '\n'. Empty lines
 * ignored. No timestamp / TTL — call macos9_deadhost_clear() to forget.
 *
 * macos9_deadhost_load fills out_buf with the file contents, NUL-
 * terminated. Returns bytes read (excluding NUL), 0 on miss or error.
 * macos9_deadhost_save writes len bytes verbatim. */
long macos9_deadhost_load(char *out_buf, long buf_cap);
void macos9_deadhost_save(const char *buf, long len);
void macos9_deadhost_clear(void);

/* fixes706 — empty the disk cache (all cached bodies + deadhosts.txt under
 * MacSurfData/Cache). Bookmarks/history/cookies at the root are untouched.
 * Returns the count of files deleted. */
long macos9_cache_clear(void);

/* fixes368 (#167) — cookie-jar persistence across launches so a Facebook
 * (or any) login survives a relaunch. Call macos9_cookies_load() once at
 * startup (after netsurf_init, before the event loop) and
 * macos9_cookies_save() once at shutdown (before netsurf_exit). Both are
 * best-effort: any I/O failure is a silent no-op leaving the jar in-memory
 * only. See macos9_disk_cache.c for the implementation notes. */
void macos9_cookies_load(void);
void macos9_cookies_save(void);

#endif /* MACOS9_DISK_CACHE_H */
