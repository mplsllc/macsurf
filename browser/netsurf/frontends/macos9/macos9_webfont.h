/*
 * MacSurf - macos9_webfont.h
 *
 * Downloadable webfont (@font-face) support. See macos9_webfont.c.
 */

#ifndef MACOS9_WEBFONT_H
#define MACOS9_WEBFONT_H

#ifdef __MACOS9__

struct content;
struct lwc_string_s;

/* Ensure the @font-face font file for the given CSS font-family is fetched and
 * disk-cached. Cheap to call per text run — does real work only the first time
 * a family is seen (resolves the src URL via the core accessor, then fetches).
 * Round 1 stops at "bytes held"; parse/paint come in later rounds. */
void macos9_webfont_ensure(struct content *content,
		struct lwc_string_s *family);

#endif /* __MACOS9__ */
#endif /* MACOS9_WEBFONT_H */
