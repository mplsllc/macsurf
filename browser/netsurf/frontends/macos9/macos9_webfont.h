/*
 * MacSurf - macos9_webfont.h
 *
 * Downloadable webfont (@font-face) glyph rendering. See macos9_webfont.c.
 *
 * Modern sites ship their icon fonts (FontAwesome, Material Design Icons) as
 * @font-face rules whose glyphs live in the Unicode Private-Use Area. This
 * module fetches the font file, parses the sfnt (cmap/hmtx/loca/glyf), and
 * paints glyph outlines as QuickDraw regions so the icons actually render.
 */

#ifndef MACOS9_WEBFONT_H
#define MACOS9_WEBFONT_H

#ifdef __MACOS9__

struct content;
struct lwc_string_s;

/* Ensure the @font-face font file for the given CSS font-family is fetched and
 * disk-cached (and, once bytes are held, parsed). Cheap to call per text run  - 
 * does real work only the first time a family is seen. */
void macos9_webfont_ensure(struct content *content,
		struct lwc_string_s *family);

/* Glyph advance width in pixels for codepoint `cp` in `family` at `size_px`.
 * Triggers the fetch (via ensure) if the font isn't loaded yet. Returns the
 * advance (>= 0) when the font is loaded, parsed, and has a glyph for cp;
 * returns -1 otherwise (not a webfont family / not loaded yet / no glyph) so
 * the caller can fall back to its normal path. Used by the MEASURE path so an
 * icon box gets real width instead of collapsing to zero. */
int macos9_webfont_advance(struct content *content,
		struct lwc_string_s *family,
		unsigned long cp,
		int size_px);

/* Paint the glyph for `cp` in `family` at pen (pen_x, baseline_y), scaled to
 * size_px, using the CURRENT QuickDraw foreground colour (caller sets it to the
 * text colour first). Returns the advance width in pixels (>= 0) if the glyph
 * was drawn (or is an empty/space glyph), or -1 if unavailable (caller falls
 * back to its blank/no-op path). */
int macos9_webfont_render(struct content *content,
		struct lwc_string_s *family,
		unsigned long cp,
		int pen_x,
		int baseline_y,
		int size_px);

#endif /* __MACOS9__ */
#endif /* MACOS9_WEBFONT_H */
