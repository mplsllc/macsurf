/*
 * MacSurf  -  Mac OS 9 frontend for NetSurf
 * plotters.c  -  All plotter_table callbacks
 *
 * Phase 5: implement clip, rectangle, text against QuickDraw.
 * Other plotters remain stubs and will be filled in incrementally.
 *
 * This file is part of MacSurf, built on the NetSurf engine.
 * Licensed under GPL v2.
 */

#include <stdlib.h>
#include <string.h>

#include <libcss/properties.h>

#include "utils/ns_errors.h"
#include "utils/log.h"
#include "netsurf/types.h"
#include "netsurf/css.h"
#include "netsurf/plot_style.h"
#include "netsurf/plotters.h"
#include "netsurf/bitmap.h"

/* Forward-declare our bitmap accessors directly so plotters.c
 * does not need an implicit-int fallback for bitmap_get_buffer.
 * These are defined in macos9_bitmap.c. */
extern unsigned char *macos9_bitmap_get_buffer(void *bitmap);
extern int macos9_bitmap_get_width(void *bitmap);
extern int macos9_bitmap_get_height(void *bitmap);
extern size_t macos9_bitmap_get_rowstride(void *bitmap);
extern bool macos9_bitmap_get_opaque(void *bitmap);
extern unsigned char *macos9_bitmap_get_mask(void *bitmap);
extern int macos9_bitmap_get_mask_rowbytes(void *bitmap);

/* Diagnostic counters - read from main.c after redraw. */
long macos9_plot_text_count = 0;
long macos9_plot_rect_count = 0;

/* fixes366c  -  per-paint-pattern log throttles for the fixes365/
 * fixes366b diagnostic lines emitted from macos9_plot_rectangle.
 * Originally function-scoped statics that capped at 5 per session;
 * hoisted to file scope so macsurf_profile_reset() can zero them
 * at the start of each navigation via
 * macos9_plotter_diag_counters_reset(). */
static int hstripe_paint_seen = 0;
static int dotgrid_paint_seen = 0;
static int diag_paint_seen = 0;
static int clip_text_mask_seen = 0;
static int clip_text_state_seen = 0;
static int blend_colour_seen = 0;
static int blend_bitmap_seen = 0;
/* fixes366d  -  log the snapshot values at plot_rectangle entry,
 * regardless of which branch the plotter ends up taking. Tells us
 * whether the one-shot values are reaching the plotter at all, or
 * whether they're being consumed/cleared by an intermediate paint.
 * Capped to first 8 entries per navigation. */
static int snapshot_seen = 0;

void macos9_plotter_diag_counters_reset(void)
{
	hstripe_paint_seen = 0;
	dotgrid_paint_seen = 0;
	diag_paint_seen = 0;
	clip_text_mask_seen = 0;
	clip_text_state_seen = 0;
	blend_colour_seen = 0;
	blend_bitmap_seen = 0;
	snapshot_seen = 0;
}

/* fixes144b: sub-AA draw-spacing experiment. QuickDraw bitmap text
 * below the AA floor (set to 12pt in main.c via
 * SetAntiAliasedTextEnabled) draws adjacent glyphs with no visible
 * separator - "Di"/"Disc"/"Dill" visually collide at 9-10pt
 * Helvetica because the D's painted right edge and the i's left edge
 * land in adjacent or shared pixel columns. fixes144a2's diagnostic
 * probe captured 216 measurements and showed every delta=0; this is
 * a paint-resolution artefact, not a metric error. The fix forces
 * the per-char draw path and adds +1px between glyphs ONLY when
 * size < AA floor and the font is proportional. Measurement (macos9_
 * font_measure) is intentionally NOT touched - layout, wrap, and
 * text-overflow stay byte-stable, so MacTrove cards / nav labels /
 * inputs don't reflow.
 *
 * Set to 0 to disable instantly. */
#define MACSURF_SUBAA_DRAW_SPACING 1

/* fixes145: font-family alias dispatch. NetSurf core resolves the
 * CSS font-family list ("Helvetica, Arial, sans-serif", etc.) into
 * one of 5 generic categories before plot_font_style is built; we
 * dispatch on that enum rather than string-matching individual
 * names.
 *
 *   PLOT_FONT_FAMILY_MONOSPACE  -> Monaco
 *   PLOT_FONT_FAMILY_SERIF      -> Times (kFontIDTimes = 20)
 *   PLOT_FONT_FAMILY_SANS_SERIF -> Helvetica (current calibration target)
 *   PLOT_FONT_FAMILY_CURSIVE    -> Helvetica (no Mac cursive system font)
 *   PLOT_FONT_FAMILY_FANTASY    -> Helvetica (no Mac fantasy system font)
 *
 * Helvetica stays as the sans-serif target rather than Geneva: all
 * the fixes51 / 68-70 / 144a/b font tuning was done against Helvetica
 * TT metrics. Geneva is a viable secondary target but flipping the
 * default would re-open all that calibration work.
 *
 * Baseline-drift risk: fixes52 force-collapsed every CSS family to
 * Helvetica because NetSurf's inline layout had a bug where a single
 * line mixing fonts with different installed metrics (body + inline
 * <code>) stacked lines 2-4px on top of each other. The proper fix
 * is real per-font ascent/descent through gui_layout_table (deferred
 * - needs NetSurf-core work). This flag is the experiment: ship the
 * alias dispatch, hardware-probe for inline-mix drift, revert if it
 * reproduces. Set to 0 to fall back to fixes52's behaviour.
 *
 * fixes145b (2026-05-19): REVERTED to 0 after hardware probe. The
 * baseline-drift bug from fixes52 also manifests horizontally on
 * mixed-family inline content: text segments from adjacent <code>
 * vs body runs scrambled into each other on the FF1-FF4 probe cards
 * ("MonacoaroumsHflfaes", "diralijoeranzas thank") because NetSurf's
 * inline layout reserves widths assuming one font's metrics while
 * individual segments paint with different fonts. The fix is real
 * per-font ascent/descent + gui_layout_table family awareness, which
 * is a NetSurf-core-side change beyond this round's scope. */
/* fixes154: re-enabled. The vmetric probe in fixes153 confirmed all
 * candidate families (Helvetica, Times, Monaco, Geneva, Chicago,
 * Palatino, Courier) have sensible per-font metrics; the data does
 * NOT explain the fixes145 horizontal scrambling that motivated the
 * 0-default. The retry ships with MACSURF_FONT_ALIAS_DIAG turned on so
 * every width/paint call logs (op, family, font_id, size, face,
 * letter/word spacing, mac string length, x/y or width). If
 * scrambling recurs on hardware, the logs will show the exact width-
 * vs-paint font_id divergence point. Set to 0 if a real regression
 * lands and a quick rollback is needed.
 *
 * Old comment kept for reference:
 *   "lines stack 2-4 px on top of each other" was the fixes52 symptom.
 *   fixes145 saw horizontal text scrambling rather than vertical
 *   stacking. The vmetric data showed mixed-family lines at CSS
 *   line-height >= 1.3 (the normal default) accommodate all OS 9
 *   families. The line-height: 1 edge case is rare on real pages. */
/* fixes154/154b/154c (rejected): all three attempted alias-related
 * changes when the actual culprit was the defensive-clamp threshold
 * (fixed at fixes156). The "empty render" symptom we kept seeing was
 * the page crossing 10000 px tall and tripping the clamp on the root
 * box. Now that fixes156 raised the y/height clamp to ±200000, the
 * alias dispatch has clean ground to be re-tested on.
 *
 * fixes157: re-enable alias dispatch, post-clamp-fix. Per-call FONTDIAG
 * logging gated on MACSURF_FONT_ALIAS_DIAG in macos9.h, and further
 * narrowed by MACSURF_FONT_ALIAS_DIAG_SMART which skips the SANS_SERIF
 * (Helvetica-default) firehose. The remaining log lines are non-default
 * family dispatches  -  exactly the cases where the width-vs-paint
 * divergence would matter. width and paint share macos9_font_id_from_style
 * as the dispatch entry point (verified at macos9_font_measure:163 and
 * plot_text:1311), so by construction they cannot disagree on a single
 * fstyle  -  but any inline-layout drift between segments will show as
 * adjacent-line family/size/face mismatches in the log.
 *
 * Acceptance criteria for this round: FF1-FF5 render visibly distinct
 * fonts; MacTrove home + advanced.html + DuckDuckGo still render past
 * 10000 px without empty-redraw regression; no horizontal scrambling
 * on multi-family inline lines. If scrambling reproduces, compare
 * width-vs-paint dispatch in the FONTDIAG log before reverting. */
#define MACSURF_FONT_FAMILY_ALIASES 1

/* fixes74b: counters incremented by redraw.c when it detects
 * CSS_MACSURF_GRADIENT_SET. Lets us see whether the cascade returned
 * SET independently of whether the plotter painted a gradient. */
long macos9_grad_set_count = 0;
long macos9_grad_radial_unpack_count = 0;
long macos9_grad_linear_unpack_count = 0;

#include "macos9.h"
#include "macsurf_debug.h"
#include "macos9_webfont.h"

/* fixes615 (webfonts)  -  lean externs to reach the current content from the
 * paint path without pulling the full browser_window / hlcache headers into
 * this frontend TU. All opaque pointers. */
struct browser_window;
struct hlcache_handle;
struct content;
extern struct browser_window *macos9_gw_bw(struct gui_window *g);
extern struct hlcache_handle *browser_window_get_content(
		struct browser_window *bw);
extern struct content *hlcache_handle_get_content(
		const struct hlcache_handle *handle);

#ifdef __MACOS9__
#include <Quickdraw.h>
#include <QuickdrawText.h>
#else
/* Linux cross-check stubs  -  match Mac toolbox shapes loosely. */
typedef struct { short top, left, bottom, right; } MacRect;
typedef struct { unsigned short red, green, blue; } RGBColor;
#define Rect MacRect
#define noErr 0
static void ClipRect(const Rect *r) { (void)r; }
static void PaintRect(const Rect *r) { (void)r; }
static void FrameRect(const Rect *r) { (void)r; }
static void RGBForeColor(const RGBColor *c) { (void)c; }
static void TextFont(short f) { (void)f; }
static void TextSize(short s) { (void)s; }
static void TextFace(short f) { (void)f; }
static void MoveTo(short h, short v) { (void)h; (void)v; }
static void DrawText(const void *b, short s, short l) { (void)b;(void)s;(void)l; }
static void LineTo(short h, short v) { (void)h; (void)v; }
#define kFontIDMonaco       4
#define kFontIDGeneva       3
#define kFontIDTimes        20
#define kFontIDCourier      22
#define kFontIDHelvetica    21
#define normal              0
#define bold                1
#define italic              2
#endif

/* ---- helpers ---- */

/*
 * NetSurf packs colours as 0xBBGGRR<flags>:
 *   bits  0-7  : red    (per netsurf/types.h, the BYTE-aligned form)
 * In practice the macros in plot_style.h treat the layout as
 * 0xRRGGBBxx with red in the low byte. We match the existing
 * NetSurf macros: red_from_colour, green_from_colour, blue_from_colour
 * which return the byte value. Replicate locally to avoid pulling
 * in extra headers.
 */
/* fixes620: backdrop the plotter composites semi-transparent (rgba)
 * fills, strokes, borders, gradient stops, shadows and text against.
 * Published per box by html_redraw_box (content/handlers/html/redraw.c)
 * as the current_background_color beneath the box being painted. ns
 * colour format is 0x00BBGGRR; the top byte carries (255 - css_alpha),
 * i.e. transparency: 0 = fully opaque, 255 = fully transparent (see
 * nscss_color_to_ns, which NOTs the css alpha byte). Init to white;
 * overwritten before the first real content fill. Non-static so
 * redraw.c can reach it via `extern colour macos9_plot_backdrop`. */
colour macos9_plot_backdrop = 0x00ffffff;

static void
macos9_colour_to_rgb(colour c, RGBColor *out)
{
	unsigned int r = (unsigned int)((c >>  0) & 0xff);
	unsigned int g = (unsigned int)((c >>  8) & 0xff);
	unsigned int b = (unsigned int)((c >> 16) & 0xff);
	unsigned int t = (unsigned int)((c >> 24) & 0xff);

	/* fixes620: composite rgba() over the current backdrop so alpha
	 * actually blends instead of rendering solid. op = css_alpha =
	 * 255 - transparency; out = fg*op + backdrop*(255-op) per channel.
	 * op + t == 255 so this is a true weighted average. Integer math
	 * only (max 255*255 = 65025, fits in int) - no long long, CW8
	 * safe. t == 0 is the opaque fast path (leaves fg untouched).
	 * NS_TRANSPARENT (0x01000000) is a sentinel, not an alpha value,
	 * so it is excluded (guarded upstream; excluding it here keeps the
	 * pre-fix behaviour if it ever reaches a fill). */
	if (t != 0 && c != NS_TRANSPARENT) {
		unsigned int op = 255u - t;
		unsigned int br = (unsigned int)((macos9_plot_backdrop >>  0) & 0xff);
		unsigned int bgc = (unsigned int)((macos9_plot_backdrop >>  8) & 0xff);
		unsigned int bb = (unsigned int)((macos9_plot_backdrop >> 16) & 0xff);
		unsigned int fr_ = r, fg_ = g, fb_ = b;   /* fixes751: pre-composite fg */
		r = (r * op + br * t) / 255u;
		g = (g * op + bgc * t) / 255u;
		b = (b * op + bb * t) / 255u;
		/* fixes751 (#204) PROBE  -  a dark semi-transparent overlay is the
		 * XenForo .p-navgroup rgba(20,20,20,.15) user-button/search fill.
		 * Log fg, alpha, the backdrop it composited against, and the
		 * result. If bd=255,255,255 the button flattened against WHITE
		 * (the bug -> grey ~202); if bd~=23,79,121 it hit the nav blue
		 * (correct). RECON prefix survives the perf log filter; dark-gated
		 * and capped so it doesn't flood. */
		if (fr_ < 60 && fg_ < 60 && fb_ < 60) {
			static int recon_ovl_n = 0;
			if (recon_ovl_n < 12) {
				recon_ovl_n++;
				macsurf_debug_log_writef(
					"RECON OVL fg=%d,%d,%d a=%d bd=%d,%d,%d -> %d,%d,%d",
					(int)fr_, (int)fg_, (int)fb_, (int)op,
					(int)br, (int)bgc, (int)bb,
					(int)r, (int)g, (int)b);
			}
		}
	}

	/* 8-bit -> 16-bit by replicating the byte (0xAB -> 0xABAB).
	 * Standard QuickDraw idiom  -  same trick CopyBits / Picture
	 * recording uses. */
	out->red   = (unsigned short)((r << 8) | r);
	out->green = (unsigned short)((g << 8) | g);
	out->blue  = (unsigned short)((b << 8) | b);
}

struct macos9_blend_rgb {
	int c[3];
};

static int macos9_blend_clamp(int value)
{
	if (value < 0) return 0;
	if (value > 255) return 255;
	return value;
}

static int macos9_blend_lum(const struct macos9_blend_rgb *rgb)
{
	return (30 * rgb->c[0] + 59 * rgb->c[1] + 11 * rgb->c[2]) / 100;
}

static int macos9_blend_sat(const struct macos9_blend_rgb *rgb)
{
	int lo = rgb->c[0];
	int hi = rgb->c[0];
	int i;
	for (i = 1; i < 3; i++) {
		if (rgb->c[i] < lo) lo = rgb->c[i];
		if (rgb->c[i] > hi) hi = rgb->c[i];
	}
	return hi - lo;
}

static void macos9_blend_clip_colour(struct macos9_blend_rgb *rgb)
{
	int lum = macos9_blend_lum(rgb);
	int lo = rgb->c[0];
	int hi = rgb->c[0];
	int i;
	for (i = 1; i < 3; i++) {
		if (rgb->c[i] < lo) lo = rgb->c[i];
		if (rgb->c[i] > hi) hi = rgb->c[i];
	}
	if (lo < 0 && lum != lo) {
		for (i = 0; i < 3; i++)
			rgb->c[i] = lum + ((rgb->c[i] - lum) * lum) / (lum - lo);
	}
	if (hi > 255 && hi != lum) {
		for (i = 0; i < 3; i++)
			rgb->c[i] = lum + ((rgb->c[i] - lum) * (255 - lum)) /
					(hi - lum);
	}
	for (i = 0; i < 3; i++)
		rgb->c[i] = macos9_blend_clamp(rgb->c[i]);
}

static void macos9_blend_set_lum(struct macos9_blend_rgb *rgb, int lum)
{
	int delta = lum - macos9_blend_lum(rgb);
	int i;
	for (i = 0; i < 3; i++) rgb->c[i] += delta;
	macos9_blend_clip_colour(rgb);
}

static void macos9_blend_set_sat(struct macos9_blend_rgb *rgb, int sat)
{
	int lo = 0;
	int hi = 0;
	int mid;
	int old_lo;
	int old_hi;
	int i;

	for (i = 1; i < 3; i++) {
		if (rgb->c[i] < rgb->c[lo]) lo = i;
		if (rgb->c[i] > rgb->c[hi]) hi = i;
	}
	mid = 3 - lo - hi;
	if (lo == hi) {
		rgb->c[0] = rgb->c[1] = rgb->c[2] = 0;
		return;
	}
	old_lo = rgb->c[lo];
	old_hi = rgb->c[hi];
	rgb->c[mid] = ((rgb->c[mid] - old_lo) * sat) /
			(old_hi - old_lo);
	rgb->c[hi] = sat;
	rgb->c[lo] = 0;
}

static int macos9_blend_isqrt(int value)
{
	int result = 0;
	int bit = 1 << 14;
	while (bit > value) bit >>= 2;
	while (bit != 0) {
		if (value >= result + bit) {
			value -= result + bit;
			result = (result >> 1) + bit;
		} else {
			result >>= 1;
		}
		bit >>= 2;
	}
	return result;
}

