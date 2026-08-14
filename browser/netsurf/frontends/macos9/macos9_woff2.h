/*
 * MacSurf - macos9_woff2.h
 *
 * Public surface of the WOFF2 -> TTF reconstructor (macos9_woff2.c). WOFF2
 * @font-face bodies are Brotli-compressed transformed sfnt data; this converts
 * one into a plain TrueType sfnt that macos9_webfont.c's existing
 * macos9_webfont_parse_sfnt() path can consume directly.
 *
 * C89 (CW8). The decoder is a port of google/woff2's woff2_dec.cc (Apache
 * 2.0); see the .c for full provenance.
 */

#ifndef MACSURF_MACOS9_WOFF2_H
#define MACSURF_MACOS9_WOFF2_H

/*
 * Decompress + reconstruct a WOFF2 font into a standard TTF sfnt.
 *
 *   src / src_len : the raw WOFF2 file bytes
 *   *out          : on success, a malloc'd buffer owned by the caller
 *   *out_len      : on success, its length in bytes
 *
 * Returns 1 on success (caller must free(*out)), 0 on failure. TrueType
 * Collections ('ttcf' flavor) are rejected - MacSurf's sfnt parser has never
 * handled collections, and no web font ships one. Failure leaves *out NULL.
 */
int macos9_woff2_to_ttf(const unsigned char *src, long src_len,
		unsigned char **out, long *out_len);

#endif /* MACSURF_MACOS9_WOFF2_H */
