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
 * fixes679: reverted 4MB->1MB (old cache style). fixes665's image/font
 * disk caching + 4MB cap + directory budget-sweep were backed out; the
 * cache holds only small text bodies (HTML/CSS/JS) again. The MacSurfData
 * one-folder layout (fixes641/647) is retained. */
#define MACSURF_CACHE_MAX_BYTES (1L * 1024L * 1024L)

/* When non-zero, the next cache_lookup short-circuits to "miss" so
 * the Reload button forces a fresh fetch. cache_store clears the flag
 * after the new body is written, so subsequent sub-resource fetches
 * resume normal cache behaviour. Defined in macos9_disk_cache.c. */
extern int macsurf_http_skip_next_cache;

/* Returns 1 if this (status, mime) pair is worth persisting. */
int macos9_cache_mime_eligible(int status, const char *mime);

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