static int macos9_blend_channel(int source, int backdrop, uint8_t mode)
{
	int d;
	long value;

	switch (mode) {
	case CSS_BACKGROUND_BLEND_MODE_MULTIPLY:
		return (source * backdrop + 127) / 255;
	case CSS_BACKGROUND_BLEND_MODE_SCREEN:
		return 255 - ((255 - source) * (255 - backdrop) + 127) / 255;
	case CSS_BACKGROUND_BLEND_MODE_OVERLAY:
		if (backdrop <= 127) return (2 * source * backdrop + 127) / 255;
		return 255 - (2 * (255 - source) * (255 - backdrop) + 127) / 255;
	case CSS_BACKGROUND_BLEND_MODE_DARKEN:
		return source < backdrop ? source : backdrop;
	case CSS_BACKGROUND_BLEND_MODE_LIGHTEN:
		return source > backdrop ? source : backdrop;
	case CSS_BACKGROUND_BLEND_MODE_COLOR_DODGE:
		if (source >= 255) return 255;
		value = ((long)backdrop * 255L) / (255 - source);
		return value > 255 ? 255 : (int)value;
	case CSS_BACKGROUND_BLEND_MODE_COLOR_BURN:
		if (source <= 0) return 0;
		value = ((long)(255 - backdrop) * 255L) / source;
		if (value > 255) value = 255;
		return 255 - (int)value;
	case CSS_BACKGROUND_BLEND_MODE_HARD_LIGHT:
		if (source <= 127) return (2 * source * backdrop + 127) / 255;
		return 255 - (2 * (255 - source) * (255 - backdrop) + 127) / 255;
	case CSS_BACKGROUND_BLEND_MODE_SOFT_LIGHT:
		if (source <= 127) {
			value = (long)(255 - 2 * source) * backdrop *
					(255 - backdrop);
			return macos9_blend_clamp(backdrop -
					(int)(value / (255L * 255L)));
		}
		if (backdrop <= 63) {
			d = (int)(((((long)16 * backdrop - 12L * 255L) *
					backdrop) / 255L + 4L * 255L) * backdrop / 255L);
		} else {
			d = macos9_blend_isqrt(backdrop * 255);
		}
		return macos9_blend_clamp(backdrop +
				((2 * source - 255) * (d - backdrop)) / 255);
	case CSS_BACKGROUND_BLEND_MODE_DIFFERENCE:
		return source > backdrop ? source - backdrop : backdrop - source;
	case CSS_BACKGROUND_BLEND_MODE_EXCLUSION:
		return macos9_blend_clamp(source + backdrop -
				(2 * source * backdrop + 127) / 255);
	default:
		return source;
	}
}

colour macos9_background_blend_colour(colour source, colour backdrop,
		uint8_t mode)
{
	struct macos9_blend_rgb src;
	struct macos9_blend_rgb back;
	struct macos9_blend_rgb out;
	unsigned int transparency;
	unsigned int opacity;
	int i;

	if (mode <= CSS_BACKGROUND_BLEND_MODE_NORMAL ||
			mode > CSS_BACKGROUND_BLEND_MODE_LUMINOSITY)
		return source;
	if (blend_colour_seen < 12) {
		macsurf_debug_log_writef(
			"LIFE blend colour mode=%d src=%ld bg=%ld",
			(int)mode, (long)source, (long)backdrop);
		blend_colour_seen++;
	}

	src.c[0] = (int)((source >> 0) & 0xff);
	src.c[1] = (int)((source >> 8) & 0xff);
	src.c[2] = (int)((source >> 16) & 0xff);
	back.c[0] = (int)((backdrop >> 0) & 0xff);
	back.c[1] = (int)((backdrop >> 8) & 0xff);
	back.c[2] = (int)((backdrop >> 16) & 0xff);
	out = src;

	if (mode <= CSS_BACKGROUND_BLEND_MODE_EXCLUSION) {
		for (i = 0; i < 3; i++)
			out.c[i] = macos9_blend_channel(src.c[i], back.c[i], mode);
	} else if (mode == CSS_BACKGROUND_BLEND_MODE_HUE) {
		macos9_blend_set_sat(&out, macos9_blend_sat(&back));
		macos9_blend_set_lum(&out, macos9_blend_lum(&back));
	} else if (mode == CSS_BACKGROUND_BLEND_MODE_SATURATION) {
		out = back;
		macos9_blend_set_sat(&out, macos9_blend_sat(&src));
		macos9_blend_set_lum(&out, macos9_blend_lum(&back));
	} else if (mode == CSS_BACKGROUND_BLEND_MODE_COLOR) {
		macos9_blend_set_lum(&out, macos9_blend_lum(&back));
	} else {
		out = back;
		macos9_blend_set_lum(&out, macos9_blend_lum(&src));
	}

	transparency = (unsigned int)((source >> 24) & 0xff);
	if (source == NS_TRANSPARENT) transparency = 255;
	if (transparency != 0) {
		opacity = 255u - transparency;
		for (i = 0; i < 3; i++) {
			out.c[i] = (int)(((unsigned int)out.c[i] * opacity +
					(unsigned int)back.c[i] * transparency) / 255u);
		}
	}

	return (colour)((unsigned long)macos9_blend_clamp(out.c[0]) |
			((unsigned long)macos9_blend_clamp(out.c[1]) << 8) |
			((unsigned long)macos9_blend_clamp(out.c[2]) << 16));
}

static void
macos9_rect_from_ns(const struct rect *src, Rect *dst)
{
	dst->left   = (short)src->x0;
	dst->top    = (short)src->y0;
	dst->right  = (short)src->x1;
	dst->bottom = (short)src->y1;
}

/* fixes51 - case-insensitive byte compare for the small set of
 * CSS font names we recognise. Avoids pulling in tolower / strcasecmp
 * dependencies; names are stable ASCII. */
static int macos9_name_match(const char *s, size_t n, const char *name)
{
	size_t i;
	for (i = 0; i < n; i++) {
		char a = s[i];
		char b = name[i];
		if (b == '\0') return 0;
		if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
		if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
		if (a != b) return 0;
	}
	return (name[n] == '\0');
}

short
macos9_font_id_from_style(const plot_font_style_t *fstyle)
{
#if MACSURF_FONT_FAMILY_ALIASES
	/* fixes145 - dispatch on generic family. See top-of-file
	 * comment for the alias table and the baseline-drift caveat. */
	if (fstyle == NULL) {
		return kFontIDHelvetica;
	}
	switch (fstyle->family) {
	case PLOT_FONT_FAMILY_MONOSPACE:
		return kFontIDMonaco;
	case PLOT_FONT_FAMILY_SERIF:
		return kFontIDTimes;
	case PLOT_FONT_FAMILY_SANS_SERIF:
	case PLOT_FONT_FAMILY_CURSIVE:
	case PLOT_FONT_FAMILY_FANTASY:
	default:
		return kFontIDHelvetica;
	}
#else
	/* fixes52 - force Helvetica for every CSS font-family. NetSurf's
	 * inline layout has a baseline-drift bug whenever a single line
	 * mixes fonts with different installed metrics (e.g. body text
	 * + <code>); lines stack 2-4 px on top of each other and become
	 * unreadable. The proper fix is real per-font ascent/descent
	 * through gui_layout_table (deferred  -  needs NetSurf-core work).
	 * Until then we sidestep the bug by collapsing every CSS family
	 * to a single font, and pick Helvetica because it ships with a
	 * full TrueType outline on every Mac OS 9 system (so the
	 * SetOutlinePreferred call from fixes51 renders it smoothly at
	 * any pt size). */
	(void)fstyle;
	return kFontIDHelvetica;
#endif
}

short
macos9_face_from_style(const plot_font_style_t *fstyle)
{
	short face = 0;
	if (fstyle == NULL)
		return 0;
	if (fstyle->weight >= 600) face |= bold;
	if (fstyle->flags & FONTF_ITALIC) face |= italic;
	if (fstyle->flags & FONTF_OBLIQUE) face |= italic;
	return face;
}

/* ---- plotters ---- */

#ifdef __MACOS9__
/* fixes361h  -  one-shot static for the second box-shadow's packed
 * value. redraw.c calls macos9_set_box_shadow_2(packed) right before
 * a plot->rectangle that should paint a second-inset bevel; the
 * plotter reads-and-clears so the value can't leak into subsequent
 * paints that didn't explicitly set it.
 *
 * Replaces the fixes361b/g attempt to extend plot_style_t with new
 * fields, which leaked stack garbage from uninitialised plot_style_t
 * locals in netsurf-core paint paths and produced phantom red bevels
 * on cards / inputs. The static is process-global but only meaningful
 * for the immediately following plot call, so it's robust to those
 * uninitialised locals. */
static int32_t macos9_box_shadow_2_oneshot = 0;
static int32_t macos9_box_shadow_3_oneshot = 0;
void macos9_set_box_shadow_2(int32_t packed)
{
	macos9_box_shadow_2_oneshot = packed;
}
/* fixes362  -  third box-shadow one-shot. Platinum convention is two
 * inset bevels (light + dark) followed by one outer drop. */
void macos9_set_box_shadow_3(int32_t packed)
{
	macos9_box_shadow_3_oneshot = packed;
}

/* fixes364  -  horizontal stripe background one-shot. Packed format
 * mirrors css_computed_macsurf_hstripe_bg: bit 31 = set flag, bits
 * 15..29 = c2 RGB555, bits 0..14 = c1 RGB555. redraw.c sets this
 * before plot_rectangle paints the background fill; the plotter
 * reads-and-clears the slot and overrides the flat-fill with
 * alternating-row stripes (c1, c2, c1, c2, ...) one pixel each. */
static int32_t macos9_hstripe_bg_oneshot = 0;
void macos9_set_hstripe_bg(int32_t packed)
{
	macos9_hstripe_bg_oneshot = packed;
}

/* fixes365c  -  two-layer dot-grid background one-shot. Same packed
 * format as hstripe_bg (bit 31 set, bits 15..29 c2 RGB555, bits 0..14
 * c1 RGB555). redraw.c sets this before plot_rectangle; the plotter
 * reads-and-clears the slot and overrides the flat fill with a 2x2
 * grid of alternating 1px vertical (c1) + horizontal (c2) stripes. */
static int32_t macos9_dotgrid_oneshot = 0;
void macos9_set_dotgrid(int32_t packed)
{
	macos9_dotgrid_oneshot = packed;
}

/* fixes365b  -  extended-linear-gradient one-shot statics. redraw.c calls
 * macos9_set_gradient_stops() / macos9_set_gradient_angle() right before
 * a plot->rectangle that should paint a diagonal or 3-stop gradient; the
 * plotter reads-and-clears so the values can't leak to subsequent paints
 * that didn't explicitly set them. Same lifecycle / safety story as the
 * box_shadow_2/3 and hstripe_bg one-shots (project_plotters_port_assumption,
 * fixes361h+364). Storing on plot_style_t would leak stack garbage. */
static const int32_t *macos9_gradient_stops_oneshot = NULL;
static uint16_t macos9_gradient_angle_oneshot = 0;
static uint8_t macos9_gradient_blend_mode_oneshot =
		CSS_BACKGROUND_BLEND_MODE_NORMAL;
static colour macos9_gradient_blend_backdrop_oneshot = 0;
void macos9_set_gradient_stops(const int32_t *stops)
{
	macos9_gradient_stops_oneshot = stops;
}
void macos9_set_gradient_angle(uint16_t angle)
{
	macos9_gradient_angle_oneshot = angle;
}
void macos9_set_gradient_blend(uint8_t mode, colour backdrop)
{
	macos9_gradient_blend_mode_oneshot = mode;
	macos9_gradient_blend_backdrop_oneshot = backdrop;
}

/* #255 `background-clip:text` carries an already-resolved element
 * background from redraw.c while one text box is being plotted. The text
 * callback rasterises the glyph run to a 1-bit QuickDraw mask, converts it
 * to a region, then reuses the ordinary rectangle/bitmap painters inside it. */
struct macos9_background_clip_text_state {
	bool active;
	plot_style_t fill;
	struct rect fill_rect;
	struct bitmap *bitmap;
	int bitmap_x;
	int bitmap_y;
	int bitmap_width;
	int bitmap_height;
	colour bitmap_background;
	bitmap_flags_t bitmap_flags;
	const int32_t *gradient_stops;
	uint16_t gradient_angle;
	uint8_t blend_mode;
	colour blend_backdrop;
};

static struct macos9_background_clip_text_state macos9_background_clip_text;

void macos9_background_clip_text_begin(const plot_style_t *fill,
		const struct rect *fill_rect, struct bitmap *bitmap,
		int bitmap_x, int bitmap_y, int bitmap_width, int bitmap_height,
		colour bitmap_background, bitmap_flags_t bitmap_flags,
		const int32_t *gradient_stops, uint16_t gradient_angle,
		uint8_t blend_mode, colour blend_backdrop)
{
	memset(&macos9_background_clip_text, 0,
		sizeof(macos9_background_clip_text));
	if (fill != NULL) {
		macos9_background_clip_text.fill = *fill;
	}
	if (fill_rect != NULL) {
		macos9_background_clip_text.fill_rect = *fill_rect;
	}
	macos9_background_clip_text.bitmap = bitmap;
	macos9_background_clip_text.bitmap_x = bitmap_x;
	macos9_background_clip_text.bitmap_y = bitmap_y;
	macos9_background_clip_text.bitmap_width = bitmap_width;
	macos9_background_clip_text.bitmap_height = bitmap_height;
	macos9_background_clip_text.bitmap_background = bitmap_background;
	macos9_background_clip_text.bitmap_flags = bitmap_flags;
	macos9_background_clip_text.gradient_stops = gradient_stops;
	macos9_background_clip_text.gradient_angle = gradient_angle;
	macos9_background_clip_text.blend_mode = blend_mode;
	macos9_background_clip_text.blend_backdrop = blend_backdrop;
	macos9_background_clip_text.active = true;
	if (clip_text_state_seen < 12) {
		macsurf_debug_log_writef(
			"LIFE clip-text state fill=%d bitmap=%p tile=%d,%d",
			(int)macos9_background_clip_text.fill.fill_type,
			(void *)bitmap, bitmap_width, bitmap_height);
		clip_text_state_seen++;
	}
}

void macos9_background_clip_text_end(void)
{
	macos9_background_clip_text.active = false;
}

extern struct gui_window *macos9_paint_gw;
/* fixes77g - prefer macos9_paint_gw over GetPort+GetWRefCon. The old
 * pattern assumed the current port was the window and read gw from the
 * window's WRefCon. When fixes77f's offscreen GWorld back-buffer makes
 * the GWorld the current port mid-redraw, casting it to WindowRef and
 * calling GetWRefCon reads garbage memory and effective clips resolve
 * to (0,0,0,0). main.c sets macos9_paint_gw around browser_window_redraw
 * so the right gw is always available regardless of which port owns the
 * draw operations. */
static struct gui_window *macos9_find_gw_for_plot(void)
{
	GrafPtr port;
	if (macos9_paint_gw != NULL) return macos9_paint_gw;
	GetPort(&port);
	return (struct gui_window *)GetWRefCon((WindowRef)port);
}

/* fixes137 / fixes309: expose viewport top-left + dimensions in WINDOW
 * coordinates for background-attachment: fixed. html_redraw_background
 * substitutes (out_x, out_y, out_w, out_h) for the element box's (x, y,
 * width, height) so the subsequent bg-position math anchors the image
 * to the viewport instead of the element. The caller does that math in
 * the same coordinate frame NetSurf uses for the rest of paint  -  window
 * (not page)  -  so return content_rect.left / content_rect.top, NOT the
 * page-space scroll offset. The fixes308 diagnostic round revealed the
 * original (scroll_x, scroll_y) values were drifting the image down
 * along with scroll instead of anchoring it.
 * Returns 1 on success (out_x/y/w/h written), 0 if no current gw. */
int macos9_get_bg_fixed_origin(int *out_x, int *out_y, int *out_w, int *out_h)
{
	struct gui_window *gw = macos9_find_gw_for_plot();
	if (gw == NULL) {
		*out_x = 0;
		*out_y = 0;
		*out_w = 0;
		*out_h = 0;
		return 0;
	}
	*out_x = (int)gw->content_rect.left;
	*out_y = (int)gw->content_rect.top;
	*out_w = (int)(gw->content_rect.right - gw->content_rect.left);
	*out_h = (int)(gw->content_rect.bottom - gw->content_rect.top);
	return 1;
}

static RgnHandle macos9_push_clip(void)
{
	struct gui_window *gw;
	RgnHandle saved_clip;
	RgnHandle content_rgn;

	gw = macos9_find_gw_for_plot();
	if (gw == NULL) return NULL;

	saved_clip = NewRgn();
	GetClip(saved_clip);

	content_rgn = NewRgn();
	RectRgn(content_rgn, &gw->content_rect);
	SectRgn(saved_clip, content_rgn, content_rgn);
	SetClip(content_rgn);
	DisposeRgn(content_rgn);

	return saved_clip;
}

static void macos9_pop_clip(RgnHandle saved_clip)
{
	if (saved_clip == NULL) return;
	SetClip(saved_clip);
	DisposeRgn(saved_clip);
}
#endif

static nserror
macos9_plot_clip(const struct redraw_context *ctx, const struct rect *clip)
{
	Rect r;
#ifdef __MACOS9__
	struct gui_window *gw;
	RgnHandle new_clip;
	RgnHandle content_rgn;
	Rect effective;
#endif

	(void)ctx;
	if (clip == NULL) return NSERROR_OK;
	macos9_rect_from_ns(clip, &r);

#ifdef __MACOS9__
	gw = macos9_find_gw_for_plot();
	if (gw == NULL) {
		ClipRect(&r);
		return NSERROR_OK;
	}

	new_clip = NewRgn();
	RectRgn(new_clip, &r);

	content_rgn = NewRgn();
	RectRgn(content_rgn, &gw->content_rect);

	SectRgn(new_clip, content_rgn, new_clip);
	/* fixes951c  -  RgnHandle is opaque under Carbon (OPAQUE_TOOLBOX_STRUCTS);
	 * (**rgn).rgnBBox is no longer legal. GetRegionBounds is the accessor
	 * and returns the same rect. */
	GetRegionBounds(new_clip, &effective);

	/* fixes91: gated  -  see macsurf_prefix.h MACSURF_VERBOSE_PLOTLOG. */
#ifdef MACSURF_VERBOSE_PLOTLOG
	macsurf_debug_log_writef("plot_clip in=(%d,%d,%d,%d) content=(%d,%d,%d,%d) effective=(%d,%d,%d,%d)",
	       r.left, r.top, r.right, r.bottom,
	       gw->content_rect.left, gw->content_rect.top, gw->content_rect.right, gw->content_rect.bottom,
	       effective.left, effective.top, effective.right, effective.bottom);
#else
	(void)effective;
#endif

	SetClip(new_clip);

	DisposeRgn(content_rgn);
	DisposeRgn(new_clip);
#else
	ClipRect(&r);
#endif
	return NSERROR_OK;
}

static nserror
macos9_plot_arc(const struct redraw_context *ctx,
		const plot_style_t *pstyle,
		int x, int y, int radius, int angle1, int angle2)
{
	Rect r;
	RGBColor rgb;
	short start_angle;
	short arc_angle;
	(void)ctx;
	if (pstyle == NULL) return NSERROR_OK;
	if (radius <= 0) return NSERROR_OK;
	r.left   = (short)(x - radius);
	r.top    = (short)(y - radius);
	r.right  = (short)(x + radius);
	r.bottom = (short)(y + radius);
	/* NetSurf angles: degrees CCW from +X. QuickDraw: CW from +Y.
	 * Convert start = 90 - ns; sweep = ns1 - ns2 (negative of CCW). */
	start_angle = (short)(90 - angle1);
	arc_angle = (short)(angle1 - angle2);
	macos9_colour_to_rgb(pstyle->stroke_colour, &rgb);
	RGBForeColor(&rgb);
#ifdef __MACOS9__
	FrameArc(&r, start_angle, arc_angle);
#endif
	return NSERROR_OK;
}

