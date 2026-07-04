/*
 * MacSurf - macos9_svg_sprite.h
 *
 * External SVG sprite support for inline <svg><use href="file.svg#id"> icons
 * (FontAwesome, as used by XenForo/68kmla). MacSurf has no XML->DOM parser
 * and no live frontend fetch primitive, so this module:
 *   - triggers a normal NetSurf fetch of the sprite file the first time an
 *     icon references it (the fetcher stores the bytes to the disk cache
 *     regardless of whether a content handler exists), and repaints when it
 *     arrives;
 *   - reads the cached bytes back via macos9_cache_lookup and holds them;
 *   - runs a targeted mini-parser (no DOM) over the FontAwesome sprite format
 *     to locate a referenced <symbol>'s viewBox and <path d="...">.
 * The inline-SVG painter (macos9_svg_inline.c) then paints that path via the
 * existing svg__path_parse / plotter path op.
 */

#ifndef MACOS9_SVG_SPRITE_H
#define MACOS9_SVG_SPRITE_H

#ifdef __MACOS9__

struct nsurl;

/* Resolve an external <use href="file.svg#id"> reference against the page
 * base URL, ensure the sprite file is fetched+cached (kicks a fetch on the
 * first miss, no-op while one is in flight), and locate the referenced
 * <symbol>'s path data.
 *
 * On success (return 1): *sym_start / *sym_end bound the referenced symbol's
 * CONTENT (between its opening tag and </symbol>) inside an internally-held,
 * NUL-terminated buffer, and vb[0..3] is the symbol's viewBox (x y w h). The
 * caller walks [*sym_start, *sym_end) for one or more <path d="…"> and paints
 * each. The pointers are valid until the next call that loads a different
 * sprite; consume them immediately.
 *
 * Returns 0 if the sprite is not yet available (fetch pending) or the symbol
 * was not found. Never blocks. */
int macos9_svg_sprite_symbol(struct nsurl *base, const char *href,
		const char **sym_start, const char **sym_end, float *vb);

#endif /* __MACOS9__ */

#endif /* MACOS9_SVG_SPRITE_H */
