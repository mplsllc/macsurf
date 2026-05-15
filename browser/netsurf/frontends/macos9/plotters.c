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

/* Diagnostic counters - read from main.c after redraw. */
long macos9_plot_text_count = 0;
long macos9_plot_rect_count = 0;

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
	 * any pt size). Cost: <code>/<kbd>/<samp> blocks lose their
	 * monospace look — they stay readable, just not visually
	 * distinct from body text. Bold/italic face variants still
	 * apply via macos9_face_from_style, so emphasis is preserved. */
	(void)fstyle;
	return kFontIDHelvetica;
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
static RgnHandle macos9_push_clip(void)
{
	GrafPtr port;
	WindowRef win;
	struct gui_window *gw;
	RgnHandle saved_clip;
	RgnHandle content_rgn;

	GetPort(&port);
	win = (WindowRef)port;
	gw = (struct gui_window *)GetWRefCon(win);
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
	GrafPtr port;
	WindowRef win;
	struct gui_window *gw;
	RgnHandle new_clip;
	RgnHandle content_rgn;
	Rect effective;
#endif

	(void)ctx;
	if (clip == NULL) return NSERROR_OK;
	macos9_rect_from_ns(clip, &r);

#ifdef __MACOS9__
	GetPort(&port);
	win = (WindowRef)port;
	gw = (struct gui_window *)GetWRefCon(win);
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

	macsurf_debug_log_writef("plot_clip in=(%d,%d,%d,%d) content=(%d,%d,%d,%d) effective=(%d,%d,%d,%d)",
	       r.left, r.top, r.right, r.bottom,
	       gw->content_rect.left, gw->content_rect.top, gw->content_rect.right, gw->content_rect.bottom,
	       effective.left, effective.top, effective.right, effective.bottom);

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

			saved_clip = macos9_push_clip();
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
			macos9_pop_clip(saved_clip);
			return NSERROR_OK;
		}
	}