static nserror
macos9_plot_disc(const struct redraw_context *ctx,
		 const plot_style_t *pstyle,
		 int x, int y, int radius)
{
	Rect r;
	RGBColor rgb;
	(void)ctx;
	if (pstyle == NULL) return NSERROR_OK;
	if (radius <= 0) return NSERROR_OK;
	r.left   = (short)(x - radius);
	r.top    = (short)(y - radius);
	r.right  = (short)(x + radius);
	r.bottom = (short)(y + radius);
	if (pstyle->fill_type != PLOT_OP_TYPE_NONE) {
		macos9_colour_to_rgb(pstyle->fill_colour, &rgb);
		RGBForeColor(&rgb);
#ifdef __MACOS9__
		PaintOval(&r);
#endif
	}
	if (pstyle->stroke_type != PLOT_OP_TYPE_NONE) {
		macos9_colour_to_rgb(pstyle->stroke_colour, &rgb);
		RGBForeColor(&rgb);
#ifdef __MACOS9__
		FrameOval(&r);
#endif
	}
	return NSERROR_OK;
}

#ifdef __MACOS9__
/* fixes960  -  dashed and dotted strokes.
 *
 * The core has always ASKED for these: redraw_border.c sets
 * PLOT_OP_TYPE_DASH / PLOT_OP_TYPE_DOT for CSS `border-style: dashed` and
 * `dotted`, and redraw.c does the same for <hr> rules. This plotter only ever
 * tested `stroke_type != PLOT_OP_TYPE_NONE`, so every dashed and dotted border
 * on the web has rendered SOLID in MacSurf.
 *
 * QuickDraw has no along-path dash, but it has a pen pattern, which is what
 * every classic Mac app used for this. The subtlety: the pattern is sampled in
 * SCREEN space, 8x8. A pattern whose rows are all identical (e.g. 0xCC eight
 * times) draws a horizontal dashed line correctly but makes a 1px-wide
 * VERTICAL line sample a single column - which is either always-on (solid) or
 * always-off (the border vanishes entirely). So the pattern has to alternate
 * on BOTH axes:
 *
 *   dash  0xCC,0xCC,0x33,0x33,...  2 on / 2 off horizontally, and the row pairs
 *                                  flip, so a vertical line also gets 2 on /
 *                                  2 off.
 *   dot   0xAA,0x55,...            classic 50% checkerboard: 1 on / 1 off in
 *                                  both axes.
 *
 * Diagonals come out stippled rather than truly dashed. That is the standard
 * QuickDraw compromise and reads correctly at border weights.
 *
 * Returns 1 if it changed the pen, so the caller knows to restore it. Leaving
 * a pen pattern set would tint every later stroke - the same class of bug as
 * the CopyBits foreground-colour leak documented in CLAUDE.md. */
static int macos9_stroke_pen_set(plot_operation_type_t t)
{
	static const Pattern pat_dash =
		{ { 0xCC, 0xCC, 0x33, 0x33, 0xCC, 0xCC, 0x33, 0x33 } };
	static const Pattern pat_dot =
		{ { 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55 } };

	if (t == PLOT_OP_TYPE_DASH) {
		PenPat(&pat_dash);
		return 1;
	}
	if (t == PLOT_OP_TYPE_DOT) {
		PenPat(&pat_dot);
		return 1;
	}
	return 0;
}

static void macos9_stroke_pen_reset(int changed)
{
	if (changed) PenNormal();
}
#endif

static nserror
macos9_plot_line(const struct redraw_context *ctx,
		 const plot_style_t *pstyle,
		 const struct rect *line)
{
	RGBColor rgb;
#ifdef __MACOS9__
	int pen_changed;
#endif
	(void)ctx;
	if (pstyle == NULL || line == NULL) return NSERROR_OK;
	macos9_colour_to_rgb(pstyle->stroke_colour, &rgb);
	RGBForeColor(&rgb);
#ifdef __MACOS9__
	{
		RgnHandle saved_clip = macos9_push_clip();
		pen_changed = macos9_stroke_pen_set(pstyle->stroke_type);
#endif
	MoveTo((short)line->x0, (short)line->y0);
	LineTo((short)line->x1, (short)line->y1);
#ifdef __MACOS9__
		macos9_stroke_pen_reset(pen_changed);
		macos9_pop_clip(saved_clip);
	}
#endif
	return NSERROR_OK;
}

/* fixes72: sin/cos lookup table for arbitrary angle rotation.
 *
 * 91-entry sin table at 1° steps (0° to 90°, inclusive) in Q15 fixed-point.
 * Range -32768..+32767. cos is recovered via cos(a) = sin(90 - a).
 *
 * Full-circle access uses quadrant symmetry:
 *   sin(  0..90 ) =  table[a]
 *   sin( 90..180) =  table[180 - a]
 *   sin(180..270) = -table[a - 180]
 *   sin(270..360) = -table[360 - a]
 *
 * Generated once, embedded as a const array (182 bytes).
 */
static const int16_t macos9_sin_q15_table[91] = {
	    0,   572,  1144,  1715,  2286,  2856,  3425,  3993,  4560,  5126,
	 5690,  6252,  6813,  7371,  7927,  8481,  9032,  9580, 10126, 10668,
	11207, 11743, 12275, 12803, 13328, 13848, 14365, 14876, 15384, 15886,
	16384, 16877, 17364, 17847, 18324, 18795, 19261, 19720, 20174, 20622,
	21063, 21498, 21926, 22348, 22763, 23170, 23571, 23965, 24351, 24730,
	25102, 25466, 25822, 26170, 26510, 26842, 27166, 27482, 27789, 28088,
	28378, 28660, 28932, 29197, 29452, 29698, 29935, 30163, 30382, 30592,
	30792, 30983, 31164, 31336, 31499, 31651, 31795, 31928, 32052, 32166,
	32270, 32365, 32449, 32524, 32588, 32643, 32688, 32723, 32748, 32763,
	32767
};

static int macos9_sin_q15(int deg)
{
	while (deg < 0) deg += 360;
	while (deg >= 360) deg -= 360;
	if (deg <=  90) return  macos9_sin_q15_table[deg];
	if (deg <= 180) return  macos9_sin_q15_table[180 - deg];
	if (deg <= 270) return -macos9_sin_q15_table[deg - 180];
	                return -macos9_sin_q15_table[360 - deg];
}

static int macos9_cos_q15(int deg)
{
	return macos9_sin_q15(deg + 90);
}

/* fixes71/72: unpack -macsurf-transform packed value.
 *   bits 31..16 rotation Q10.6 deg (signed)
 *   bits 15..8  translate-x int8 px
 *   bits 7..0   translate-y int8 px
 * Returns rotation in integer degrees (0..359) and translation pixels.
 * Sub-degree precision is dropped  -  V2 accuracy is 1° per step which is
 * imperceptible for typical CSS rotations. */
static void
macos9_transform_unpack(int transform,
			int *rot_deg, int *tx, int *ty)
{
	int32_t rot_q106;
	int deg;
	int is_pct = (int)(((uint32_t)transform >> 31) & 1);
	int8_t tx_px = (int8_t)((((uint32_t)transform) >> 8) & 0xff);
	int8_t ty_px = (int8_t)( ((uint32_t)transform)       & 0xff);

	/* fixes610: rotation is now a 15-bit signed field (bits 30..16); bit
	 * 31 flags percent-translate. A % translate resolves against the box's
	 * own size and is applied at the box level in redraw (html_redraw_box),
	 * so here  -  the background-fill and text-glyph transform paths  -  we
	 * zero the translate to avoid double-applying it. Pixel translate
	 * (is_pct == 0) is unchanged and still applied here as before. */
	rot_q106 = (int32_t)((((uint32_t)transform) >> 16) & 0x7fff);
	if (rot_q106 & 0x4000) rot_q106 -= 0x8000;   /* sign-extend 15-bit */
	deg = (int)(rot_q106 / 64);
	while (deg < 0)   deg += 360;
	while (deg >= 360) deg -= 360;

	*rot_deg = deg;
	if (is_pct) {
		*tx = 0;
		*ty = 0;
	} else {
		*tx = (int)tx_px;
		*ty = (int)ty_px;
	}
}

/* Rotate a single point around (cx, cy) by rot_deg degrees, then translate.
 * Q15 sin/cos lookup; integer-arithmetic only, no FPU dependency. */
static void
macos9_transform_point(int *px, int *py,
		       int cx, int cy, int rot_deg,
		       int tx, int ty)
{
	int dx = *px - cx;
	int dy = *py - cy;
	int s = macos9_sin_q15(rot_deg);
	int c = macos9_cos_q15(rot_deg);
	int nx, ny;

	/* Fast exact path for the four cardinal rotations  -  avoids
	 * accumulating Q15 rounding error on what should be pixel-perfect
	 * corners. */
	switch (rot_deg) {
	case 0:    nx = dx;  ny = dy;  break;
	case 90:   nx = -dy; ny = dx;  break;
	case 180:  nx = -dx; ny = -dy; break;
	case 270:  nx = dy;  ny = -dx; break;
	default:
		/* Q15 affine: new_x = dx*cos - dy*sin, scaled down by 2^15.
		 * +16384 rounds half-to-nearest. */
		nx = (dx * c - dy * s + 16384) >> 15;
		ny = (dx * s + dy * c + 16384) >> 15;
		break;
	}
	*px = cx + nx + tx;
	*py = cy + ny + ty;
}

