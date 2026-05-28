/*
 * MacSurf — Mac OS 9 frontend for NetSurf
 * plotters.c — All plotter_table callbacks
 *
 * Phase 5: implement clip, rectangle, text against QuickDraw.
 * Other plotters remain stubs and will be filled in incrementally.
 *
 * This file is part of MacSurf, built on the NetSurf engine.
 * Licensed under GPL v2.
 */

#include <stdlib.h>
#include <string.h>

#include "utils/errors.h"
#include "utils/log.h"
#include "netsurf/types.h"
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

/* fixes144b: sub-AA draw-spacing experiment. QuickDraw bitmap text
 * below the AA floor (set to 12pt in main.c via
 * SetAntiAliasedTextEnabled) draws adjacent glyphs with no visible
 * separator -- "Di"/"Disc"/"Dill" visually collide at 9-10pt
 * Helvetica because the D's painted right edge and the i's left edge
 * land in adjacent or shared pixel columns. fixes144a2's diagnostic
 * probe captured 216 measurements and showed every delta=0; this is
 * a paint-resolution artefact, not a metric error. The fix forces
 * the per-char draw path and adds +1px between glyphs ONLY when
 * size < AA floor and the font is proportional. Measurement (macos9_
 * font_measure) is intentionally NOT touched -- layout, wrap, and
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
 * -- needs NetSurf-core work). This flag is the experiment: ship the
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
 * family dispatches — exactly the cases where the width-vs-paint
 * divergence would matter. width and paint share macos9_font_id_from_style
 * as the dispatch entry point (verified at macos9_font_measure:163 and
 * plot_text:1311), so by construction they cannot disagree on a single
 * fstyle — but any inline-layout drift between segments will show as
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

#ifdef __MACOS9__
#include <Quickdraw.h>
#include <QuickdrawText.h>
#include <Fonts.h>
#include <TextUtils.h>
#else
/* Linux cross-check stubs — match Mac toolbox shapes loosely. */
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
static void
macos9_colour_to_rgb(colour c, RGBColor *out)
{
	unsigned int r = (unsigned int)((c >>  0) & 0xff);
	unsigned int g = (unsigned int)((c >>  8) & 0xff);
	unsigned int b = (unsigned int)((c >> 16) & 0xff);

	/* 8-bit -> 16-bit by replicating the byte (0xAB -> 0xABAB).
	 * Standard QuickDraw idiom — same trick CopyBits / Picture
	 * recording uses. */
	out->red   = (unsigned short)((r << 8) | r);
	out->green = (unsigned short)((g << 8) | g);
	out->blue  = (unsigned short)((b << 8) | b);
}

static void
macos9_rect_from_ns(const struct rect *src, Rect *dst)
{
	dst->left   = (short)src->x0;
	dst->top    = (short)src->y0;
	dst->right  = (short)src->x1;
	dst->bottom = (short)src->y1;
}

/* fixes51 -- case-insensitive byte compare for the small set of
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
	/* fixes145 -- dispatch on generic family. See top-of-file
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
	/* fixes52 -- force Helvetica for every CSS font-family. NetSurf's
	 * inline layout has a baseline-drift bug whenever a single line
	 * mixes fonts with different installed metrics (e.g. body text
	 * + <code>); lines stack 2-4 px on top of each other and become
	 * unreadable. The proper fix is real per-font ascent/descent
	 * through gui_layout_table (deferred — needs NetSurf-core work).
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
extern struct gui_window *macos9_paint_gw;
/* fixes77g -- prefer macos9_paint_gw over GetPort+GetWRefCon. The old
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

/* fixes137: expose scroll origin + viewport dimensions for
 * background-attachment: fixed. NetSurf core's html_redraw_background
 * needs to know where the viewport sits in page coordinates to anchor
 * the bg image origin instead of letting it scroll with the element box.
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
	*out_x = gw->scroll_x;
	*out_y = gw->scroll_y;
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
	effective = (**new_clip).rgnBBox;

	/* fixes91: gated — see macsurf_prefix.h MACSURF_VERBOSE_PLOTLOG. */
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