#endif

	/* Diagnostic: dump fill / stroke colour + rect for the first few
	 * rectangles each redraw so we can compare what libcss decided
	 * vs. what UA default would produce. */
	if (macos9_plot_rect_count <= 8) {
		unsigned int fr = (unsigned int)((pstyle->fill_colour >>  0) & 0xff);
		unsigned int fg = (unsigned int)((pstyle->fill_colour >>  8) & 0xff);
		unsigned int fb = (unsigned int)((pstyle->fill_colour >> 16) & 0xff);
		unsigned int sr = (unsigned int)((pstyle->stroke_colour >>  0) & 0xff);
		unsigned int sg = (unsigned int)((pstyle->stroke_colour >>  8) & 0xff);
		unsigned int sb = (unsigned int)((pstyle->stroke_colour >> 16) & 0xff);
		macsurf_debug_log_writef(
			"plot_rect[%d] fill=%d/%d/%d ft=%d stroke=%d/%d/%d st=%d at (%d,%d,%d,%d)",
			(int)macos9_plot_rect_count,
			(int)fr, (int)fg, (int)fb, (int)pstyle->fill_type,
			(int)sr, (int)sg, (int)sb, (int)pstyle->stroke_type,
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
	 * The shadow size comes from css_computed_box_shadow simplified
	 * to a single fixed-point integer (see plot_style_s.box_shadow).
	 * QuickDraw has no blur primitive, so we approximate with a
	 * solid offset rect at 50% grey -- the recognisable Mac OS
	 * floating-window shadow look. */
#ifdef __MACOS9__
	if ((pstyle->box_shadow != 0 || pstyle->box_shadow_y != 0) &&
	    pstyle->fill_type != PLOT_OP_TYPE_NONE) {
		short hoff = (short)(pstyle->box_shadow   >> PLOT_STYLE_RADIX);
		short voff = (short)(pstyle->box_shadow_y >> PLOT_STYLE_RADIX);
		/* fixes48 -- defensive clamp; pages with absurd offsets
		 * (e.g. inset shadows we don't render specially) still
		 * shouldn't blow past the visible window. */
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
#endif

	if (pstyle->fill_type != PLOT_OP_TYPE_NONE &&
	    pstyle->fill_type != PLOT_OP_TYPE_LINEAR_GRADIENT &&
	    pstyle->fill_type != PLOT_OP_TYPE_LINEAR_GRADIENT_H) {
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
			/* Backcolor stays at whatever the port has — usually
			 * white for body content. The pattern mixes fg + bg
			 * at the dot level. */
			RgnHandle saved_clip = macos9_push_clip();
			FillRect(&r, &stipple_pat);
			macos9_pop_clip(saved_clip);
		} else {
			RgnHandle saved_clip = macos9_push_clip();
			PaintRect(&r);
			macos9_pop_clip(saved_clip);
		}
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
	/* Path flattening: treat the path as a sequence of
	 * PLOTTER_PATH_* tokens (MOVE / LINE / BEZIER / CLOSE)
	 * and emit LineTo() for each straight segment. Bezier
	 * curves are approximated by sampling 8 points along
	 * each cubic and LineTo-ing between them. No transform
	 * handling (matrix is identity for all non-SVG content). */
	RGBColor rgb;
	unsigned int i;
	float cx, cy;
	(void)transform;
	if (pstyle == NULL || p == NULL || n == 0) return NSERROR_OK;
	macos9_colour_to_rgb(pstyle->stroke_colour, &rgb);
	RGBForeColor(&rgb);
	cx = 0.0f; cy = 0.0f;
	i = 0;
	while (i < n) {
		unsigned int op = (unsigned int)p[i++];
		if (op == PLOTTER_PATH_MOVE) {
			if (i + 1 >= n) break;
			cx = p[i]; cy = p[i + 1]; i += 2;
			MoveTo((short)cx, (short)cy);
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
			/* no explicit op needed for QuickDraw lines */
		} else {
			break;
		}
	}
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
	(void)ctx; (void)bg; (void)flags;
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

	err = NewGWorld(&gw, 32, &src_rect, NULL, NULL, 0);
	if (err != noErr || gw == NULL) return NSERROR_OK;

	pm = GetGWorldPixMap(gw);
	if (pm == NULL || !LockPixels(pm)) {
		DisposeGWorld(gw);
		return NSERROR_OK;
	}

	/* Copy bitmap buffer (RGBA) to GWorld (ARGB on PPC). */
	dst_rowbytes = (*pm)->rowBytes & 0x3FFF;
	for (row = 0; row < bh; row++) {
		src_row = buf + row * rowstride;
		dst_row = (unsigned char *)GetPixBaseAddr(pm) + row * dst_rowbytes;
		for (col = 0; col < bw; col++) {
			unsigned char r = src_row[col * 4 + 0];
			unsigned char g = src_row[col * 4 + 1];
			unsigned char b = src_row[col * 4 + 2];
			unsigned char a = src_row[col * 4 + 3];
			/* ARGB big-endian */
			dst_row[col * 4 + 0] = a;
			dst_row[col * 4 + 1] = r;
			dst_row[col * 4 + 2] = g;
			dst_row[col * 4 + 3] = b;
		}
	}

	{
		GrafPtr save_port;
		RgnHandle saved_clip;
		GetPort(&save_port);
		saved_clip = macos9_push_clip();
		CopyBits((BitMap *)*pm,
			&((GrafPtr)save_port)->portBits,
			&src_rect, &dst_rect, srcCopy, NULL);
		macos9_pop_clip(saved_clip);
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
		int sx;
		int sy;

		mac_len = macos9_utf8_to_macroman(text, length, mac_buf,
				sizeof(mac_buf));
		saved_clip = macos9_push_clip();
		ls = (fstyle != NULL) ? fstyle->letter_spacing : 0;
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

		/* fixes50 -- text-shadow pass. Paint the same glyphs at
		 * (x+sx, y+sy) in the shadow colour before the main
		 * pass paints them in the foreground colour. Skip the
		 * shadow if both offsets are zero. Defensive clamp so
		 * a pathological CSS value can't blow past the window. */
		if ((sx != 0 || sy != 0) && fstyle != NULL) {
			RGBColor shadow_rgb;
			short clamped_sx = (short)sx;
			short clamped_sy = (short)sy;
			if (clamped_sx < -16) clamped_sx = -16;
			if (clamped_sx >  16) clamped_sx =  16;
			if (clamped_sy < -16) clamped_sy = -16;
			if (clamped_sy >  16) clamped_sy =  16;
			macos9_colour_to_rgb(fstyle->shadow_color, &shadow_rgb);
			RGBForeColor(&shadow_rgb);
			if (ls == 0 || mac_len <= 1) {
				MoveTo((short)(x + clamped_sx),
				       (short)(y + clamped_sy));
				DrawText(mac_buf, 0, (short)mac_len);
			} else {
				size_t i;
				short pen_x = (short)(x + clamped_sx);
				short cw;
				for (i = 0; i < mac_len; i++) {
					MoveTo(pen_x,
					       (short)(y + clamped_sy));
					DrawText(mac_buf, (short)i, 1);
					cw = (short)CharWidth(mac_buf[i]);
					pen_x = (short)(pen_x + cw + ls);
				}
			}
			/* Restore foreground for the main pass. */
			RGBForeColor(&rgb);
		}

		if (ls == 0 || mac_len <= 1) {
			MoveTo((short)x, (short)y);
			DrawText(mac_buf, 0, (short)mac_len);
		} else {
			/* fixes42: letter-spacing fast-fallback. QuickDraw
			 * has no built-in CharExtra; draw one MacRoman
			 * glyph at a time, advancing pen by the glyph width
			 * plus letter-spacing pixels. Slower than the bulk
			 * DrawText but exercised only when CSS specifies a
			 * non-default letter-spacing. */
			size_t i;
			short pen_x = (short)x;
			short cw;
			for (i = 0; i < mac_len; i++) {
				MoveTo(pen_x, (short)y);
				DrawText(mac_buf, (short)i, 1);
				cw = (short)CharWidth(mac_buf[i]);
				pen_x = (short)(pen_x + cw + ls);
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