static nserror
macos9_plot_rectangle(const struct redraw_context *ctx,
		      const plot_style_t *pstyle,
		      const struct rect *rectangle)
{
	Rect r;
	RGBColor rgb;
	/* fixes361h/362  -  snapshot the extra-shadow one-shot statics and
	 * clear them up-front. Any subsequent plot_rectangle call without
	 * a fresh macos9_set_box_shadow_{2,3} from redraw.c starts at 0,
	 * so the values can't leak across paints. */
	int32_t bsh2_local;
	int32_t bsh3_local;
	int32_t hstripe_local; /* fixes364 */
	int32_t dotgrid_local; /* fixes365c */
	/* fixes365b  -  extended-linear-gradient one-shots. */
	const int32_t *grad_stops_local;
	uint16_t grad_angle_local;
	uint8_t grad_blend_mode_local;
	colour grad_blend_backdrop_local;

	(void)ctx;

	if (pstyle == NULL || rectangle == NULL)
		return NSERROR_OK;

	bsh2_local = macos9_box_shadow_2_oneshot;
	bsh3_local = macos9_box_shadow_3_oneshot;
	hstripe_local = macos9_hstripe_bg_oneshot; /* fixes364 */
	dotgrid_local = macos9_dotgrid_oneshot; /* fixes365c */
	grad_stops_local = macos9_gradient_stops_oneshot; /* fixes365b */
	grad_angle_local = macos9_gradient_angle_oneshot; /* fixes365b */
	grad_blend_mode_local = macos9_gradient_blend_mode_oneshot;
	grad_blend_backdrop_local = macos9_gradient_blend_backdrop_oneshot;
	macos9_box_shadow_2_oneshot = 0;
	macos9_box_shadow_3_oneshot = 0;
	macos9_hstripe_bg_oneshot = 0; /* fixes364 */
	macos9_dotgrid_oneshot = 0; /* fixes365c */
	macos9_gradient_stops_oneshot = NULL; /* fixes365b */
	macos9_gradient_angle_oneshot = 0; /* fixes365b */
	macos9_gradient_blend_mode_oneshot = CSS_BACKGROUND_BLEND_MODE_NORMAL;
	macos9_gradient_blend_backdrop_oneshot = 0;

	/* fixes366d/e  -  log the snapshot values regardless of branch so we
	 * can tell whether the one-shot pipeline is reaching the plotter
	 * or being consumed by an intermediate paint.
	 *
	 * fixes366e: only log when ANY value is non-zero (i.e. when this
	 * paint actually inherited a one-shot). Previously capped at 8
	 * calls flat which burned the budget on early chrome paints before
	 * any content setters had fired. Cap raised to 32 of the interesting
	 * cases since they're rare in practice. */
	if (snapshot_seen < 32 &&
	    (hstripe_local != 0 || dotgrid_local != 0 ||
	     grad_stops_local != NULL || grad_angle_local != 0)) {
		macsurf_debug_log_writef(
			"plot: snapshot hstripe=%ld dotgrid=%ld grad_stops=%p angle=%d",
			(long)hstripe_local,
			(long)dotgrid_local,
			(void *)grad_stops_local,
			(int)grad_angle_local);
		snapshot_seen++;
	}

	macos9_plot_rect_count++;
	macos9_rect_from_ns(rectangle, &r);

#ifdef __MACOS9__
	/* fixes71 - transform-aware rectangle. When the box has a
	 * non-identity -macsurf-transform, build a 4-corner polygon
	 * rotated around the rectangle's centre, then fill/frame it.
	 * Skipped for identity (transform == 0) so the fast path stays
	 * untouched for the 99% case. */
	if (pstyle->transform != 0 ||
	    (pstyle->transform_b != 0 && pstyle->transform_b != (int)0x01000100)) {
		int rot_deg, tx, ty;
		int cx, cy;
		int x[4], y[4];
		int sx_q88, sy_q88;
		PolyHandle poly;
		RgnHandle saved_clip;
		int i;

		macos9_transform_unpack(pstyle->transform, &rot_deg, &tx, &ty);

		/* fixes73 / fixes73b: unpack scale from transform_b. Identity
		 * sentinel is 0x01000100 = (1.0, 1.0). If transform_b is zero
		 * (uninitialised plot_style_t struct from a code path that
		 * predates fixes73), treat it as identity  -  earlier code did
		 * an early-return here which killed every transformed draw
		 * whose plot_style went through the zero-fill struct init. */
		sx_q88 = (int)((((uint32_t)pstyle->transform_b) >> 16) & 0xffff);
		sy_q88 = (int)( ((uint32_t)pstyle->transform_b)        & 0xffff);
		if (sx_q88 == 0) sx_q88 = 256;
		if (sy_q88 == 0) sy_q88 = 256;

		if (rot_deg != 0 || tx != 0 || ty != 0 ||
		    sx_q88 != 256 || sy_q88 != 256) {
			cx = (r.left + r.right) / 2;
			cy = (r.top  + r.bottom) / 2;
			x[0] = r.left;  y[0] = r.top;
			x[1] = r.right; y[1] = r.top;
			x[2] = r.right; y[2] = r.bottom;
			x[3] = r.left;  y[3] = r.bottom;
			/* fixes73: pre-rotation scale around centre. */
			if (sx_q88 != 256 || sy_q88 != 256) {
				for (i = 0; i < 4; i++) {
					int dx = x[i] - cx;
					int dy = y[i] - cy;
					x[i] = cx + (dx * sx_q88) / 256;
					y[i] = cy + (dy * sy_q88) / 256;
				}
			}
			for (i = 0; i < 4; i++) {
				macos9_transform_point(&x[i], &y[i],
					cx, cy, rot_deg, tx, ty);
			}

			/* fixes73e: widen clip for transformed paint. The standard
			 * push_clip narrows the clip to (current AND content_rect),
			 * and NetSurf's redraw tightens the current clip to each
			 * box's layout slot before plot.rectangle runs - so a
			 * scale > 1 fill paints outside the slot and immediately
			 * gets cut. For transform we want to paint freely within
			 * the whole content area; save the existing clip, replace
			 * it with content_rect, then restore. Other plot paths
			 * keep using the tight clip - only the transform branch
			 * needs the wider scope. */
			{
				struct gui_window *gw;
				RgnHandle wide_clip;

				gw = macos9_find_gw_for_plot();

				saved_clip = NewRgn();
				GetClip(saved_clip);
				if (gw != NULL) {
					wide_clip = NewRgn();
					RectRgn(wide_clip, &gw->content_rect);
					SetClip(wide_clip);
					DisposeRgn(wide_clip);
				}
			}
			poly = OpenPoly();
			if (poly != NULL) {
				MoveTo((short)x[0], (short)y[0]);
				LineTo((short)x[1], (short)y[1]);
				LineTo((short)x[2], (short)y[2]);
				LineTo((short)x[3], (short)y[3]);
				LineTo((short)x[0], (short)y[0]);
				ClosePoly();
				if (pstyle->fill_type != PLOT_OP_TYPE_NONE) {
					macos9_colour_to_rgb(pstyle->fill_colour, &rgb);
					RGBForeColor(&rgb);
					PaintPoly(poly);
				}
				if (pstyle->stroke_type != PLOT_OP_TYPE_NONE) {
					macos9_colour_to_rgb(pstyle->stroke_colour, &rgb);
					RGBForeColor(&rgb);
					FramePoly(poly);
				}
				KillPoly(poly);
			}
			if (saved_clip != NULL) {
				SetClip(saved_clip);
				DisposeRgn(saved_clip);
			}
			return NSERROR_OK;
		}
	}
#endif

	/* fixes74b diagnostic: log EVERY non-solid rectangle so we can see
	 * whether gradients (linear or radial) are reaching the plotter.
	 * If a swatch with -macsurf-gradient ends up with fill_type=1
	 * (SOLID) here, the cascade dropped the SET status  -  parser or
	 * cascade bug, not plotter. */
	if (pstyle->fill_type == PLOT_OP_TYPE_LINEAR_GRADIENT ||
	    pstyle->fill_type == PLOT_OP_TYPE_LINEAR_GRADIENT_H ||
	    pstyle->fill_type == PLOT_OP_TYPE_RADIAL_GRADIENT) {
		macsurf_debug_log_writef(
			"GRADIENT plot_rect[%d] ft=%d fill=%d/%d/%d fill2=%d/%d/%d at (%d,%d,%d,%d)",
			(int)macos9_plot_rect_count,
			(int)pstyle->fill_type,
			(int)((pstyle->fill_colour >>  0) & 0xff),
			(int)((pstyle->fill_colour >>  8) & 0xff),
			(int)((pstyle->fill_colour >> 16) & 0xff),
			(int)((pstyle->fill_colour2 >>  0) & 0xff),
			(int)((pstyle->fill_colour2 >>  8) & 0xff),
			(int)((pstyle->fill_colour2 >> 16) & 0xff),
			(int)r.left, (int)r.top, (int)r.right, (int)r.bottom);
	}
	/* Diagnostic: dump fill / stroke colour + rect. fixes219 raises
	 * the cap from 8 to 300 so we can catch the body-bg paint colour
	 * on real pages (mactrove emits ~188 rects per redraw). Flip back
	 * to 8 once the grey-bg investigation is closed. */
	if (macos9_plot_rect_count <= 300) {
		unsigned int fr = (unsigned int)((pstyle->fill_colour >>  0) & 0xff);
		unsigned int fg = (unsigned int)((pstyle->fill_colour >>  8) & 0xff);
		unsigned int fb = (unsigned int)((pstyle->fill_colour >> 16) & 0xff);
		unsigned int sr = (unsigned int)((pstyle->stroke_colour >>  0) & 0xff);
		unsigned int sg = (unsigned int)((pstyle->stroke_colour >>  8) & 0xff);
		unsigned int sb = (unsigned int)((pstyle->stroke_colour >> 16) & 0xff);
		macsurf_debug_log_writef(
			"plot_rect[%d] fill=%d/%d/%d ft=%d stroke=%d/%d/%d st=%d op=%d at (%d,%d,%d,%d)",
			(int)macos9_plot_rect_count,
			(int)fr, (int)fg, (int)fb, (int)pstyle->fill_type,
			(int)sr, (int)sg, (int)sb, (int)pstyle->stroke_type,
			(int)pstyle->opacity,
			(int)r.left, (int)r.top, (int)r.right, (int)r.bottom);
	}

#ifdef __MACOS9__
	/* Use RoundRect when border_radius is set (fixes172). */
	if (pstyle->border_radius > 0) {
		short ovalSize = (short)(pstyle->border_radius >> PLOT_STYLE_RADIX);
		RgnHandle saved_clip;
		if (ovalSize < 1) ovalSize = 1;
		if (ovalSize > 32767) ovalSize = 32767;
		saved_clip = macos9_push_clip();
		if (pstyle->fill_type != PLOT_OP_TYPE_NONE) {
			macos9_colour_to_rgb(pstyle->fill_colour, &rgb);
			RGBForeColor(&rgb);
			PaintRoundRect(&r, ovalSize, ovalSize);
		}
		if (pstyle->stroke_type != PLOT_OP_TYPE_NONE) {
			macos9_colour_to_rgb(pstyle->stroke_colour, &rgb);
			RGBForeColor(&rgb);
			FrameRoundRect(&r, ovalSize, ovalSize);
		}
		macos9_pop_clip(saved_clip);
		return NSERROR_OK;
	}
#endif

	/* box-shadow: paint a slightly-offset grey rect BEHIND the fill.
	 * QuickDraw has no blur primitive, so we approximate with a
	 * solid offset rect at 50% grey - the recognisable Mac OS
	 * floating-window shadow look. */
#ifdef __MACOS9__
	if ((pstyle->box_shadow != 0 || pstyle->box_shadow_y != 0) &&
	    pstyle->fill_type != PLOT_OP_TYPE_NONE &&
	    !pstyle->box_shadow_inset) {
		short hoff = (short)(pstyle->box_shadow   >> PLOT_STYLE_RADIX);
		short voff = (short)(pstyle->box_shadow_y >> PLOT_STYLE_RADIX);
		/* fixes48 - defensive clamp. */
		if (hoff < -16) hoff = -16;
		if (hoff >  16) hoff =  16;
		if (voff < -16) voff = -16;
		if (voff >  16) voff =  16;
		if (hoff != 0 || voff != 0) {
			RGBColor sh;
			Rect s;
			RgnHandle saved_clip;
			if (pstyle->box_shadow_color != 0) {
				macos9_colour_to_rgb(
					pstyle->box_shadow_color, &sh);
			} else {
				sh.red = sh.green = sh.blue = 0x6666;
			}
			s = r;
			s.left  = (short)(s.left  + hoff);
			s.right = (short)(s.right + hoff);
			s.top    = (short)(s.top    + voff);
			s.bottom = (short)(s.bottom + voff);
			RGBForeColor(&sh);
			saved_clip = macos9_push_clip();
			PaintRect(&s);
			macos9_pop_clip(saved_clip);
		}
	}
	/* fixes362  -  outer drop shadow from the box_shadow_3 one-shot
	 * static. Paints BEFORE the box fill (same timing as the first
	 * outset shadow above). Common Platinum pattern is two inset
	 * bevels + one outer drop, with the drop providing a subtle
	 * floating effect on cards / buttons. */
	if (bsh3_local != 0 && pstyle->fill_type != PLOT_OP_TYPE_NONE) {
		bool inset3 = (((uint32_t)bsh3_local) & 0x8000) != 0;
		if (!inset3) {
			int8_t h3 = (int8_t)((((uint32_t)bsh3_local) >> 24) & 0xff);
			int8_t v3 = (int8_t)((((uint32_t)bsh3_local) >> 16) & 0xff);
			uint16_t rgb555_3 = (uint16_t)(((uint32_t)bsh3_local) & 0x7fff);
			short hoff3 = (short)h3;
			short voff3 = (short)v3;
			if (hoff3 < -16) hoff3 = -16;
			if (hoff3 >  16) hoff3 =  16;
			if (voff3 < -16) voff3 = -16;
			if (voff3 >  16) voff3 =  16;
			if (rgb555_3 != 0 && (hoff3 != 0 || voff3 != 0)) {
				uint8_t r5b = (uint8_t)((rgb555_3 >> 10) & 0x1f);
				uint8_t g5b = (uint8_t)((rgb555_3 >>  5) & 0x1f);
				uint8_t b5b = (uint8_t)((rgb555_3      ) & 0x1f);
				RGBColor sh3;
				Rect s3;
				RgnHandle saved_clip3;
				sh3.red   = (unsigned short)
					(((unsigned int)((r5b << 3) | (r5b >> 2))) * 0x0101);
				sh3.green = (unsigned short)
					(((unsigned int)((g5b << 3) | (g5b >> 2))) * 0x0101);
				sh3.blue  = (unsigned short)
					(((unsigned int)((b5b << 3) | (b5b >> 2))) * 0x0101);
				s3 = r;
				s3.left   = (short)(s3.left   + hoff3);
				s3.right  = (short)(s3.right  + hoff3);
				s3.top    = (short)(s3.top    + voff3);
				s3.bottom = (short)(s3.bottom + voff3);
				RGBForeColor(&sh3);
				saved_clip3 = macos9_push_clip();
				PaintRect(&s3);
				macos9_pop_clip(saved_clip3);
			}
		}
	}
#endif

	/* fixes365b  -  diagonal / 3-stop linear gradient. Triggered when the
	 * fill is LINEAR_GRADIENT (cardinal or otherwise) AND the redraw
	 * side has pushed the extended one-shot descriptor. Iterates the
	 * box pixel by pixel, computes a per-pixel `t` along the requested
	 * 45/135/225/315 axis, then interpolates colour from the 2-stop or
	 * 3-stop palette with explicit positions.
	 *
	 * Expensive vs the row-fill cardinal path, but mactrove's close
	 * box is 11x11 px so the cost is negligible. Falls through to the
	 * cardinal path when grad_stops_local is NULL. */
#ifdef __MACOS9__
	if ((pstyle->fill_type == PLOT_OP_TYPE_LINEAR_GRADIENT ||
	     pstyle->fill_type == PLOT_OP_TYPE_LINEAR_GRADIENT_H) &&
	    grad_stops_local != NULL) {
		RGBColor cA, cB, cC;
		RGBColor cur;
		long box_w = (long)(r.right - r.left);
		long box_h = (long)(r.bottom - r.top);
		long denom;
		long row, col;
		int n_stops;
		int32_t p0_eff, p1_eff, p2_eff;
		int angle_norm;
		RgnHandle saved_clip;
		PenState saved_pen;
		Rect pixel;

		/* Decode angle (low 16 bits) and stop count (high 16 bits)
		 * from the packed descriptor word. The parser packs both so
		 * the painter doesn't have to infer stop count from
		 * potentially-zero col2/pos2 values. */
		{
			uint32_t raw = (uint32_t)grad_stops_local[0];
			angle_norm = (int)(raw & 0xffffu);
			n_stops = (int)((raw >> 16) & 0xffffu);
			if (n_stops < 2 || n_stops > 3) {
				/* Legacy/zero-encoded fallback. */
				if (grad_stops_local[3] == 0 &&
				    grad_stops_local[6] == 0) {
					n_stops = 2;
				} else {
					n_stops = 3;
				}
			}
		}
		/* The redraw side also passes the bare angle via the
		 * separate one-shot. Prefer the embedded form, but use the
		 * one-shot as backup when the descriptor didn't encode the
		 * high bits (e.g. inherited / older bytecode). */
		if (angle_norm == 0 && grad_angle_local != 0) {
			angle_norm = (int)grad_angle_local;
		}
		angle_norm = angle_norm % 360;
		if (angle_norm < 0) angle_norm += 360;
		{
			if (diag_paint_seen < 5) {
				macsurf_debug_log_writef(
					"LIFE gradient ext angle=%d stops=%d rect=%d,%d,%d,%d",
					angle_norm, n_stops, (int)r.left,
					(int)r.top, (int)r.right, (int)r.bottom);
				diag_paint_seen++;
			}
		}
		p0_eff = grad_stops_local[1];
		p1_eff = grad_stops_local[2];
		p2_eff = grad_stops_local[3];

		{
			colour ext_a = nscss_color_to_ns((uint32_t)grad_stops_local[4]);
			colour ext_b = nscss_color_to_ns((uint32_t)grad_stops_local[5]);
			colour ext_c = nscss_color_to_ns((uint32_t)grad_stops_local[6]);
			if (grad_blend_mode_local > CSS_BACKGROUND_BLEND_MODE_NORMAL) {
				ext_a = macos9_background_blend_colour(ext_a,
						grad_blend_backdrop_local, grad_blend_mode_local);
				ext_b = macos9_background_blend_colour(ext_b,
						grad_blend_backdrop_local, grad_blend_mode_local);
				ext_c = macos9_background_blend_colour(ext_c,
						grad_blend_backdrop_local, grad_blend_mode_local);
			}
			macos9_colour_to_rgb(ext_a, &cA);
			macos9_colour_to_rgb(ext_b, &cB);
			macos9_colour_to_rgb(ext_c, &cC);
		}

		saved_clip = macos9_push_clip();
		GetPenState(&saved_pen);
		PenNormal();

		if (box_w < 1) box_w = 1;
		if (box_h < 1) box_h = 1;

		/* Diagonal axis length, in pixels. For the 4 supported
		 * angles the diagonal is the same: w + h - 2 covers the
		 * full corner-to-corner span. denom is reused per row. */
		denom = box_w + box_h - 2;
		if (denom < 1) denom = 1;

		for (row = 0; row < box_h; row++) {
			for (col = 0; col < box_w; col++) {
				long t256;
				long axis;
				long t10000;
				int seg_lo, seg_hi;
				int32_t pos_lo, pos_hi;
				RGBColor *cLo;
				RGBColor *cHi;
				long span_p;
				long local_t;

				/* Per-axis pixel distance along the gradient
				 * line. 45 = top-left to bottom-right,
				 * 135 = top-right to bottom-left,
				 * 225 = bottom-right to top-left,
				 * 315 = bottom-left to top-right.
				 *
				 * Cardinal angles also handled (the cardinal
				 * branch below is the fast path; this only
				 * fires when the rule needs the side-channel,
				 * which can include 2-stop cardinal with
				 * non-default positions). */
				switch (angle_norm) {
				case 45:
					axis = col + row;
					break;
				case 135:
					axis = (box_w - 1 - col) + row;
					break;
				case 225:
					axis = (box_w - 1 - col) +
						(box_h - 1 - row);
					break;
				case 315:
					axis = col + (box_h - 1 - row);
					break;
				case 90:
					axis = (col * (box_w + box_h - 2)) /
						((box_w > 1) ? (box_w - 1) : 1);
					break;
				case 270:
					axis = ((box_w - 1 - col) *
						(box_w + box_h - 2)) /
						((box_w > 1) ? (box_w - 1) : 1);
					break;
				case 0:
					axis = ((box_h - 1 - row) *
						(box_w + box_h - 2)) /
						((box_h > 1) ? (box_h - 1) : 1);
					break;
				case 180:
				default:
					axis = (row * (box_w + box_h - 2)) /
						((box_h > 1) ? (box_h - 1) : 1);
					break;
				}
				if (axis < 0) axis = 0;
				if (axis > denom) axis = denom;
				t10000 = (axis * 10000L) / denom;

				/* Find which segment t falls in, then
				 * interpolate between the adjacent stops. */
				if (n_stops == 2) {
					seg_lo = 0;
					seg_hi = 1;
					pos_lo = p0_eff;
					pos_hi = p1_eff;
					cLo = &cA;
					cHi = &cB;
				} else {
					if (t10000 <= p1_eff) {
						seg_lo = 0;
						seg_hi = 1;
						pos_lo = p0_eff;
						pos_hi = p1_eff;
						cLo = &cA;
						cHi = &cB;
					} else {
						seg_lo = 1;
						seg_hi = 2;
						pos_lo = p1_eff;
						pos_hi = p2_eff;
						cLo = &cB;
						cHi = &cC;
					}
				}
				(void)seg_lo; (void)seg_hi;
				span_p = (long)pos_hi - (long)pos_lo;
				if (span_p <= 0) span_p = 1;
				local_t = (t10000 - (long)pos_lo);
				if (local_t < 0) local_t = 0;
				if (local_t > span_p) local_t = span_p;
				t256 = (local_t * 256L) / span_p;

				cur.red = (unsigned short)
					(((long)cLo->red * (256L - t256) +
					  (long)cHi->red * t256) >> 8);
				cur.green = (unsigned short)
					(((long)cLo->green * (256L - t256) +
					  (long)cHi->green * t256) >> 8);
				cur.blue = (unsigned short)
					(((long)cLo->blue * (256L - t256) +
					  (long)cHi->blue * t256) >> 8);
				RGBForeColor(&cur);
				SetRect(&pixel, (short)(r.left + col),
						(short)(r.top + row),
						(short)(r.left + col + 1),
						(short)(r.top + row + 1));
				PaintRect(&pixel);
			}
		}

		SetPenState(&saved_pen);
		macos9_pop_clip(saved_clip);
		if (pstyle->stroke_type != PLOT_OP_TYPE_NONE) {
			macos9_colour_to_rgb(pstyle->stroke_colour, &rgb);
			RGBForeColor(&rgb);
			saved_clip = macos9_push_clip();
			FrameRect(&r);
			macos9_pop_clip(saved_clip);
		}
		return NSERROR_OK;
	}
#endif

	/* fixes47 - real vertical linear gradient (top stop -> bottom).
	 * fixes48 - horizontal variant fills left-to-right.
	 * Both interpolate in 16-bit RGB with 1.8 fixed-point t, all
	 * 32-bit long math (CW8 PPC long-long codegen is unsafe). */
#ifdef __MACOS9__
	if (pstyle->fill_type == PLOT_OP_TYPE_LINEAR_GRADIENT ||
	    pstyle->fill_type == PLOT_OP_TYPE_LINEAR_GRADIENT_H) {
		bool horiz = (pstyle->fill_type ==
				PLOT_OP_TYPE_LINEAR_GRADIENT_H);
		RGBColor c1;
		RGBColor c2;
		RGBColor cur;
		long span;
		long i;
		long denom;
		RgnHandle saved_clip;
		macos9_colour_to_rgb(pstyle->fill_colour,  &c1);
		macos9_colour_to_rgb(pstyle->fill_colour2, &c2);
		saved_clip = macos9_push_clip();
		span = horiz ? (long)(r.right - r.left)
			     : (long)(r.bottom - r.top);
		denom = (span > 1) ? (span - 1) : 1;
		for (i = 0; i < span; i++) {
			long t = (i * 256L) / denom;        /* 0..256 */
			long inv = 256L - t;
			cur.red   = (unsigned short)
				(((long)c1.red   * inv + (long)c2.red   * t) >> 8);
			cur.green = (unsigned short)
				(((long)c1.green * inv + (long)c2.green * t) >> 8);
			cur.blue  = (unsigned short)
				(((long)c1.blue  * inv + (long)c2.blue  * t) >> 8);
			RGBForeColor(&cur);
			if (horiz) {
				MoveTo((short)(r.left + i), r.top);
				LineTo((short)(r.left + i),
						(short)(r.bottom - 1));
			} else {
				MoveTo(r.left, (short)(r.top + i));
				LineTo((short)(r.right - 1),
						(short)(r.top + i));
			}
		}
		macos9_pop_clip(saved_clip);
		if (pstyle->stroke_type != PLOT_OP_TYPE_NONE) {
			macos9_colour_to_rgb(pstyle->stroke_colour, &rgb);
			RGBForeColor(&rgb);
			saved_clip = macos9_push_clip();
			FrameRect(&r);
			macos9_pop_clip(saved_clip);
		}
		return NSERROR_OK;
	}

	/* fixes74 - radial gradient. fill_colour is the centre colour,
	 * fill_colour2 is the edge colour. Paint the bounding rectangle
	 * with c2 first, then a stack of progressively smaller ovals from
	 * the box bounds down toward zero size, each interpolating from
	 * c2 (outer) to c1 (centre). 24 rings give visibly smooth
	 * concentric banding on 8-bit displays. The ovals fit the box
	 * bounds so non-square boxes render an ellipse, matching CSS
	 * radial-gradient's default ellipse-farthest-corner shape. */
	if (pstyle->fill_type == PLOT_OP_TYPE_RADIAL_GRADIENT) {
		RGBColor c1;
		RGBColor c2;
		RGBColor cur;
		RgnHandle saved_clip;
		long width;
		long height;
		long rings = 24;
		long i;
		Rect grad_rect;
		macos9_colour_to_rgb(pstyle->fill_colour,  &c1);
		macos9_colour_to_rgb(pstyle->fill_colour2, &c2);
		saved_clip = macos9_push_clip();
		grad_rect = r;
		/* fixes345  -  when the rule carried a size+position prefix
		 * (radial_set true), build a smaller grad_rect centered at
		 * the requested position so the rings draw at the author's
		 * intended location instead of filling the whole bounding
		 * rect. The original `r` is still used for the c2 fill so
		 * the rest of the element gets the edge colour. */
		if (pstyle->radial_set) {
			long box_w = (long)(r.right - r.left);
			long box_h = (long)(r.bottom - r.top);
			long sx = (pstyle->radial_sx >= 0) ?
				(long)pstyle->radial_sx : box_w;
			long sy = (pstyle->radial_sy >= 0) ?
				(long)pstyle->radial_sy : box_h;
			long cx_pct = (pstyle->radial_px >= -10000) ?
				(long)pstyle->radial_px : 5000;
			long cy_pct = (pstyle->radial_py >= -10000) ?
				(long)pstyle->radial_py : 5000;
			long cx = r.left + (box_w * cx_pct) / 10000;
			long cy = r.top  + (box_h * cy_pct) / 10000;
			grad_rect.left   = (short)(cx - sx);
			grad_rect.right  = (short)(cx + sx);
			grad_rect.top    = (short)(cy - sy);
			grad_rect.bottom = (short)(cy + sy);
		}
		width  = (long)(grad_rect.right - grad_rect.left);
		height = (long)(grad_rect.bottom - grad_rect.top);
		if (width < 2 || height < 2) {
			RGBForeColor(&c2);
			PaintRect(&r);
		} else {
			/* Fill background with c2 first (so corners outside
			 * the largest oval show the edge colour). */
			RGBForeColor(&c2);
			PaintRect(&r);
			/* Concentric ovals from outer to inner. Ring 0 is
			 * the full bounding rect; ring N-1 is a 1px speck
			 * at the centre. Colour walks from c2 (outer) to c1
			 * (centre). */
			for (i = 0; i < rings; i++) {
				Rect ring;
				long inset_x = (width  * i) / (rings * 2);
				long inset_y = (height * i) / (rings * 2);
				long t = (i * 256L) / (rings - 1);  /* 0..256 */
				long inv = 256L - t;
				/* fixes345  -  when radial_set, rings inset from
				 * grad_rect (the author-placed ellipse). When
				 * not set, rings inset from r (existing
				 * behaviour: fill bounding rect). */
				ring.left   = (short)(grad_rect.left   + inset_x);
				ring.right  = (short)(grad_rect.right  - inset_x);
				ring.top    = (short)(grad_rect.top    + inset_y);
				ring.bottom = (short)(grad_rect.bottom - inset_y);
				if (ring.right - ring.left < 1 ||
				    ring.bottom - ring.top < 1) break;
				cur.red   = (unsigned short)
					(((long)c2.red   * inv + (long)c1.red   * t) >> 8);
				cur.green = (unsigned short)
					(((long)c2.green * inv + (long)c1.green * t) >> 8);
				cur.blue  = (unsigned short)
					(((long)c2.blue  * inv + (long)c1.blue  * t) >> 8);
				RGBForeColor(&cur);
				PaintOval(&ring);
			}
		}
		macos9_pop_clip(saved_clip);
		if (pstyle->stroke_type != PLOT_OP_TYPE_NONE) {
			macos9_colour_to_rgb(pstyle->stroke_colour, &rgb);
			RGBForeColor(&rgb);
			saved_clip = macos9_push_clip();
			FrameRect(&r);
			macos9_pop_clip(saved_clip);
		}
		return NSERROR_OK;
	}
#endif

	if (pstyle->fill_type != PLOT_OP_TYPE_NONE &&
	    pstyle->fill_type != PLOT_OP_TYPE_LINEAR_GRADIENT &&
	    pstyle->fill_type != PLOT_OP_TYPE_LINEAR_GRADIENT_H &&
	    pstyle->fill_type != PLOT_OP_TYPE_RADIAL_GRADIENT) {
		/* fixes49 - opacity bucket. plot_style_fixed value with
		 * PLOT_STYLE_SCALE (=1024) for opaque. Below ~5% don't
		 * paint at all. Between 5% and ~85% paint with a stipple
		 * pattern that approximates alpha on 8-bit displays:
		 *   < 5%      skip
		 *   5..35%    ltGray (sparse foreground)
		 *   35..60%   gray  (50/50)
		 *   60..85%   dkGray (dense foreground)
		 *   > 85%     full solid
		 * Patterns paint foreground bits where the pattern is 1
		 * and background bits where the pattern is 0. */
		plot_style_fixed op = pstyle->opacity;
		bool stipple = false;
#ifdef __MACOS9__
		Pattern stipple_pat;
#endif
		/* fixes223 reverted: many code paths leave pstyle.opacity = 0
		 * by default (calloc / memset of plot_style structs that
		 * don't go through redraw.c's css_computed_opacity check),
		 * and treating 0 as "skip" zeroes out borders and chrome.
		 * Keep the uninit->opaque fallback. The dark-grey-on-mactrove
		 * regression is not from this code path. */
		if (op == 0) op = (plot_style_fixed)PLOT_STYLE_SCALE; /* uninit -> opaque */
		if (op < (plot_style_fixed)(PLOT_STYLE_SCALE / 20)) {
			/* < 5% - skip painting entirely. */
			goto opacity_done;
		}
#ifdef __MACOS9__
		if (op < (plot_style_fixed)((PLOT_STYLE_SCALE * 35) / 100)) {
			GetIndPattern(&stipple_pat, sysPatListID, 2);
			/* ltGray approx; if GetIndPattern fails the pattern
			 * is already zero-initialised which means solid bg. */
			stipple = true;
		} else if (op < (plot_style_fixed)((PLOT_STYLE_SCALE * 60) / 100)) {
			GetIndPattern(&stipple_pat, sysPatListID, 3);
			stipple = true;
		} else if (op < (plot_style_fixed)((PLOT_STYLE_SCALE * 85) / 100)) {
			GetIndPattern(&stipple_pat, sysPatListID, 4);
			stipple = true;
		}
#endif
		macos9_colour_to_rgb(pstyle->fill_colour, &rgb);
		RGBForeColor(&rgb);
#ifdef __MACOS9__
		if (stipple) {
			/* fixes220  -  explicitly set RGBBackColor to white before
			 * the stipple FillRect. The pattern paints FG at 1 bits
			 * and BG at 0 bits. If a prior DrawText / image-blit /
			 * etc. left BackColor at black, a 50% stipple of #cc
			 * over #00 averages to #66 dark grey instead of the
			 * intended translucent-over-white look  -  which is the
			 * "tables are too dark" bug. */
			RGBColor wht;
			RgnHandle saved_clip = macos9_push_clip();
			wht.red = 0xFFFF; wht.green = 0xFFFF; wht.blue = 0xFFFF;
			RGBBackColor(&wht);
			FillRect(&r, &stipple_pat);
			macos9_pop_clip(saved_clip);
		} else if ((hstripe_local & (int32_t)0x80000000) != 0) {
			/* fixes364  -  alternating-row horizontal stripes from
			 * `-macsurf-hstripe-bg: c1 c2`. Packed format:
			 *   bit 31      = set
			 *   bits 15..29 = c2 RGB555
			 *   bits 0..14  = c1 RGB555
			 * The mactrove Platinum title-bar pattern is the true
			 * 3px-period pinstripe from the CSS:
			 *   #ffffff 0..1px, #cccccc 1..2px, #ffffff 2..3px
			 * i.e. 2px of c1 (white) then 1px of c2 (gray) per
			 * period. fixes366g paints that exactly  -  the 2px
			 * approximation used before was 50% gray and read far
			 * heavier than the genuine subtle Platinum pinstripe. */
			uint32_t up = (uint32_t)hstripe_local;
			uint32_t rgb1 = up & 0x7fff;
			uint32_t rgb2 = (up >> 15) & 0x7fff;
			RGBColor sc1;
			RGBColor sc2;
			short y;
			RgnHandle saved_clip;
			if (hstripe_paint_seen < 5) {
				macsurf_debug_log_write("plot: hstripe paint");
				hstripe_paint_seen++;
			}
			sc1.red   = (unsigned short)(((rgb1 >> 10) & 0x1f) << 11);
			sc1.green = (unsigned short)(((rgb1 >> 5)  & 0x1f) << 11);
			sc1.blue  = (unsigned short)(( rgb1        & 0x1f) << 11);
			sc2.red   = (unsigned short)(((rgb2 >> 10) & 0x1f) << 11);
			sc2.green = (unsigned short)(((rgb2 >> 5)  & 0x1f) << 11);
			sc2.blue  = (unsigned short)(( rgb2        & 0x1f) << 11);
			saved_clip = macos9_push_clip();
			for (y = r.top; y < r.bottom; y++) {
				/* 3px period: rows 0,2 = c1 (white),
				 * row 1 = c2 (gray). Matches the CSS
				 * repeating-linear-gradient exactly. */
				if (((y - r.top) % 3) == 1)
					RGBForeColor(&sc2);
				else
					RGBForeColor(&sc1);
				MoveTo(r.left, y);
				LineTo((short)(r.right - 1), y);
			}
			macos9_pop_clip(saved_clip);
		} else if ((dotgrid_local & (int32_t)0x80000000) != 0) {
			/* fixes365c  -  2x2 dot-grid pattern from
			 * `-macsurf-dotgrid: c1 c2`. Same packed format as the
			 * hstripe branch above (bit 31 set, bits 15..29 c2 RGB555,
			 * bits 0..14 c1 RGB555).
			 *
			 * Paint pattern (matches mactrove's two-crossed-1px-grad
			 * background): every column with x%2==0 gets a 1px
			 * vertical line in c1, every row with y%2==0 gets a 1px
			 * horizontal line in c2. The intersection lands in c2
			 * (last-write-wins), producing the dot-grid texture. */
			uint32_t up = (uint32_t)dotgrid_local;
			uint32_t rgb1 = up & 0x7fff;
			uint32_t rgb2 = (up >> 15) & 0x7fff;
			RGBColor sc1;
			RGBColor sc2;
			short x;
			short y;
			RgnHandle saved_clip;
			if (dotgrid_paint_seen < 5) {
				macsurf_debug_log_write("plot: dotgrid paint");
				dotgrid_paint_seen++;
			}
			sc1.red   = (unsigned short)(((rgb1 >> 10) & 0x1f) << 11);
			sc1.green = (unsigned short)(((rgb1 >> 5)  & 0x1f) << 11);
			sc1.blue  = (unsigned short)(( rgb1        & 0x1f) << 11);
			sc2.red   = (unsigned short)(((rgb2 >> 10) & 0x1f) << 11);
			sc2.green = (unsigned short)(((rgb2 >> 5)  & 0x1f) << 11);
			sc2.blue  = (unsigned short)(( rgb2        & 0x1f) << 11);
			saved_clip = macos9_push_clip();
			/* Vertical stripes (c1) at every even column. */
			RGBForeColor(&sc1);
			for (x = r.left; x < r.right; x++) {
				if (((x - r.left) & 1) == 0) {
					MoveTo(x, r.top);
					LineTo(x, (short)(r.bottom - 1));
				}
			}
			/* Horizontal stripes (c2) at every even row, overlaid. */
			RGBForeColor(&sc2);
			for (y = r.top; y < r.bottom; y++) {
				if (((y - r.top) & 1) == 0) {
					MoveTo(r.left, y);
					LineTo((short)(r.right - 1), y);
				}
			}
			macos9_pop_clip(saved_clip);
		} else {
			RgnHandle saved_clip = macos9_push_clip();
			PaintRect(&r);
			macos9_pop_clip(saved_clip);
		}
#ifdef __MACOS9__
		/* fixes361  -  proper inset box-shadow: paint thin edge lines
		 * along the inside of the box, NOT a full offset rectangle.
		 *
		 * CSS spec inverts the offset direction for inset shadows.
		 * For `box-shadow: inset h v 0 color`:
		 *   - h > 0: paint |h|-px line on the LEFT inside edge
		 *   - h < 0: paint |h|-px line on the RIGHT inside edge
		 *   - v > 0: paint |v|-px line on the TOP inside edge
		 *   - v < 0: paint |v|-px line on the BOTTOM inside edge
		 *
		 * Mactrove's Platinum windows declare a pair of inset
		 * shadows for 3D bevels:
		 *   inset -1px -1px 0 dark    -> 1px dark line on right+bottom
		 *   inset  1px  1px 0 light   -> 1px light line on top+left
		 * Only the FIRST shadow is currently parsed (single-shadow
		 * limitation), so this round paints the first inset's edges
		 * faithfully. A multi-shadow round can layer the second.
		 *
		 * Re-enabled after fixes225's blanket disable. The dark-wash
		 * regression was the `box_shadow_color == 0` fallback paint-
		 * dark-grey-everywhere path; now we SKIP the paint when the
		 * colour didn't round-trip from var() resolution. */
		if (pstyle->box_shadow_inset &&
		    pstyle->box_shadow_color != 0 &&
		    (pstyle->box_shadow != 0 || pstyle->box_shadow_y != 0)) {
			short hoff = (short)(pstyle->box_shadow   >> PLOT_STYLE_RADIX);
			short voff = (short)(pstyle->box_shadow_y >> PLOT_STYLE_RADIX);
			RGBColor sh;
			RgnHandle saved_clip;
			Rect edge;
			/* Clamp |offset| to box width/height so we never paint a
			 * line wider than the box itself. */
			short bw = (short)(r.right - r.left);
			short bh = (short)(r.bottom - r.top);
			if (hoff >  bw) hoff =  bw;
			if (hoff < -bw) hoff = -bw;
			if (voff >  bh) voff =  bh;
			if (voff < -bh) voff = -bh;

			macos9_colour_to_rgb(pstyle->box_shadow_color, &sh);
			RGBForeColor(&sh);
			saved_clip = macos9_push_clip();
			ClipRect(&r);

			/* Horizontal edge (left/right inside line). */
			if (hoff > 0) {
				edge = r;
				edge.right = (short)(edge.left + hoff);
				PaintRect(&edge);
			} else if (hoff < 0) {
				edge = r;
				edge.left = (short)(edge.right + hoff);
				PaintRect(&edge);
			}

			/* Vertical edge (top/bottom inside line). */
			if (voff > 0) {
				edge = r;
				edge.bottom = (short)(edge.top + voff);
				PaintRect(&edge);
			} else if (voff < 0) {
				edge = r;
				edge.top = (short)(edge.bottom + voff);
				PaintRect(&edge);
			}

			macos9_pop_clip(saved_clip);
		}

		/* fixes361h / 362  -  second + third inset box-shadows from
		 * the one-shot statics (snapshot at function entry into
		 * bsh2_local / bsh3_local). The outset path for bsh3 ran
		 * BEFORE the fill above; here we paint the inset branches
		 * if either was declared as `inset`. */
		{
			int32_t bsh2 = bsh2_local;
			if (bsh2 != 0) {
				int8_t h2 = (int8_t)((((uint32_t)bsh2) >> 24) & 0xff);
				int8_t v2 = (int8_t)((((uint32_t)bsh2) >> 16) & 0xff);
				bool inset2 = (((uint32_t)bsh2) & 0x8000) != 0;
				uint16_t rgb555_2 = (uint16_t)(((uint32_t)bsh2) & 0x7fff);
				if (inset2 && rgb555_2 != 0 && (h2 != 0 || v2 != 0)) {
					uint8_t r5b = (uint8_t)((rgb555_2 >> 10) & 0x1f);
					uint8_t g5b = (uint8_t)((rgb555_2 >>  5) & 0x1f);
					uint8_t b5b = (uint8_t)((rgb555_2      ) & 0x1f);
					RGBColor sh2;
					RgnHandle saved_clip2;
					Rect edge2;
					short hoff2 = (short)h2;
					short voff2 = (short)v2;
					short bw2 = (short)(r.right - r.left);
					short bh2 = (short)(r.bottom - r.top);
					if (hoff2 >  bw2) hoff2 =  bw2;
					if (hoff2 < -bw2) hoff2 = -bw2;
					if (voff2 >  bh2) voff2 =  bh2;
					if (voff2 < -bh2) voff2 = -bh2;
					sh2.red   = (unsigned short)
						(((unsigned int)((r5b << 3) | (r5b >> 2))) * 0x0101);
					sh2.green = (unsigned short)
						(((unsigned int)((g5b << 3) | (g5b >> 2))) * 0x0101);
					sh2.blue  = (unsigned short)
						(((unsigned int)((b5b << 3) | (b5b >> 2))) * 0x0101);
					RGBForeColor(&sh2);
					saved_clip2 = macos9_push_clip();
					ClipRect(&r);
					if (hoff2 > 0) {
						edge2 = r;
						edge2.right = (short)(edge2.left + hoff2);
						PaintRect(&edge2);
					} else if (hoff2 < 0) {
						edge2 = r;
						edge2.left = (short)(edge2.right + hoff2);
						PaintRect(&edge2);
					}
					if (voff2 > 0) {
						edge2 = r;
						edge2.bottom = (short)(edge2.top + voff2);
						PaintRect(&edge2);
					} else if (voff2 < 0) {
						edge2 = r;
						edge2.top = (short)(edge2.bottom + voff2);
						PaintRect(&edge2);
					}
					macos9_pop_clip(saved_clip2);
				}
			}

			/* fixes362  -  third inset (when the third shadow is
			 * declared `inset` rather than the typical outer drop). */
			{
				int32_t bsh3 = bsh3_local;
				if (bsh3 != 0) {
					int8_t h3 = (int8_t)((((uint32_t)bsh3) >> 24) & 0xff);
					int8_t v3 = (int8_t)((((uint32_t)bsh3) >> 16) & 0xff);
					bool inset3 = (((uint32_t)bsh3) & 0x8000) != 0;
					uint16_t rgb555_3 = (uint16_t)(((uint32_t)bsh3) & 0x7fff);
					if (inset3 && rgb555_3 != 0 && (h3 != 0 || v3 != 0)) {
						uint8_t r5b = (uint8_t)((rgb555_3 >> 10) & 0x1f);
						uint8_t g5b = (uint8_t)((rgb555_3 >>  5) & 0x1f);
						uint8_t b5b = (uint8_t)((rgb555_3      ) & 0x1f);
						RGBColor sh3;
						RgnHandle saved_clip3;
						Rect edge3;
						short hoff3 = (short)h3;
						short voff3 = (short)v3;
						short bw3 = (short)(r.right - r.left);
						short bh3 = (short)(r.bottom - r.top);
						if (hoff3 >  bw3) hoff3 =  bw3;
						if (hoff3 < -bw3) hoff3 = -bw3;
						if (voff3 >  bh3) voff3 =  bh3;
						if (voff3 < -bh3) voff3 = -bh3;
						sh3.red   = (unsigned short)
							(((unsigned int)((r5b << 3) | (r5b >> 2))) * 0x0101);
						sh3.green = (unsigned short)
							(((unsigned int)((g5b << 3) | (g5b >> 2))) * 0x0101);
						sh3.blue  = (unsigned short)
							(((unsigned int)((b5b << 3) | (b5b >> 2))) * 0x0101);
						RGBForeColor(&sh3);
						saved_clip3 = macos9_push_clip();
						ClipRect(&r);
						if (hoff3 > 0) {
							edge3 = r;
							edge3.right = (short)(edge3.left + hoff3);
							PaintRect(&edge3);
						} else if (hoff3 < 0) {
							edge3 = r;
							edge3.left = (short)(edge3.right + hoff3);
							PaintRect(&edge3);
						}
						if (voff3 > 0) {
							edge3 = r;
							edge3.bottom = (short)(edge3.top + voff3);
							PaintRect(&edge3);
						} else if (voff3 < 0) {
							edge3 = r;
							edge3.top = (short)(edge3.bottom + voff3);
							PaintRect(&edge3);
						}
						macos9_pop_clip(saved_clip3);
					}
				}
			}
		}
#endif
#else
		PaintRect(&r);
#endif
opacity_done:
		;
	}

	if (pstyle->stroke_type != PLOT_OP_TYPE_NONE) {
		macos9_colour_to_rgb(pstyle->stroke_colour, &rgb);
		RGBForeColor(&rgb);
#ifdef __MACOS9__
		{
			RgnHandle saved_clip = macos9_push_clip();
			/* fixes960  -  honour dashed/dotted here too, so a
			 * framed rect matches the dashed edges drawn by the
			 * line plotter instead of boxing them in solid. */
			int pen_changed =
				macos9_stroke_pen_set(pstyle->stroke_type);
#endif
		FrameRect(&r);
#ifdef __MACOS9__
			macos9_stroke_pen_reset(pen_changed);
			macos9_pop_clip(saved_clip);
		}
#endif
	}

	return NSERROR_OK;
}