static nserror
macos9_plot_line(const struct redraw_context *ctx,
		 const plot_style_t *pstyle,
		 const struct rect *line)
{
	RGBColor rgb;
	(void)ctx;
	if (pstyle == NULL || line == NULL) return NSERROR_OK;
	macos9_colour_to_rgb(pstyle->stroke_colour, &rgb);
	RGBForeColor(&rgb);
#ifdef __MACOS9__
	{
		RgnHandle saved_clip = macos9_push_clip();
#endif
	MoveTo((short)line->x0, (short)line->y0);
	LineTo((short)line->x1, (short)line->y1);
#ifdef __MACOS9__
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
 * Sub-degree precision is dropped — V2 accuracy is 1° per step which is
 * imperceptible for typical CSS rotations. */
static void
macos9_transform_unpack(int transform,
			int *rot_deg, int *tx, int *ty)
{
	int rot_q106;
	int deg;
	int8_t tx_px = (int8_t)((((uint32_t)transform) >> 8) & 0xff);
	int8_t ty_px = (int8_t)( ((uint32_t)transform)       & 0xff);

	rot_q106 = (int)((int16_t)((((uint32_t)transform) >> 16) & 0xffff));
	deg = rot_q106 / 64;
	while (deg < 0)   deg += 360;
	while (deg >= 360) deg -= 360;

	*rot_deg = deg;
	*tx = (int)tx_px;
	*ty = (int)ty_px;
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

	/* Fast exact path for the four cardinal rotations — avoids
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

	(void)ctx;

	if (pstyle == NULL || rectangle == NULL)
		return NSERROR_OK;

	macos9_plot_rect_count++;
	macos9_rect_from_ns(rectangle, &r);

#ifdef __MACOS9__
	/* fixes71 -- transform-aware rectangle. When the box has a
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
		 * predates fixes73), treat it as identity — earlier code did
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
			 * box's layout slot before plot.rectangle runs -- so a
			 * scale > 1 fill paints outside the slot and immediately
			 * gets cut. For transform we want to paint freely within
			 * the whole content area; save the existing clip, replace
			 * it with content_rect, then restore. Other plot paths
			 * keep using the tight clip -- only the transform branch
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
	 * (SOLID) here, the cascade dropped the SET status — parser or
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
	 * solid offset rect at 50% grey -- the recognisable Mac OS
	 * floating-window shadow look. */
#ifdef __MACOS9__
	if ((pstyle->box_shadow != 0 || pstyle->box_shadow_y != 0) &&
	    pstyle->fill_type != PLOT_OP_TYPE_NONE &&
	    !pstyle->box_shadow_inset) {
		short hoff = (short)(pstyle->box_shadow   >> PLOT_STYLE_RADIX);
		short voff = (short)(pstyle->box_shadow_y >> PLOT_STYLE_RADIX);
		/* fixes48 -- defensive clamp. */
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
#endif

	/* fixes47 -- real vertical linear gradient (top stop -> bottom).
	 * fixes48 -- horizontal variant fills left-to-right.
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

	/* fixes74 -- radial gradient. fill_colour is the centre colour,
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
		macos9_colour_to_rgb(pstyle->fill_colour,  &c1);
		macos9_colour_to_rgb(pstyle->fill_colour2, &c2);
		saved_clip = macos9_push_clip();
		width  = (long)(r.right - r.left);
		height = (long)(r.bottom - r.top);
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
				ring.left   = (short)(r.left   + inset_x);
				ring.right  = (short)(r.right  - inset_x);
				ring.top    = (short)(r.top    + inset_y);
				ring.bottom = (short)(r.bottom - inset_y);
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
		/* fixes49 -- opacity bucket. plot_style_fixed value with
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
			/* < 5% -- skip painting entirely. */
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
			/* fixes220 — explicitly set RGBBackColor to white before
			 * the stipple FillRect. The pattern paints FG at 1 bits
			 * and BG at 0 bits. If a prior DrawText / image-blit /
			 * etc. left BackColor at black, a 50% stipple of #cc
			 * over #00 averages to #66 dark grey instead of the
			 * intended translucent-over-white look — which is the
			 * "tables are too dark" bug. */
			RGBColor wht;
			RgnHandle saved_clip = macos9_push_clip();
			wht.red = 0xFFFF; wht.green = 0xFFFF; wht.blue = 0xFFFF;
			RGBBackColor(&wht);
			FillRect(&r, &stipple_pat);
			macos9_pop_clip(saved_clip);
		} else {
			RgnHandle saved_clip = macos9_push_clip();
			PaintRect(&r);
			macos9_pop_clip(saved_clip);
		}
#ifdef __MACOS9__
		/* fixes200: inset box-shadow. Paint the offset grey rect
		 * INSIDE the main rect r, clipped to r.
		 *
		 * fixes225 BISECT: gate behind MACSURF_INSET_BOX_SHADOW so
		 * we can prove/disprove that this is what's painting the
		 * dark wash on mactrove. Default 0 = disabled. Flip to 1
		 * to re-enable if mactrove looks the same with this off
		 * (meaning the bug is elsewhere). */
#ifndef MACSURF_INSET_BOX_SHADOW
#define MACSURF_INSET_BOX_SHADOW 0
#endif
		if (MACSURF_INSET_BOX_SHADOW &&
		    pstyle->box_shadow_inset &&
		    (pstyle->box_shadow != 0 || pstyle->box_shadow_y != 0)) {
			short hoff = (short)(pstyle->box_shadow   >> PLOT_STYLE_RADIX);
			short voff = (short)(pstyle->box_shadow_y >> PLOT_STYLE_RADIX);
			RGBColor sh;
			Rect s;
			RgnHandle saved_clip;
			if (pstyle->box_shadow_color != 0) {
				macos9_colour_to_rgb(pstyle->box_shadow_color, &sh);
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
			ClipRect(&r);
			PaintRect(&s);
			macos9_pop_clip(saved_clip);
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
#endif
		FrameRect(&r);
#ifdef __MACOS9__
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
	/* fixes203 — path flattening with fill + stroke.
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
	 * per cubic — the LineTo calls between sample points get
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
			 * new one — out of scope for V1. */
		} else {
			break;
		}
	}
#ifdef __MACOS9__
	ClosePoly();
	if (has_started) {
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
	}
	KillPoly(poly);
#else
	(void)has_started; (void)rgb;
#endif
	return NSERROR_OK;
}

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

	MS_ASSERT(bitmap != NULL, "plot_bitmap: bitmap is NULL");
	(void)ctx; (void)bg;
	if (bitmap == NULL) return NSERROR_OK;

	buf = macos9_bitmap_get_buffer((void *)bitmap);
	if (buf == NULL) return NSERROR_OK;
	bw = macos9_bitmap_get_width((void *)bitmap);
	bh = macos9_bitmap_get_height((void *)bitmap);
	rowstride = (long)macos9_bitmap_get_rowstride((void *)bitmap);
	if (bw <= 0 || bh <= 0) return NSERROR_OK;

	SetRect(&src_rect, 0, 0, (short)bw, (short)bh);
	SetRect(&dst_rect, (short)x, (short)y,
		(short)(x + width), (short)(y + height));

	/* useTempMem (=4) so a large source bitmap (e.g. 1600x1200 JPEG
	 * = 7.7 MB) doesn't exhaust the app heap on every redraw. */
	err = NewGWorld(&gw, 32, &src_rect, NULL, NULL, (GWorldFlags)4);
	if (err != noErr || gw == NULL) {
		MS_LOG("plot_bitmap: NewGWorld tempmem FAIL, retry");
		err = NewGWorld(&gw, 32, &src_rect, NULL, NULL, 0);
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
			dst_row[col * 4 + 0] = 0xFF;
			dst_row[col * 4 + 1] = r;
			dst_row[col * 4 + 2] = g;
			dst_row[col * 4 + 3] = b;
		}
	}

	/* fixes221 — kill switch for fixes203's box-filter pre-downscale.
	 * fixes257 — flipped ON. The dark-grey-wash investigation that
	 * caused fixes221 to disable this turned out to be a separate bug
	 * (fixes225, inset box-shadow). Box-filter is the right call for
	 * downscale: CopyMask's nearest-neighbor 1-bit mask scaling drops
	 * pixels on non-integer ratios, producing the "faded" look that
	 * has plagued every PNG with transparency since fixes128. With
	 * box-filter pre-downscaling, the destination receives a properly
	 * averaged mask that doesn't lose pixels — image is sharper AND
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

	/* fixes203 — box-filter pre-downscale for high-quality image
	 * rendering. QuickDraw's CopyBits / CopyMask scale via nearest-
	 * neighbor, which on large downscale ratios (3x+) produces severe
	 * aliasing (the "rainbow streak" artefact visible on OP1 / OP2 in
	 * fixes202 hardware probes: the mactrove logo at 1058×245 reduced
	 * to ~160×37 by object-fit:contain). Average each src block of
	 * (sx × sy) pixels into one dst pixel of a target-sized
	 * intermediate GWorld, then have the existing blit code copy the
	 * intermediate at 1:1 — no QuickDraw scaling involved.
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
		/* fixes257 — threshold lowered from 3× to 1.5× (q8 = 384).
		 * Below 1.5× the box-filter cost outweighs the visual win
		 * (and the CopyMask fade is mild). Above 1.5× the fade is
		 * visible and box-filter is a clear improvement. mactrove's
		 * 1058x245 logo at 400x92 is 2.6× — under the old 3× gate
		 * it stayed faded; under 1.5× it pre-downscales sharply. */
		if (MACSURF_BOX_FILTER_DOWNSCALE &&
				(sx_ratio_q8 >= (3L * 128L) || sy_ratio_q8 >= (3L * 128L)) &&
				width >= 4 && height >= 4) {
			GWorldPtr gw_small = NULL;
			Rect small_rect;
			PixMapHandle pm_small;
			OSErr small_err;
			SetRect(&small_rect, 0, 0, (short)width, (short)height);
			small_err = NewGWorld(&gw_small, 32, &small_rect, NULL,
					NULL, (GWorldFlags)4);
			if (small_err != noErr || gw_small == NULL) {
				small_err = NewGWorld(&gw_small, 32, &small_rect,
						NULL, NULL, 0);
			}
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

		/* fixes301j — reset foreground to black and background to white
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

		/* fixes203 — if the box-filter pre-downscale above swapped
		 * src_rect from bw×bh to width×height, the original mask
		 * (sized to bw×bh) no longer matches the small pixmap.
		 * Box-filter the mask to a fresh dest-sized buffer
		 * (rowbytes rounded up to whole words for QuickDraw),
		 * UNION over the source block. Owned locally; freed below. */
		if (mask_data != NULL &&
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

		/* fixes190 — Revert fixes188 composite branch. The
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
						&((GrafPtr)save_port)->portBits,
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
						&((GrafPtr)save_port)->portBits,
						&src_rect, &tile_dst,
						srcCopy, NULL);
					if (++tile_count >= tile_cap)
						goto blit_done;
				}
			}
		}
blit_done:
		macos9_pop_clip(saved_clip);
		/* fixes203 — release the box-filtered mask buffer. */
		if (bf_small_mask != NULL) {
			free(bf_small_mask);
		}
	}

	UnlockPixels(pm);
	DisposeGWorld(gw);
#else
	(void)ctx; (void)bitmap;
	(void)x; (void)y; (void)width; (void)height; (void)bg; (void)flags;
#endif
	return NSERROR_OK;
}

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
		ls = (fstyle != NULL) ? fstyle->letter_spacing : 0;
		/* fixes139b: word-spacing pulled out so the per-char branch
		 * below can add it whenever the just-drawn glyph is an ASCII
		 * space. ws == 0 (the common case) keeps the fast DrawText
		 * path eligible. */
		ws = (fstyle != NULL) ? fstyle->word_spacing : 0;
		sx = (fstyle != NULL) ? fstyle->shadow_x : 0;
		sy = (fstyle != NULL) ? fstyle->shadow_y : 0;

		/* fixes71/72 -- text-side transform.
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

		/* fixes70: bold smear breathing room (the real fix).
		 * QuickDraw fakes bold by drawing each glyph twice with a
		 * 1-pixel right shift. The smear from glyph N paints into
		 * glyph N+1's slot, fusing letter pairs like "OB"/"BE" in
		 * tight bold runs (visible on PROBE card headings).
		 *
		 * font_width slop (fixes69) reserves enough space for the
		 * whole bold run in layout, but within one DrawText call
		 * QuickDraw still positions glyphs at their natural bold
		 * advances and the smear collides. Real fix: add +1 to
		 * effective letter-spacing for bold runs, which forces the
		 * per-char draw path below and inserts a real pixel between
		 * each glyph. Smear has its breathing room and "PROBE"
		 * letters stop fusing. */
		if ((face & 1) && mac_len > 1) {
			ls += 1;
		}

#if MACSURF_SUBAA_DRAW_SPACING
		/* fixes144b: sub-AA bitmap glyph-overlap optical correction.
		 * Below 12pt (the AA floor set in main.c) QuickDraw paints
		 * raw bitmaps with no anti-aliased transition pixel between
		 * adjacent glyphs, so Helvetica's "Di" pair has D's right
		 * edge touching i's left edge with no visible gap. Adding
		 * +1 to ls forces the per-char draw branch below and inserts
		 * a real pixel between every glyph. Skipped for Monaco
		 * (monospaced — every advance is already wider than the
		 * painted glyph, no overlap possible). Measurement is left
		 * untouched so layout / wrap / text-overflow stay stable.
		 * See plotters.c top-of-file comment for full rationale. */
		if (size < 12 && font_id != kFontIDMonaco && mac_len > 1) {
			ls += 1;
		}
#endif

		/* fixes50 -- text-shadow pass. Paint the same glyphs at
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