static nserror
macos9_plot_polygon(const struct redraw_context *ctx,
		    const plot_style_t *pstyle,
		    const int *p,
		    unsigned int n)
{
	RGBColor rgb;
	unsigned int i;
#ifdef __MACOS9__
	PolyHandle poly;
#endif
	(void)ctx;
	if (pstyle == NULL || p == NULL || n < 3) return NSERROR_OK;
#ifdef __MACOS9__
	poly = OpenPoly();
	if (poly == NULL) return NSERROR_OK;
	MoveTo((short)p[0], (short)p[1]);
	for (i = 1; i < n; i++) {
		LineTo((short)p[i * 2], (short)p[i * 2 + 1]);
	}
	LineTo((short)p[0], (short)p[1]);
	ClosePoly();
	if (pstyle->fill_type != PLOT_OP_TYPE_NONE) {
		macos9_colour_to_rgb(pstyle->fill_colour, &rgb);
		RGBForeColor(&rgb);
		PaintPoly(poly);
	}
	if (pstyle->stroke_type != PLOT_OP_TYPE_NONE) {
		macos9_colour_to_rgb(pstyle->stroke_colour, &rgb);
		RGBForeColor(&rgb);
		FramePoly(poly);
	}
	KillPoly(poly);
#else
	(void)pstyle; (void)p; (void)n; (void)i; (void)rgb;
#endif
	return NSERROR_OK;
}

static nserror
macos9_plot_path(const struct redraw_context *ctx,
		 const plot_style_t *pstyle,
		 const float *p,
		 unsigned int n,
		 const float transform[6])
{
	/* fixes203  -  path flattening with fill + stroke.
	 *
	 * Pre-fixes203 this function emitted LineTo only, so SVG <path>,
	 * <ellipse>, <circle>, and the new rotated-rect path emitted by
	 * svg__paint_rect rendered as stroke-only outlines and never
	 * filled. Wrap the path traversal in OpenPoly / ClosePoly so the
	 * same sequence is recorded into a PolyHandle. After traversal
	 * issue PaintPoly (if fill_type != NONE) and FramePoly (if
	 * stroke_type != NONE).
	 *
	 * Bezier curves are still approximated by sampling 8 points
	 * per cubic  -  the LineTo calls between sample points get
	 * captured by OpenPoly so the polygon edges follow the curve.
	 *
	 * No transform handling: the caller has already baked any
	 * affine into the supplied (x, y) coordinates. */
	RGBColor rgb;
	unsigned int i;
	float cx, cy;
	int has_started = 0;
#ifdef __MACOS9__
	PolyHandle poly;
#endif
	(void)transform;
	if (pstyle == NULL || p == NULL || n == 0) return NSERROR_OK;
#ifdef __MACOS9__
	poly = OpenPoly();
	if (poly == NULL) return NSERROR_OK;
#endif
	cx = 0.0f; cy = 0.0f;
	i = 0;
	while (i < n) {
		unsigned int op = (unsigned int)p[i++];
		if (op == PLOTTER_PATH_MOVE) {
			if (i + 1 >= n) break;
			cx = p[i]; cy = p[i + 1]; i += 2;
			MoveTo((short)cx, (short)cy);
			has_started = 1;
		} else if (op == PLOTTER_PATH_LINE) {
			if (i + 1 >= n) break;
			cx = p[i]; cy = p[i + 1]; i += 2;
			LineTo((short)cx, (short)cy);
		} else if (op == PLOTTER_PATH_BEZIER) {
			float c1x, c1y, c2x, c2y, ex, ey;
			int step;
			if (i + 5 >= n) break;
			c1x = p[i]; c1y = p[i + 1];
			c2x = p[i + 2]; c2y = p[i + 3];
			ex  = p[i + 4]; ey  = p[i + 5];
			i += 6;
			for (step = 1; step <= 8; step++) {
				float t = (float)step / 8.0f;
				float u = 1.0f - t;
				float bx = u*u*u*cx + 3.0f*u*u*t*c1x + 3.0f*u*t*t*c2x + t*t*t*ex;
				float by = u*u*u*cy + 3.0f*u*u*t*c1y + 3.0f*u*t*t*c2y + t*t*t*ey;
				LineTo((short)bx, (short)by);
			}
			cx = ex; cy = ey;
		} else if (op == PLOTTER_PATH_CLOSE) {
			/* No explicit op needed: ClosePoly below closes the
			 * outline automatically. For multi-subpath paths
			 * we'd need to flush the current poly and start a
			 * new one  -  out of scope for V1. */
		} else {
			break;
		}
	}
#ifdef __MACOS9__
	ClosePoly();
	if (has_started) {
		/* fixes1059 (#258)  -  this block used to PaintPoly/FramePoly with
		 * nothing but a foreground colour, discarding two fields the
		 * caller had already computed correctly:
		 *
		 *  - pstyle->stroke_width was never applied, so QuickDraw's
		 *    default 1x1 pen drew EVERY svg stroke as a hairline no
		 *    matter what stroke-width said. plot_rectangle and the
		 *    border code have honoured PenSize since fixes170; only this
		 *    path missed it, which is why it never showed outside SVG.
		 *  - pstyle->opacity was ignored, so fill-opacity /
		 *    stroke-opacity painted fully solid.
		 *
		 * Both now use the same idioms as macos9_plot_rectangle: PenSize
		 * from the fixed-point width, and the fixes49 stipple buckets for
		 * opacity. Pen state is restored with PenNormal() so a wide pen
		 * cannot leak into the next primitive. */
		plot_style_fixed op = pstyle->opacity;
		bool stipple = false;
		Pattern stipple_pat;

		/* fixes49/fixes223: 0 means "never set" for the many callers
		 * that memset their plot_style, NOT transparent. */
		if (op == 0) op = (plot_style_fixed)PLOT_STYLE_SCALE;
		if (op < (plot_style_fixed)(PLOT_STYLE_SCALE / 20)) {
			/* < 5%  -  paint nothing at all */
			KillPoly(poly);
			return NSERROR_OK;
		}
		if (op < (plot_style_fixed)((PLOT_STYLE_SCALE * 35) / 100)) {
			GetIndPattern(&stipple_pat, sysPatListID, 2);
			stipple = true;
		} else if (op < (plot_style_fixed)((PLOT_STYLE_SCALE * 60) / 100)) {
			GetIndPattern(&stipple_pat, sysPatListID, 3);
			stipple = true;
		} else if (op < (plot_style_fixed)((PLOT_STYLE_SCALE * 85) / 100)) {
			GetIndPattern(&stipple_pat, sysPatListID, 4);
			stipple = true;
		}

		if (pstyle->fill_type != PLOT_OP_TYPE_NONE) {
			macos9_colour_to_rgb(pstyle->fill_colour, &rgb);
			RGBForeColor(&rgb);
			if (stipple) {
				/* fixes220  -  force a white backdrop first: the
				 * pattern paints FG on 1 bits and BG on 0 bits,
				 * and a stale black BackColor turns a 50%
				 * stipple into dark grey instead of a
				 * translucent wash.
				 *
				 * PenPat + PaintPoly rather than FillPoly:
				 * PaintPoly fills with the CURRENT pen pattern,
				 * so this is the same result using calls this
				 * frontend already proves it has. FillPoly
				 * appears nowhere else in the tree and CW8's
				 * Carbon headers are not worth gambling on for
				 * an equivalent operation. */
				RGBColor wht;
				wht.red = 0xFFFF; wht.green = 0xFFFF;
				wht.blue = 0xFFFF;
				RGBBackColor(&wht);
				PenPat(&stipple_pat);
				PaintPoly(poly);
				PenNormal();
			} else {
				PaintPoly(poly);
			}
		}
		if (pstyle->stroke_type != PLOT_OP_TYPE_NONE) {
			short pw = (short)(pstyle->stroke_width >>
					PLOT_STYLE_RADIX);
			if (pw < 1) pw = 1;
			macos9_colour_to_rgb(pstyle->stroke_colour, &rgb);
			RGBForeColor(&rgb);
			PenSize(pw, pw);
			if (stipple) {
				RGBColor wht;
				wht.red = 0xFFFF; wht.green = 0xFFFF;
				wht.blue = 0xFFFF;
				RGBBackColor(&wht);
				PenPat(&stipple_pat);
			}
			FramePoly(poly);
			PenNormal();   /* restore 1x1 pen AND solid pattern */
		}
	}
	KillPoly(poly);
#else
	(void)has_started; (void)rgb;
#endif
	return NSERROR_OK;
}

/* fixes648/650 (Track C1): prepared-GWorld cache on the bitmap
 * (macos9_bitmap.c). Declared HERE, not with the other bitmap externs near the
 * top of the file, because these use GWorldPtr  -  a type that isn't defined
 * until macos9.h pulls in QDOffscreen.h further down. Placing them before that
 * include broke the parse and cascaded through the Carbon headers. */
extern GWorldPtr macos9_bitmap_get_prepared(void *bitmap, int render_w,
		int render_h, bool is_opaque, bool nearest,
		int *out_src_w, int *out_src_h,
		unsigned char **out_mask, int *out_mask_rowbytes);
extern bool macos9_bitmap_set_prepared(void *bitmap, GWorldPtr gw,
		int render_w, int render_h, int src_w, int src_h,
		unsigned char *mask, int mask_rowbytes, bool is_opaque,
		bool nearest, long bytes);

static nserror
macos9_plot_bitmap(const struct redraw_context *ctx,
		   struct bitmap *bitmap,
		   int x, int y,
		   int width, int height,
		   colour bg,
		   bitmap_flags_t flags)
{
#ifdef __MACOS9__
	GWorldPtr gw = NULL;
	Rect src_rect, dst_rect;
	PixMapHandle pm;
	OSErr err;
	unsigned char *buf;
	long rowstride;
	int bw, bh;
	long row;
	unsigned char *src_row;
	unsigned char *dst_row;
	long dst_rowbytes;
	long col;
	/* fixes648 (Track C1): prepared-GWorld cache locals. */
	GWorldPtr cached_gw = NULL;
	int psrc_w = 0, psrc_h = 0;
	unsigned char *pmask = NULL;
	int pmrb = 0;
	int prep_cached = 0;
	int is_opaque_early = 0;
	uint8_t blend_mode = (uint8_t)((flags & BITMAPF_BLEND_MASK) >>
			BITMAPF_BLEND_SHIFT);
	/* fixes829b (#256): nearest-neighbor (image-rendering:pixelated/
	 * crisp-edges) request. Part of the prepared-GWorld cache key so a
	 * smooth and a nearest render of the SAME bitmap at the SAME size
	 * don't collide (they'd otherwise paint-order-alias to one look). */
	int nn = (flags & BITMAPF_NEAREST) != 0;

	MS_ASSERT(bitmap != NULL, "plot_bitmap: bitmap is NULL");
	(void)ctx;
	if (bitmap == NULL) return NSERROR_OK;
	if (blend_mode > CSS_BACKGROUND_BLEND_MODE_NORMAL &&
			blend_bitmap_seen < 8) {
		macsurf_debug_log_writef(
			"LIFE blend bitmap mode=%d bg=%ld size=%d,%d",
			(int)blend_mode, (long)bg, width, height);
		blend_bitmap_seen++;
	}

	buf = macos9_bitmap_get_buffer((void *)bitmap);
	if (buf == NULL) return NSERROR_OK;
	bw = macos9_bitmap_get_width((void *)bitmap);
	bh = macos9_bitmap_get_height((void *)bitmap);
	rowstride = (long)macos9_bitmap_get_rowstride((void *)bitmap);
	if (bw <= 0 || bh <= 0) return NSERROR_OK;

	/* fixes829g (#256 crash): use-after-free guard on the incoming bitmap.
	 *
	 * Hardware probe (fixes829f) caught a FREED bitmap struct reaching
	 * plot_bitmap on the shared-bitmap page during refresh:
	 *   bw=1266812540 (a heap pointer) rs=772282864 buf=00000003
	 * i.e. a navigation/refresh freed the content's bitmap while a pending
	 * knockout redraw still held a pointer to it (the fixes650/#168
	 * content-lifecycle UAF class). The box-filter/fill loops then walked
	 * garbage dimensions off the freed struct -> the reported crash.
	 *
	 * Proper fix is upstream (don't replay a redraw against a freed
	 * bitmap); until then this guard makes the UAF non-fatal: if the
	 * bitmap's own reported dimensions are impossible, the struct is not a
	 * live bitmap -> paint nothing instead of crashing. Real dimensions are
	 * always small on this platform (Mac 16-bit coord space); a >8192 side,
	 * a non-positive render size, or a stride too small for the width all
	 * mean corruption. Logs once per hit (rare) so the UAF stays visible. */
	if (bw > 8192 || bh > 8192 || width <= 0 || height <= 0 ||
	    width > 8192 || height > 8192 || rowstride < (long)bw * 4) {
		macsurf_debug_log_writef(
			"FAIL plot_bitmap freed/corrupt bitmap bw=%ld bh=%ld "
			"w=%d h=%d rs=%ld buf=%p", (long)bw, (long)bh,
			width, height, rowstride, (void *)buf);
		return NSERROR_OK;
	}

	SetRect(&src_rect, 0, 0, (short)bw, (short)bh);
	SetRect(&dst_rect, (short)x, (short)y,
		(short)(x + width), (short)(y + height));

	/* fixes648 (Track C1): if this bitmap already has a prepared GWorld for
	 * this exact render size (+ opaque/mask-presence), reuse it and jump
	 * straight to the blit  -  skipping NewGWorld + the RGBA->XRGB byte-swap +
	 * the box-filter downscale (the measured ~2.5s/paint prepare cost). The
	 * cached pixmap is kept LockPixels'd for the entry's whole life. */
	is_opaque_early = macos9_bitmap_get_opaque((void *)bitmap);
	cached_gw = NULL;
	if (blend_mode <= CSS_BACKGROUND_BLEND_MODE_NORMAL) {
		cached_gw = macos9_bitmap_get_prepared((void *)bitmap, width, height,
				is_opaque_early ? true : false, nn ? true : false,
				&psrc_w, &psrc_h, &pmask, &pmrb);
	}
	if (cached_gw != NULL) {
		PixMapHandle cpm = GetGWorldPixMap(cached_gw);
		if (cpm != NULL) {
			gw = cached_gw;
			pm = cpm;
			SetRect(&src_rect, 0, 0, (short)psrc_w, (short)psrc_h);
			dst_rowbytes = (*pm)->rowBytes & 0x3FFF;
			prep_cached = 1;
			goto do_blit;
		}
	}

	/* fixes829d (#256): NORMAL memory primary, TEMP memory fallback.
	 *
	 * The crash (fixes829c investigation): the box-filter reads THIS
	 * GWorld's pixels while allocating the small intermediate gw_small.
	 * When both were temp memory, gw_small's temp allocation reclaimed
	 * this source's pixels out from under the locked pixmap -> the
	 * box-filter-loop fault (src[col*4], stale base). The real fix is that
	 * gw_small is now NORMAL-only (below), so it can never reclaim a temp
	 * source. That lets us keep a temp FALLBACK here: normal memory is
	 * preferred (stable, cache-safe), but if the app heap is tight (a
	 * RAM-limited G3/iMac) normal can fail - and fixes829c, by dropping
	 * the old temp fallback, made those failures paint NOTHING (the
	 * intermittent blank-image regression). Fall back to temp so the
	 * image still renders; gw_small being normal keeps it crash-free. */
	err = NewGWorld(&gw, 32, &src_rect, NULL, NULL, 0);
	if (err != noErr || gw == NULL) {
		MS_LOG("plot_bitmap: NewGWorld normal FAIL, temp fallback");
		err = NewGWorld(&gw, 32, &src_rect, NULL, NULL, (GWorldFlags)4);
		if (err != noErr || gw == NULL) {
			MS_LOG("plot_bitmap: NewGWorld FAIL");
			return NSERROR_OK;
		}
	}

	pm = GetGWorldPixMap(gw);
	if (pm == NULL || !LockPixels(pm)) {
		DisposeGWorld(gw);
		return NSERROR_OK;
	}

	/* Copy bitmap buffer (RGBA) to GWorld (ARGB on PPC).
	 *
	 * fixes189: force the ARGB high byte to 0xFF (XRGB convention).
	 * Classic QuickDraw 32-bit pixmaps are spec'd XRGB, but some
	 * code paths (CopyMask, CopyBits depth-conversion) interpret
	 * the high byte as a per-pixel alpha factor on color systems.
	 * fixes188 propagated the actual alpha byte here, which made
	 * partial-alpha pixels render at reduced opacity → image read
	 * as "transparency lost" because the whole image looked
	 * washed-out. The 1-bit mask plane handles the
	 * transparent-vs-opaque distinction; the new composite path
	 * reads alpha from `buf` directly. Keep pm strictly XRGB. */
	dst_rowbytes = (*pm)->rowBytes & 0x3FFF;
	for (row = 0; row < bh; row++) {
		src_row = buf + row * rowstride;
		dst_row = (unsigned char *)GetPixBaseAddr(pm) + row * dst_rowbytes;
		for (col = 0; col < bw; col++) {
			unsigned char r = src_row[col * 4 + 0];
			unsigned char g = src_row[col * 4 + 1];
			unsigned char b = src_row[col * 4 + 2];
			if (blend_mode > CSS_BACKGROUND_BLEND_MODE_NORMAL) {
				unsigned char a = src_row[col * 4 + 3];
				colour src_colour = (colour)((unsigned long)r |
						((unsigned long)g << 8) |
						((unsigned long)b << 16) |
						((unsigned long)(255u - a) << 24));
				colour blended = macos9_background_blend_colour(
						src_colour, bg, blend_mode);
				r = (unsigned char)(blended & 0xff);
				g = (unsigned char)((blended >> 8) & 0xff);
				b = (unsigned char)((blended >> 16) & 0xff);
			}
			dst_row[col * 4 + 0] = 0xFF;
			dst_row[col * 4 + 1] = r;
			dst_row[col * 4 + 2] = g;
			dst_row[col * 4 + 3] = b;
		}
	}

	/* fixes221  -  kill switch for fixes203's box-filter pre-downscale.
	 * fixes257  -  flipped ON. The dark-grey-wash investigation that
	 * caused fixes221 to disable this turned out to be a separate bug
	 * (fixes225, inset box-shadow). Box-filter is the right call for
	 * downscale: CopyMask's nearest-neighbor 1-bit mask scaling drops
	 * pixels on non-integer ratios, producing the "faded" look that
	 * has plagued every PNG with transparency since fixes128. With
	 * box-filter pre-downscaling, the destination receives a properly
	 * averaged mask that doesn't lose pixels  -  image is sharper AND
	 * not faded.
	 * Set MACSURF_BOX_FILTER_DOWNSCALE = 0 to revert to pure QuickDraw
	 * nearest-neighbor scaling (the pre-fixes203
	 * the-GWorld path is darkening unrelated pixels through some
	 * QuickDraw state leak. fixes221 ships with this DISABLED so the
	 * user can confirm whether box-filter is the dark-grey culprit.
	 * If still dark with this off, box-filter is innocent. If light,
	 * we need a narrower fix that keeps the rainbow-streak repair
	 * without the side effect. Flip to 1 to restore. */
#ifndef MACSURF_BOX_FILTER_DOWNSCALE
#define MACSURF_BOX_FILTER_DOWNSCALE 1
#endif

	/* fixes203  -  box-filter pre-downscale for high-quality image
	 * rendering. QuickDraw's CopyBits / CopyMask scale via nearest-
	 * neighbor, which on large downscale ratios (3x+) produces severe
	 * aliasing (the "rainbow streak" artefact visible on OP1 / OP2 in
	 * fixes202 hardware probes: the mactrove logo at 1058×245 reduced
	 * to ~160×37 by object-fit:contain). Average each src block of
	 * (sx × sy) pixels into one dst pixel of a target-sized
	 * intermediate GWorld, then have the existing blit code copy the
	 * intermediate at 1:1  -  no QuickDraw scaling involved.
	 *
	 * Mask handling: the 1-bit mask is also box-filtered. Each dst
	 * mask bit is set when more than half of the source bits in the
	 * corresponding block are set, preserving the alpha>=128 threshold
	 * the decoder applied.
	 *
	 * Gated by the 3× threshold so modest 1.0–2.5× scaling stays on
	 * the fast nearest-neighbor path. The original gw / pm are
	 * replaced in place by the smaller intermediate when this fires;
	 * the small-mask buffer (when present) is freed at function exit
	 * via the bf_small_mask local. */
	{
		long sx_ratio_q8 = (long)width <= 0 ? 0 :
				(((long)bw << 8) / (long)width);
		long sy_ratio_q8 = (long)height <= 0 ? 0 :
				(((long)bh << 8) / (long)height);
		/* fixes257  -  threshold lowered from 3× to 1.5× (q8 = 384).
		 * Below 1.5× the box-filter cost outweighs the visual win
		 * (and the CopyMask fade is mild). Above 1.5× the fade is
		 * visible and box-filter is a clear improvement. mactrove's
		 * 1058x245 logo at 400x92 is 2.6×  -  under the old 3× gate
		 * it stayed faded; under 1.5× it pre-downscales sharply. */
		/* fixes829b (#256): image-rendering:pixelated/crisp-edges samples
		 * nearest instead of averaging. We deliberately keep going THROUGH
		 * the box-filter block (not skipping it) so the prepared-GWorld
		 * cache stores a render-sized GWorld exactly as the smooth path
		 * does - skipping it cached the full-size temp-memory source under
		 * the render-size key, and the size/handle mismatch crashed
		 * DisposeGWorld in the cache (fixes829 regression). The nn flag
		 * (computed at function scope) collapses each source block to its
		 * top-left pixel. */
		if (MACSURF_BOX_FILTER_DOWNSCALE &&
				(sx_ratio_q8 >= (3L * 128L) || sy_ratio_q8 >= (3L * 128L)) &&
				width >= 4 && height >= 4) {
			GWorldPtr gw_small = NULL;
			Rect small_rect;
			PixMapHandle pm_small;
			OSErr small_err;
			SetRect(&small_rect, 0, 0, (short)width, (short)height);
			/* fixes829c: normal memory, not temp (see the main
			 * NewGWorld above). This box-filtered intermediate is
			 * what the prepared-GWorld cache stores for downscaled
			 * images, so temp-mem reclamation of its pixels was the
			 * box-filter-read crash on cache-churning pages. */
			small_err = NewGWorld(&gw_small, 32, &small_rect, NULL,
					NULL, 0);
			if (small_err == noErr && gw_small != NULL) {
				pm_small = GetGWorldPixMap(gw_small);
				if (pm_small != NULL && LockPixels(pm_small)) {
					long src_rb = dst_rowbytes;
					long dst_rb_small;
					unsigned char *src_base =
						(unsigned char *)
						GetPixBaseAddr(pm);
					unsigned char *dst_base_small =
						(unsigned char *)
						GetPixBaseAddr(pm_small);
					long dy;
					long dxp;
					dst_rb_small = (*pm_small)->rowBytes
						& 0x3FFF;
					/* fixes829e (#256): belt-and-braces guard.
					 * The box-filter walks src_base (bw*bh) and
					 * dst_base_small (width*height). If EITHER
					 * base is NULL or a rowbytes is nonsensically
					 * small for its width, do NOT run the loops --
					 * a bad base is exactly what crashed as
					 * src[col*4] on the temp-mem-reclamation bug.
					 * Skip the box-filter and let the full-size
					 * source blit via QuickDraw instead (image
					 * still shows, just un-downscaled), rather than
					 * fault. */
					if (src_base == NULL ||
					    dst_base_small == NULL ||
					    src_rb < (long)bw * 4 ||
					    dst_rb_small < (long)width * 4) {
						UnlockPixels(pm_small);
						DisposeGWorld(gw_small);
						gw_small = NULL;
						MS_LOG("plot_bitmap: box-filter "
							"bad base/stride, skip");
						goto do_blit;
					}
					for (dy = 0; dy < (long)height; dy++) {
						long sy0 = (dy * (long)bh) /
							(long)height;
						long sy1 = ((dy + 1) *
							(long)bh) /
							(long)height;
						unsigned char *drow;
						if (sy1 <= sy0) sy1 = sy0 + 1;
						drow = dst_base_small +
							dy * dst_rb_small;
						for (dxp = 0; dxp <
								(long)width;
								dxp++) {
							long sx0 = (dxp *
								(long)bw) /
								(long)width;
							long sx1 = ((dxp + 1)
								* (long)bw) /
								(long)width;
							unsigned long sum_r;
							unsigned long sum_g;
							unsigned long sum_b;
							unsigned long count;
							long syk;
							if (sx1 <= sx0)
								sx1 = sx0 + 1;
							/* fixes829b: nearest -> one
							 * top-left sample, no average.
							 * (sy1 is recomputed every dy
							 * row, so clamping it here is
							 * safe.) */
							if (nn) {
								sx1 = sx0 + 1;
								sy1 = sy0 + 1;
							}
							sum_r = 0; sum_g = 0;
							sum_b = 0; count = 0;
							for (syk = sy0; syk <
								sy1; syk++) {
								unsigned char *
								srow = src_base
								+ syk *
								src_rb;
								long sxk;
								for (sxk = sx0;
								sxk < sx1;
								sxk++) {
									sum_r += srow[sxk * 4 + 1];
									sum_g += srow[sxk * 4 + 2];
									sum_b += srow[sxk * 4 + 3];
									count++;
								}
							}
							if (count == 0)
								count = 1;
							drow[dxp * 4 + 0]
								= 0xFF;
							drow[dxp * 4 + 1] =
								(unsigned char)
								(sum_r / count);
							drow[dxp * 4 + 2] =
								(unsigned char)
								(sum_g / count);
							drow[dxp * 4 + 3] =
								(unsigned char)
								(sum_b / count);
						}
					}
					/* Swap in the small GWorld and update
					 * src_rect so the existing blit code
					 * copies 1:1. */
					UnlockPixels(pm);
					DisposeGWorld(gw);
					gw = gw_small;
					pm = pm_small;
					SetRect(&src_rect, 0, 0,
						(short)width,
						(short)height);
					dst_rowbytes = dst_rb_small;
					/* The new gw_small replaces gw; do NOT
					 * dispose gw_small here. */
				} else {
					if (pm_small != NULL)
						UnlockPixels(pm_small);
					DisposeGWorld(gw_small);
				}
			}
		}
	}

do_blit:
	{
		GrafPtr save_port;
		RgnHandle saved_clip;
		bool is_opaque;
		unsigned char *mask_data;
		int mask_rowbytes;
		unsigned char *bf_small_mask = NULL;
		int bf_small_mask_rowbytes = 0;
		bool repeat_x;
		bool repeat_y;
		int start_x, start_y, end_x, end_y;
		int tile_x, tile_y;
		Rect clip_bounds;
		Rect tile_dst;
		BitMap mask_bm;
		long tile_count;
		long tile_cap;
		RGBColor blit_fg;
		RGBColor blit_bg;

		GetPort(&save_port);
		saved_clip = macos9_push_clip();

		/* fixes301j  -  reset foreground to black and background to white
		 * before CopyBits / CopyMask. Classic QuickDraw colorizes the
		 * transfer with the port's current fg/bg colours; the page draws
		 * blue link text (RGBForeColor blue) and leaves the port fg blue,
		 * so without this reset every image gets tinted toward the
		 * leftover fg colour (the "blue tint" / "faded" symptom, and why
		 * it appeared only after coloured text had drawn). Confirmed via
		 * DESTRB probe: a black source pixel [255,0,0,0] landed in the
		 * dest as [0,0,95,169] (blue). With fg=black / bg=white the
		 * colorize is the identity transform. */
		blit_fg.red = 0; blit_fg.green = 0; blit_fg.blue = 0;
		blit_bg.red = 0xFFFF; blit_bg.green = 0xFFFF; blit_bg.blue = 0xFFFF;
		RGBForeColor(&blit_fg);
		RGBBackColor(&blit_bg);
		is_opaque = macos9_bitmap_get_opaque((void *)bitmap);
		mask_data = is_opaque ? NULL :
				macos9_bitmap_get_mask((void *)bitmap);
		mask_rowbytes = is_opaque ? 0 :
				macos9_bitmap_get_mask_rowbytes((void *)bitmap);
		/* fixes648: on a cache hit that was box-filter-downscaled, use the
		 * cached scaled mask; pmask==NULL means the non-downscaled case, so
		 * keep the live mask fetched just above. */
		if (prep_cached && pmask != NULL) {
			mask_data = pmask;
			mask_rowbytes = pmrb;
		}

		/* fixes203  -  if the box-filter pre-downscale above swapped
		 * src_rect from bw×bh to width×height, the original mask
		 * (sized to bw×bh) no longer matches the small pixmap.
		 * Box-filter the mask to a fresh dest-sized buffer
		 * (rowbytes rounded up to whole words for QuickDraw),
		 * UNION over the source block. Owned locally; freed below. */
		if (!prep_cached && mask_data != NULL &&
				(src_rect.right - src_rect.left) == (int)width &&
				(src_rect.bottom - src_rect.top) == (int)height &&
				(bw != (int)width || bh != (int)height)) {
			int dest_w_bytes = ((int)width + 7) / 8;
			int dest_w_words = (dest_w_bytes + 1) / 2;
			int dst_rb = dest_w_words * 2;
			long buf_bytes = (long)dst_rb * (long)height;
			unsigned char *new_mask;
			if (buf_bytes < 0) buf_bytes = 0;
			new_mask = buf_bytes > 0 ?
				(unsigned char *)calloc(1, (size_t)buf_bytes) :
				NULL;
			if (new_mask != NULL) {
				long dy;
				long dxp;
				for (dy = 0; dy < (long)height; dy++) {
					long sy0 = (dy * (long)bh) /
						(long)height;
					long sy1 = ((dy + 1) *
						(long)bh) / (long)height;
					unsigned char *drow;
					if (sy1 <= sy0) sy1 = sy0 + 1;
					drow = new_mask + dy * dst_rb;
					for (dxp = 0; dxp < (long)width;
							dxp++) {
						long sx0 = (dxp * (long)bw) /
							(long)width;
						long sx1 = ((dxp + 1) *
							(long)bw) /
							(long)width;
						unsigned long on = 0;
						unsigned long total = 0;
						long syk;
						if (sx1 <= sx0) sx1 = sx0 + 1;
						for (syk = sy0; syk < sy1;
								syk++) {
							long sxk;
							unsigned char *srow =
								mask_data +
								syk *
								mask_rowbytes;
							for (sxk = sx0; sxk <
								sx1; sxk++) {
								unsigned char
								bit = srow[sxk
								>> 3] >>
								(7 - (sxk &
								7));
								on += (bit
								& 1);
								total++;
							}
						}
						if (total > 0 &&
								on * 2 >=
								total) {
							drow[dxp >> 3] |=
								(unsigned char)
								(0x80 >> (dxp
								& 7));
						}
					}
				}
				bf_small_mask = new_mask;
				bf_small_mask_rowbytes = dst_rb;
				mask_data = bf_small_mask;
				mask_rowbytes = bf_small_mask_rowbytes;
			}
		}

		/* fixes138: honour BITMAPF_REPEAT_X / BITMAPF_REPEAT_Y.
		 * NetSurf's image content handler passes the tile size in
		 * (width, height) and the anchor in (x, y); the plotter is
		 * expected to tile across the active clip rect. Pre-138
		 * MacSurf ignored the flags and painted one tile, which
		 * looked correct for no-repeat but broke every
		 * `background-repeat: repeat[-x|-y]` page including the
		 * fixes137 background-attachment: fixed parallax demo. */
		repeat_x = (flags & BITMAPF_REPEAT_X) != 0;
		repeat_y = (flags & BITMAPF_REPEAT_Y) != 0;

		if (repeat_x || repeat_y) {
			RgnHandle cur_clip = NewRgn();
			if (cur_clip != NULL) {
				GetClip(cur_clip);
				GetRegionBounds(cur_clip, &clip_bounds);
				DisposeRgn(cur_clip);
			} else {
				SetRect(&clip_bounds, 0, 0, 0, 0);
			}
		} else {
			SetRect(&clip_bounds, 0, 0, 0, 0);
		}

		if (repeat_x) {
			/* Step back from x by `width` until the next step
			 * would precede the clip's left edge, then fill
			 * forward to the right edge. The anchor (x) is
			 * guaranteed to land on a tile boundary, which is
			 * what fixes137's viewport-anchored origin needs. */
			start_x = x;
			while (start_x - width >= (int)clip_bounds.left)
				start_x -= width;
			end_x = (int)clip_bounds.right;
			if (end_x < x + width) end_x = x + width;
		} else {
			start_x = x;
			end_x = x + width;
		}
		if (repeat_y) {
			start_y = y;
			while (start_y - height >= (int)clip_bounds.top)
				start_y -= height;
			end_y = (int)clip_bounds.bottom;
			if (end_y < y + height) end_y = y + height;
		} else {
			start_y = y;
			end_y = y + height;
		}

		/* Hard ceiling: 4096 tiles in a single blit. Anything beyond
		 * is a layout bug, not a real page. QD itself will refuse
		 * pathological coordinates but the cap keeps the redraw
		 * predictable on degenerate input. */
		tile_count = 0;
		tile_cap = 4096;

		/* fixes190  -  Revert fixes188 composite branch. The
		 * destination-readback CopyBits did not behave as
		 * expected on hardware and consumed PNG transparency.
		 * Back to the single CopyMask path for all alpha
		 * bitmaps; scaled icons regain the fixes187 "sharper
		 * but still faded" baseline. The macos9_image.c
		 * non-premultiplied / threshold-8 mask state from
		 * fixes188 stays in place; the buf->pm XRGB
		 * enforcement from fixes189 also stays. A future
		 * round can take a different shape (CopyDeepMask, or
		 * pre-scale-and-bg-blend with no readback) once we
		 * have a working hardware experiment. */
		if (mask_data != NULL && mask_rowbytes > 0) {
			mask_bm.baseAddr = (Ptr)mask_data;
			mask_bm.rowBytes = (short)mask_rowbytes;
			mask_bm.bounds = src_rect;
			MS_LOG("plot_bitmap: alpha CopyMask");
#if 1
			/* fixes301b blit probe: dump the source GWorld centre
			 * pixel (post-fill / post-box-filter) and the blit
			 * geometry so we can see what CopyMask actually scales.
			 * Capped per session. */
			{
				static long macos9_blit_probe = 0;
				int sw = src_rect.right - src_rect.left;
				int sh = src_rect.bottom - src_rect.top;
				if (macos9_blit_probe < 10 && sw > 1 && sh > 1) {
					unsigned char *gp =
						(unsigned char *)GetPixBaseAddr(pm) +
						(long)(sh / 2) * dst_rowbytes +
						(long)(sw / 2) * 4;
					macos9_blit_probe++;
					macsurf_debug_log_writef(
						"BLIT src=%dx%d dst=%dx%d mrb=%d "
						"gw=[%d,%d,%d,%d] %s",
						sw, sh, width, height,
						(int)mask_rowbytes,
						(int)gp[0], (int)gp[1], (int)gp[2],
						(int)gp[3],
						(sw == width && sh == height) ?
							"1:1" : "SCALED");
				}
			}
#endif
			for (tile_y = start_y; tile_y < end_y; tile_y += height) {
				for (tile_x = start_x; tile_x < end_x;
						tile_x += width) {
					SetRect(&tile_dst,
						(short)tile_x,
						(short)tile_y,
						(short)(tile_x + width),
						(short)(tile_y + height));
					CopyMask((BitMap *)*pm,
						&mask_bm,
						GetPortBitMapForCopyBits((CGrafPtr)save_port),
						&src_rect, &src_rect,
						&tile_dst);
					if (++tile_count >= tile_cap)
						goto blit_done;
				}
			}
		} else {
			MS_LOG(is_opaque ? "plot_bitmap: opaque srcCopy" :
					"plot_bitmap: nonopaque no-mask srcCopy");
			for (tile_y = start_y; tile_y < end_y; tile_y += height) {
				for (tile_x = start_x; tile_x < end_x;
						tile_x += width) {
					SetRect(&tile_dst,
						(short)tile_x,
						(short)tile_y,
						(short)(tile_x + width),
						(short)(tile_y + height));
					CopyBits((BitMap *)*pm,
						GetPortBitMapForCopyBits((CGrafPtr)save_port),
						&src_rect, &tile_dst,
						srcCopy, NULL);
					if (++tile_count >= tile_cap)
						goto blit_done;
				}
			}
		}
blit_done:
		macos9_pop_clip(saved_clip);
		/* fixes648 (Track C1): cache the freshly-prepared GWorld (+ any
		 * box-filtered mask) so future repaints at this size skip the whole
		 * prepare. On success the bitmap OWNS gw + mask (kept LockPixels'd)
		 * and we must not free/dispose them here; on decline (oversize /
		 * budget) we fall through to the transient free + dispose below,
		 * byte-for-byte identical to the pre-cache behaviour. */
		if (!prep_cached && blend_mode <= CSS_BACKGROUND_BLEND_MODE_NORMAL) {
			int cache_sw = src_rect.right - src_rect.left;
			int cache_sh = src_rect.bottom - src_rect.top;
			/* fixes650b (review): if this was a DOWNSCALE of a MASKED
			 * bitmap but the box-filtered small-mask couldn't be built
			 * (calloc failed under heap pressure), this frame already
			 * blitted a mismatched full-res mask against the W×H source.
			 * Do NOT cache that broken state  -  a cache hit would repeat
			 * the wrong-transparency blit forever, whereas the transient
			 * path self-heals on the next repaint once memory frees. */
			int downscaled = (cache_sw != bw) || (cache_sh != bh);
			int masked = (!is_opaque_early) &&
				(macos9_bitmap_get_mask((void *)bitmap) != NULL);
			if (!(downscaled && masked && bf_small_mask == NULL)) {
				long cache_bytes = (long)dst_rowbytes *
						(long)cache_sh;
				if (bf_small_mask != NULL)
					cache_bytes += (long)bf_small_mask_rowbytes *
							(long)height;
				if (macos9_bitmap_set_prepared((void *)bitmap, gw,
						width, height, cache_sw, cache_sh,
						bf_small_mask,
						bf_small_mask_rowbytes,
						is_opaque_early ? true : false,
						nn ? true : false,
						cache_bytes)) {
					prep_cached = 1;
					bf_small_mask = NULL; /* owned by bitmap */
				}
			}
		}
		/* fixes203  -  release the box-filtered mask buffer (unless the cache
		 * took ownership of it just above). */
		if (bf_small_mask != NULL) {
			free(bf_small_mask);
		}
	}

	/* fixes648: on a cache hit OR a successful cache store, the GWorld is now
	 * owned by the bitmap (kept locked)  -  do NOT unlock/dispose it here. Only
	 * the transient / declined path tears down. */
	if (!prep_cached) {
		UnlockPixels(pm);
		DisposeGWorld(gw);
	}
#else
	(void)ctx; (void)bitmap;
	(void)x; (void)y; (void)width; (void)height; (void)bg; (void)flags;
#endif
	return NSERROR_OK;
}

#ifdef __MACOS9__
/* First UTF-8 codepoint of a run (0 on empty/invalid); PUA test. Shared by the
 * webfont icon-glyph diversion in macos9_plot_text (fixes630). */
static unsigned long macos9_first_cp(const char *s, size_t len)
{
	unsigned char c;
	if (s == NULL || len == 0)
		return 0;
	c = (unsigned char) s[0];
	if (c < 0x80)
		return c;
	if ((c & 0xE0) == 0xC0 && len >= 2)
		return ((unsigned long) (c & 0x1F) << 6) |
		       (unsigned long) (s[1] & 0x3F);
	if ((c & 0xF0) == 0xE0 && len >= 3)
		return ((unsigned long) (c & 0x0F) << 12) |
		       ((unsigned long) (s[1] & 0x3F) << 6) |
		       (unsigned long) (s[2] & 0x3F);
	if ((c & 0xF8) == 0xF0 && len >= 4)
		return ((unsigned long) (c & 0x07) << 18) |
		       ((unsigned long) (s[1] & 0x3F) << 12) |
		       ((unsigned long) (s[2] & 0x3F) << 6) |
		       (unsigned long) (s[3] & 0x3F);
	return 0;
}
#define MACOS9_CP_IS_PUA(cp) (((cp) >= 0xE000UL && (cp) <= 0xF8FFUL) || \
			      ((cp) >= 0xF0000UL && (cp) <= 0x10FFFDUL))

static void macos9_text_draw_run(const char *text, size_t length,
		int x, int y, int letter_spacing, int word_spacing)
{
	if ((letter_spacing == 0 && word_spacing == 0) || length <= 1) {
		MoveTo((short)x, (short)y);
		DrawText(text, 0, (short)length);
	} else {
		size_t i;
		short pen_x = (short)x;
		short char_width;
		int gap;

		for (i = 0; i < length; i++) {
			MoveTo(pen_x, (short)y);
			DrawText(text, (short)i, 1);
			char_width = (short)CharWidth(text[i]);
			gap = letter_spacing;
			if (text[i] == ' ') gap += word_spacing;
			pen_x = (short)(pen_x + char_width + gap);
		}
	}
}

static int macos9_text_run_width(const char *text, size_t length,
		int letter_spacing, int word_spacing)
{
	size_t i;
	int width = 0;
	int gap;

	for (i = 0; i < length; i++) {
		width += (int)CharWidth(text[i]);
		if (i + 1 < length) {
			gap = letter_spacing;
			if (text[i] == ' ') gap += word_spacing;
			width += gap;
		}
	}

	return width;
}

/* OpenRgn records vector primitives but does not reliably retain system-font
 * glyphs on the target. Rasterise the already-selected QuickDraw text instead,
 * then turn its black pixels into the text clipping region. */
static bool macos9_make_text_clip(RgnHandle glyph_clip,
		const char *text, size_t length, int x, int y,
		int letter_spacing, int word_spacing, short font_id,
		short face, short size)
{
	GWorldPtr mask_gw = NULL;
	CGrafPtr saved_port;
	GDHandle saved_device;
	const BitMap *mask_bits;
	Rect mask_rect;
	Rect glyph_bounds;
	RGBColor black;
	RGBColor white;
	OSErr err;
	int run_width;
	int pad;
	int mask_width;
	int mask_height;
	bool made = false;

	if (glyph_clip == NULL || text == NULL || length == 0)
		return false;

	run_width = macos9_text_run_width(text, length,
			letter_spacing, word_spacing);
	if (run_width < 1) return false;

	pad = (int)size * 2 + 8;
	if (pad < 16) pad = 16;
	mask_width = run_width + 4;
	mask_height = pad * 2;
	if (mask_width > 8192 || mask_height > 8192)
		return false;

	SetRect(&mask_rect, 0, 0, (short)mask_width, (short)mask_height);
	GetGWorld(&saved_port, &saved_device);
	err = NewGWorld(&mask_gw, 1, &mask_rect, NULL, NULL, 0);
	if (err != noErr || mask_gw == NULL)
		return false;

	SetGWorld(mask_gw, NULL);
	black.red = 0;
	black.green = 0;
	black.blue = 0;
	white.red = 0xffff;
	white.green = 0xffff;
	white.blue = 0xffff;
	RGBForeColor(&black);
	RGBBackColor(&white);
	EraseRect(&mask_rect);
	TextFont(font_id);
	TextSize(size);
	TextFace(face);
	macos9_text_draw_run(text, length, 2, pad,
			letter_spacing, word_spacing);
	mask_bits = GetPortBitMapForCopyBits((CGrafPtr)mask_gw);
	if (mask_bits != NULL) {
		BitMapToRegion(glyph_clip, (BitMap *)mask_bits);
		OffsetRgn(glyph_clip, (short)(x - 2), (short)(y - pad));
		GetRegionBounds(glyph_clip, &glyph_bounds);
		made = glyph_bounds.left < glyph_bounds.right &&
				glyph_bounds.top < glyph_bounds.bottom;
		if (clip_text_mask_seen < 12) {
			macsurf_debug_log_writef(
				"LIFE clip-text mask=%d,%d glyph=%d,%d,%d,%d ok=%d",
				mask_width, mask_height, glyph_bounds.left,
				glyph_bounds.top, glyph_bounds.right,
				glyph_bounds.bottom, made ? 1 : 0);
			clip_text_mask_seen++;
		}
	}

	SetGWorld(saved_port, saved_device);
	DisposeGWorld(mask_gw);
	return made;
}

static void macos9_paint_background_clip_text(
		const struct redraw_context *ctx, const char *text, size_t length,
		int x, int y, int letter_spacing, int word_spacing,
		short font_id, short face, short size)
{
	RgnHandle base_clip;
	RgnHandle glyph_clip;
	struct macos9_background_clip_text_state *state =
		&macos9_background_clip_text;

	if (!state->active) return;

	base_clip = NewRgn();
	glyph_clip = NewRgn();
	if (base_clip == NULL || glyph_clip == NULL) {
		if (base_clip != NULL) DisposeRgn(base_clip);
		if (glyph_clip != NULL) DisposeRgn(glyph_clip);
		return;
	}

	GetClip(base_clip);
	if (!macos9_make_text_clip(glyph_clip, text, length, x, y,
			letter_spacing, word_spacing, font_id, face, size)) {
		DisposeRgn(glyph_clip);
		DisposeRgn(base_clip);
		return;
	}
	SectRgn(base_clip, glyph_clip, glyph_clip);
	SetClip(glyph_clip);

	if (state->fill.fill_type != PLOT_OP_TYPE_NONE) {
		if (state->gradient_stops != NULL) {
			macos9_set_gradient_stops(state->gradient_stops);
			macos9_set_gradient_angle(state->gradient_angle);
			macos9_set_gradient_blend(state->blend_mode,
					state->blend_backdrop);
		}
		(void)macos9_plot_rectangle(ctx, &state->fill,
				&state->fill_rect);
	}
	if (state->bitmap != NULL && state->bitmap_width > 0 &&
			state->bitmap_height > 0) {
		(void)macos9_plot_bitmap(ctx, state->bitmap,
				state->bitmap_x, state->bitmap_y,
				state->bitmap_width, state->bitmap_height,
				state->bitmap_background, state->bitmap_flags);
	}

	SetClip(base_clip);
	DisposeRgn(glyph_clip);
	DisposeRgn(base_clip);
}
#endif

static nserror
macos9_plot_text(const struct redraw_context *ctx,
		 const plot_font_style_t *fstyle,
		 int x, int y,
		 const char *text,
		 size_t length)
{
	RGBColor rgb;
	short font_id;
	short face;
	short size;
	(void)ctx;

	macos9_plot_text_count++;

	if (fstyle == NULL || text == NULL || length == 0)
		return NSERROR_OK;

	/* fixes630 (webfonts): the fetch is now kicked from the MEASURE path
	 * (macos9_font.c) so the icon box gets sized; the actual glyph render
	 * for a PUA-in-webfont run happens below, after the text fg is set. */

	font_id = macos9_font_id_from_style(fstyle);
	face    = macos9_face_from_style(fstyle);
	/* plot_style_fixed is a 22.10 fixed-point pt size; shift down. */
	size = (short)(fstyle->size >> PLOT_STYLE_RADIX);
	if (size <= 0) size = 12;

	TextFont(font_id);
	TextSize(size);
	TextFace(face);

	macos9_colour_to_rgb(fstyle->foreground, &rgb);
	RGBForeColor(&rgb);

#ifdef __MACOS9__
	/* fixes630: render a downloadable-webfont icon glyph (a PUA codepoint
	 * in an @font-face family) as a filled QuickDraw region in the text fg
	 * just set. Falls through to the normal path (which blanks PUA) if the
	 * font isn't loaded/parsed yet or has no glyph for this codepoint. y is
	 * the text baseline. */
	{
		unsigned long cp = macos9_first_cp(text, length);
		if (MACOS9_CP_IS_PUA(cp) && macos9_paint_gw != NULL &&
				fstyle->families != NULL) {
			struct browser_window *bw = macos9_gw_bw(macos9_paint_gw);
			struct hlcache_handle *h = (bw != NULL) ?
					browser_window_get_content(bw) : NULL;
			struct content *c = (h != NULL) ?
					hlcache_handle_get_content(h) : NULL;
			if (c != NULL) {
				lwc_string * const *ff;
				for (ff = fstyle->families; *ff != NULL; ff++) {
					if (macos9_webfont_render(c, *ff, cp,
							(int) x, (int) y,
							(int) size) >= 0)
						return NSERROR_OK;
				}
			}
		}
	}
#endif

	/* Diagnostic: dump foreground colour + first 16 chars of the
	 * string for the first ~12 text plots each redraw, so we can
	 * see whether libcss applied <style> rules (e.g. h1 colour
	 * navy, body text colour, etc.) or whether everything stayed
	 * UA default black. */
	if (macos9_plot_text_count <= 12) {
		unsigned int fr = (unsigned int)((fstyle->foreground >>  0) & 0xff);
		unsigned int fg = (unsigned int)((fstyle->foreground >>  8) & 0xff);
		unsigned int fb = (unsigned int)((fstyle->foreground >> 16) & 0xff);
		char snippet[20];
		size_t copy = length < 16 ? length : 16;
		size_t i;
		for (i = 0; i < copy; i++) {
			char ch = text[i];
			snippet[i] = (ch >= 32 && ch < 127) ? ch : '.';
		}
		snippet[copy] = '\0';
		macsurf_debug_log_writef(
			"plot_text[%d] fg=%d/%d/%d sz=%d face=%d at (%d,%d) \"%s\"",
			(int)macos9_plot_text_count,
			(int)fr, (int)fg, (int)fb,
			(int)size, (int)face, (int)x, (int)y,
			snippet);
	}

	/* Convert UTF-8 to MacRoman so bullets, em-dashes, smart quotes,
	 * etc. render as the right glyph instead of as `?` or garbage.
	 * Without this, list-style-type:disc bullets (U+2022) appear
	 * as `;` and most modern punctuation breaks. */
#ifdef __MACOS9__
	{
		char mac_buf[1024];
		size_t mac_len;
		RgnHandle saved_clip;
		int ls;
		int ws;
		int sx;
		int sy;

		mac_len = macos9_utf8_to_macroman(text, length, mac_buf,
				sizeof(mac_buf));

#if MACSURF_FONT_ALIAS_DIAG
		/* fixes157: log paint-side dispatch for the comparison against
		 * macos9_font_measure's matching line. SMART filter (in macos9.h)
		 * skips PLOT_FONT_FAMILY_SANS_SERIF so the Helvetica-path
		 * firehose stays out of the log; non-default families (SERIF /
		 * MONOSPACE / CURSIVE / FANTASY) and NULL fstyle still log. */
		{
			int log_this = 1;
#if MACSURF_FONT_ALIAS_DIAG_SMART
			if (fstyle != NULL &&
			    fstyle->family == PLOT_FONT_FAMILY_SANS_SERIF) {
				log_this = 0;
			}
#endif
			if (log_this) {
				char dpv[24];
				size_t pn = (mac_len < 16) ? mac_len : 16;
				size_t pk;
				for (pk = 0; pk < pn; pk++) {
					char c = mac_buf[pk];
					dpv[pk] = (c >= 0x20 && c < 0x7f) ? c : '.';
				}
				dpv[pn] = '\0';
				macsurf_debug_log_writef(
				    "[FONTDIAG] op=paint   fam=%d id=%d sz=%d face=%d "
				    "ls=%d ws=%d mac=%d xy=(%d,%d) str=\"%s\"",
				    (int)(fstyle ? fstyle->family : -1),
				    (int)font_id, (int)size, (int)face,
				    (int)(fstyle ? fstyle->letter_spacing : 0),
				    (int)(fstyle ? fstyle->word_spacing : 0),
				    (int)mac_len, (int)x, (int)y, dpv);
			}
		}
#endif

		saved_clip = macos9_push_clip();
		/* fixes609: derive effective letter/word spacing from the SAME
		 * shared helper the measure path uses (macos9_run_spacing), so
		 * the bulk-vs-per-char branch and the advance width can never
		 * drift between measure and paint. It folds in the bold-smear
		 * and sub-12 bitmap-gap bumps that used to live inline below
		 * (fixes70 + fixes144b). */
		macos9_run_spacing(fstyle, font_id, face, size, mac_len,
				   &ls, &ws);
		sx = (fstyle != NULL) ? fstyle->shadow_x : 0;
		sy = (fstyle != NULL) ? fstyle->shadow_y : 0;

		/* fixes71/72 - text-side transform.
		 *
		 * V2 (fixes72): the translate component still moves the
		 * text origin, but the glyphs themselves continue to
		 * render upright regardless of rotation. True glyph
		 * rotation (letters tilted, not just origin shifted)
		 * needs an offscreen GWorld with per-pixel rotation,
		 * which is queued for V3. */
		if (fstyle != NULL && fstyle->transform != 0) {
			int rot_deg, tx, ty;
			macos9_transform_unpack(fstyle->transform,
				&rot_deg, &tx, &ty);
			x += tx;
			y += ty;
		}

		/* fixes609: the bold-smear (was fixes70) and sub-12 bitmap-gap
		 * (was fixes144b) breathing-room bumps are now folded into ls
		 * by macos9_run_spacing above, so the measure path applies them
		 * identically and the two can't drift. */

		/* fixes50 - text-shadow pass. Paint the same glyphs at
		 * (x+sx, y+sy) in the shadow colour before the main
		 * pass paints them in the foreground colour. Skip the
		 * shadow if both offsets are zero. Defensive clamp so
		 * a pathological CSS value can't blow past the window. */
		if ((sx != 0 || sy != 0) && fstyle != NULL) {
			RGBColor shadow_rgb;
			short clamped_sx = (short)sx;
			short clamped_sy = (short)sy;
			int pass;
			int passes = (fstyle->shadow_blur > 0) ? 3 : 1;
			if (clamped_sx < -16) clamped_sx = -16;
			if (clamped_sx >  16) clamped_sx =  16;
			if (clamped_sy < -16) clamped_sy = -16;
			if (clamped_sy >  16) clamped_sy =  16;
			macos9_colour_to_rgb(fstyle->shadow_color, &shadow_rgb);
			RGBForeColor(&shadow_rgb);

			for (pass = 0; pass < passes; pass++) {
				short cur_sx = clamped_sx;
				short cur_sy = clamped_sy;
				if (pass == 1) { cur_sx++; cur_sy++; }
				if (pass == 2) { cur_sx--; cur_sy--; }

				if ((ls == 0 && ws == 0) || mac_len <= 1) {
					MoveTo((short)(x + cur_sx),
					       (short)(y + cur_sy));
					DrawText(mac_buf, 0, (short)mac_len);
				} else {
					size_t i;
					short pen_x = (short)(x + cur_sx);
					short cw;
					int gap;
					for (i = 0; i < mac_len; i++) {
						MoveTo(pen_x,
						       (short)(y + cur_sy));
						DrawText(mac_buf, (short)i, 1);
						cw = (short)CharWidth(mac_buf[i]);
						gap = ls;
						if (mac_buf[i] == ' ') gap += ws;
						pen_x = (short)(pen_x + cw + gap);
					}
				}
			}
			/* Restore foreground for the main pass. */
			RGBForeColor(&rgb);
		}

		if (macos9_background_clip_text.active) {
			if (clip_text_state_seen < 12) {
				macsurf_debug_log_writef(
					"LIFE clip-text plot len=%d at=%d,%d",
					(int)mac_len, x, y);
				clip_text_state_seen++;
			}
			macos9_paint_background_clip_text(ctx, mac_buf, mac_len,
					x, y, ls, ws, font_id, face, size);
			/* CSS `color: transparent` is the normal companion to
			 * background-clip:text. QuickDraw has no transparent text fill,
			 * so avoid its opaque foreground pass in that case. */
			if (((fstyle->foreground >> 24) & 0xff) >= 0xfe) {
				macos9_pop_clip(saved_clip);
				return NSERROR_OK;
			}
		}

		if ((ls == 0 && ws == 0) || mac_len <= 1) {
			MoveTo((short)x, (short)y);
			DrawText(mac_buf, 0, (short)mac_len);
		} else {
			/* fixes42 + fixes139b: per-glyph paint path. ls
			 * inserts after every glyph; ws additionally inserts
			 * after each ASCII space. Slower than bulk DrawText
			 * but exercised only when CSS specifies non-default
			 * letter-spacing or word-spacing. */
			size_t i;
			short pen_x = (short)x;
			short cw;
			int gap;
			for (i = 0; i < mac_len; i++) {
				MoveTo(pen_x, (short)y);
				DrawText(mac_buf, (short)i, 1);
				cw = (short)CharWidth(mac_buf[i]);
				gap = ls;
				if (mac_buf[i] == ' ') gap += ws;
				pen_x = (short)(pen_x + cw + gap);
			}
		}
		macos9_pop_clip(saved_clip);
	}
#else
	MoveTo((short)x, (short)y);
	DrawText(text, 0, (short)length);
#endif

	return NSERROR_OK;
}

/* Field order: clip, arc, disc, line, rectangle, polygon, path,
 * bitmap, text, group_start, group_end, flush, option_knockout
 * (see include/netsurf/plotters.h) */
const struct plotter_table macos9_plotters = {
	macos9_plot_clip,
	macos9_plot_arc,
	macos9_plot_disc,
	macos9_plot_line,
	macos9_plot_rectangle,
	macos9_plot_polygon,
	macos9_plot_path,
	macos9_plot_bitmap,
	macos9_plot_text,
	NULL,				/* group_start */
	NULL,				/* group_end */
	NULL,				/* flush */
	true				/* option_knockout */
};
