#include <stdlib.h>
#include <string.h>
#include "utils/errors.h"
#include "utils/log.h"
#include "utils/nsurl.h"
#include "netsurf/types.h"
#include "netsurf/window.h"
#include "netsurf/mouse.h"
#include "netsurf/browser_window.h"
#include "desktop/browser_history.h"
#include "macos9.h"
#include "macsurf_config.h"
#include "macsurf_debug.h"

#ifdef __MACOS9__
#include <MacWindows.h>
#include <Controls.h>
#include <Appearance.h>
#include <Quickdraw.h>
#include <TextEdit.h>
#endif

static struct gui_window *window_list = NULL;
static struct gui_window *macos9_window_create(struct browser_window *bw, struct gui_window *ex, gui_window_create_flags f);

/* fixes294 — Phase 0 favicon plumbing.
 *
 * Lessons from the failed fixes292/293 attempts:
 *   - Inserting a Rect field in the middle of struct gui_window caused
 *     CW8 missed-recompile corruption (other .c files reading
 *     content_rect / status_rect at stale offsets).  This attempt
 *     keeps ALL state in file-scope statics — no struct changes at all.
 *   - set_icon callback wiring is harmless on its own (NetSurf core
 *     substitutes an empty default when slot is NULL anyway), but Phase 0
 *     deliberately doesn't wire it.  The static default icon is loaded
 *     once at startup and painted in every URL bar; per-site favicon
 *     swap is Phase 1.
 *
 * The 718-byte 16.png is baked in as a const byte array, decoded once
 * via lodepng at startup, and painted on top of the URL bar after
 * TEUpdate.  compute_url_te_rect's left offset shifts from +4 to +20
 * so the URL text doesn't overlap. */
extern unsigned lodepng_decode32(unsigned char **out, unsigned *w, unsigned *h,
		const unsigned char *in, unsigned long insize);
extern void *macos9_bitmap_create(int width, int height, unsigned int state);
extern unsigned char *macos9_bitmap_get_buffer(void *bitmap);
extern int macos9_bitmap_get_width(void *bitmap);
extern int macos9_bitmap_get_height(void *bitmap);

static const unsigned char macos9_default_favicon_png[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x10,
    0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0xF3, 0xFF, 0x61, 0x00, 0x00, 0x00,
    0x09, 0x70, 0x48, 0x59, 0x73, 0x00, 0x00, 0x0E, 0xC3, 0x00, 0x00, 0x0E,
    0xC3, 0x01, 0xC7, 0x6F, 0xA8, 0x64, 0x00, 0x00, 0x00, 0x19, 0x74, 0x45,
    0x58, 0x74, 0x53, 0x6F, 0x66, 0x74, 0x77, 0x61, 0x72, 0x65, 0x00, 0x77,
    0x77, 0x77, 0x2E, 0x69, 0x6E, 0x6B, 0x73, 0x63, 0x61, 0x70, 0x65, 0x2E,
    0x6F, 0x72, 0x67, 0x9B, 0xEE, 0x3C, 0x1A, 0x00, 0x00, 0x02, 0x5B, 0x49,
    0x44, 0x41, 0x54, 0x38, 0x8D, 0xA5, 0x93, 0x5D, 0x48, 0x93, 0x61, 0x14,
    0xC7, 0xFF, 0xCF, 0x9E, 0xF7, 0xDD, 0x96, 0xDB, 0xCC, 0xD7, 0x6D, 0xEA,
    0xDE, 0x69, 0xE1, 0x4C, 0xB2, 0xE6, 0xCC, 0xA6, 0x66, 0x16, 0x65, 0xE0,
    0x95, 0x16, 0x8A, 0x53, 0x2F, 0x22, 0x8A, 0x6E, 0x42, 0xBA, 0xF2, 0x46,
    0x85, 0x08, 0x8C, 0xA0, 0x08, 0xE9, 0xA2, 0x08, 0xBA, 0x50, 0x86, 0x74,
    0x21, 0x49, 0x1F, 0x48, 0x89, 0x58, 0x62, 0x41, 0x57, 0x65, 0x5E, 0x18,
    0x92, 0x89, 0x1F, 0x2B, 0xA7, 0x6B, 0x1F, 0xA8, 0x18, 0xAE, 0xB9, 0x2F,
    0xDF, 0x77, 0x4F, 0x77, 0xB1, 0x68, 0x8D, 0xA8, 0xFF, 0xE5, 0x39, 0xF0,
    0xE3, 0x77, 0x0E, 0xE7, 0x10, 0xC6, 0x18, 0xFE, 0x27, 0x8A, 0x54, 0xC5,
    0x92, 0xEA, 0x0B, 0x85, 0xC5, 0x75, 0xED, 0x45, 0x7F, 0x03, 0x20, 0xC9,
    0x06, 0xA6, 0x23, 0xAD, 0x56, 0x4E, 0x66, 0xFD, 0x1A, 0x9D, 0xA6, 0x3A,
    0x5B, 0xAF, 0xA7, 0x92, 0x2C, 0x27, 0x64, 0x49, 0xDA, 0x8A, 0x45, 0xE2,
    0xBD, 0xB3, 0xAF, 0x9D, 0xBD, 0x69, 0x01, 0x7B, 0xEC, 0x4D, 0x95, 0x12,
    0xA1, 0x93, 0x2A, 0x8E, 0xA3, 0x5D, 0x1D, 0xE7, 0x20, 0xE6, 0xEA, 0x91,
    0x63, 0x10, 0xE0, 0xF3, 0xAF, 0x63, 0xF8, 0xC5, 0x3B, 0xF6, 0x79, 0xC1,
    0x35, 0xD6, 0x30, 0x3E, 0xD0, 0x78, 0x8D, 0xB1, 0x44, 0xCA, 0x11, 0x12,
    0xA0, 0x23, 0x06, 0x21, 0x93, 0x0E, 0xF6, 0xF5, 0xA0, 0xB6, 0xE6, 0x10,
    0x64, 0x49, 0xC2, 0x8D, 0x5E, 0x27, 0xB6, 0x82, 0x21, 0x74, 0x5E, 0x6E,
    0x21, 0xA5, 0xFB, 0x72, 0x4F, 0x9F, 0xB9, 0x68, 0x5A, 0x7F, 0xD0, 0x66,
    0x3B, 0xF5, 0x9B, 0x41, 0xFE, 0x61, 0xC7, 0x5D, 0x6B, 0x69, 0x51, 0xC7,
    0xCD, 0x2B, 0x97, 0xB0, 0xE2, 0xF1, 0x62, 0xD9, 0xED, 0xC3, 0xB3, 0xD1,
    0x37, 0x98, 0xF9, 0xB8, 0x84, 0xAA, 0x8A, 0x83, 0xB8, 0x77, 0xBB, 0x0B,
    0xC1, 0xD0, 0x36, 0xCA, 0xFA, 0x8F, 0x61, 0x53, 0x52, 0x26, 0x3E, 0x6D,
    0xEB, 0x46, 0x42, 0x31, 0x77, 0x6B, 0xDB, 0x63, 0x26, 0x13, 0xC6, 0x18,
    0xCC, 0x15, 0x8E, 0xE0, 0xAD, 0x9E, 0x76, 0x9D, 0xED, 0x80, 0x05, 0x3C,
    0xC7, 0x01, 0x00, 0x02, 0x81, 0x0D, 0x3C, 0x7C, 0x32, 0x8E, 0x86, 0xDA,
    0x52, 0x54, 0x71, 0x8B, 0x88, 0x98, 0xAB, 0x91, 0xE5, 0x6C, 0x81, 0x9E,
    0x46, 0xB1, 0x16, 0x57, 0x61, 0x25, 0xBA, 0xCB, 0xE3, 0x0D, 0x7B, 0x0B,
    0x09, 0xEA, 0xEB, 0x55, 0x62, 0x40, 0x1D, 0x15, 0x76, 0x6B, 0x31, 0xD8,
    0xD7, 0x03, 0x8E, 0xA3, 0xBF, 0x2C, 0xC9, 0xF8, 0xAA, 0x13, 0xCA, 0x6F,
    0x2E, 0xC8, 0xAA, 0x2C, 0x48, 0x8B, 0x73, 0xD0, 0x73, 0x11, 0x2C, 0x85,
    0xB5, 0x50, 0x2B, 0x64, 0xBC, 0xDD, 0xCA, 0xBE, 0x43, 0xCD, 0x5A, 0x9B,
    0x03, 0x04, 0x6D, 0xD1, 0x58, 0x1C, 0x7E, 0xFF, 0x06, 0x8E, 0xD7, 0x94,
    0x81, 0x24, 0x01, 0xB4, 0x5F, 0x46, 0xA1, 0xDC, 0x9C, 0x87, 0xDB, 0x1B,
    0xC2, 0xF9, 0x89, 0x62, 0x3C, 0xDF, 0xC8, 0x03, 0xE5, 0x80, 0xF2, 0xCC,
    0xEF, 0x60, 0x40, 0x3E, 0xD5, 0x89, 0x25, 0x5D, 0x00, 0x29, 0x07, 0x00,
    0xB7, 0x27, 0x00, 0x31, 0xCF, 0x00, 0xCB, 0x5E, 0xF1, 0x27, 0x60, 0x85,
    0x99, 0x20, 0xF0, 0x11, 0xBC, 0x1C, 0x03, 0xEC, 0xF3, 0x73, 0xD0, 0x04,
    0x63, 0x98, 0x5A, 0xD3, 0xE1, 0x51, 0xC0, 0x84, 0xA9, 0xB0, 0x20, 0x11,
    0xD1, 0xDE, 0x3C, 0x0F, 0x86, 0xFD, 0xC9, 0xDA, 0x19, 0x19, 0x6A, 0xD4,
    0x9D, 0xAC, 0x40, 0xBE, 0x68, 0x44, 0x81, 0x98, 0x83, 0xED, 0x50, 0x48,
    0x76, 0x77, 0x5F, 0x5D, 0xE0, 0x76, 0x76, 0x84, 0xA8, 0x82, 0x8F, 0xEF,
    0x28, 0xC8, 0x4C, 0x98, 0xE3, 0xEF, 0x0F, 0xF8, 0x66, 0x27, 0x88, 0x68,
    0x6F, 0x5E, 0x05, 0x43, 0x41, 0xDA, 0x6B, 0x03, 0x62, 0x5F, 0xA7, 0x87,
    0xD5, 0xA9, 0x7A, 0x0A, 0xCA, 0x54, 0x47, 0x01, 0xE6, 0x4A, 0x07, 0x60,
    0x80, 0xD2, 0x58, 0xD2, 0xA4, 0x4B, 0x09, 0x58, 0x9D, 0x1E, 0xF2, 0x09,
    0x71, 0xDE, 0x0A, 0x42, 0xAE, 0x03, 0x88, 0xFE, 0x49, 0x42, 0xA9, 0xA1,
    0x4F, 0x53, 0x36, 0x92, 0x7F, 0xC1, 0x5C, 0xD9, 0x68, 0x40, 0x82, 0x3F,
    0x0B, 0xC2, 0x4E, 0x80, 0xC1, 0x02, 0xC6, 0x28, 0x08, 0xB2, 0x41, 0xE8,
    0x7B, 0x65, 0x82, 0xEF, 0x5E, 0xFE, 0x30, 0xE4, 0x4E, 0x0B, 0xF8, 0x97,
    0xFC, 0x00, 0x7E, 0x79, 0xEA, 0x16, 0xF5, 0x2B, 0x2E, 0xC6, 0x00, 0x00,
    0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
};
static const unsigned long macos9_default_favicon_png_len = 718;

static void *macos9_default_favicon_bitmap = NULL;    /* struct macos9_bitmap * */
static GWorldPtr macos9_default_favicon_gworld = NULL;
static Rect macos9_default_favicon_src_rect;
static int  macos9_default_favicon_loaded = 0;

/* fixes295 Phase 1b — active per-site favicon GWorld.  When NetSurf
 * resolves a page's <link rel=icon> or default /favicon.ico, set_icon
 * receives the hlcache_handle.  We pull the bitmap via
 * content_get_bitmap (Phase 1a makes this work for PNG natural-size),
 * build a fresh 16x16 GWorld from the source, and swap the active
 * pointer.  draw_favicon prefers the active over the default.  On
 * set_icon(NULL) or navigate-away, dispose active and revert.
 *
 * Single-window scope.  Multi-window distinct-favicons-per-window is a
 * separate enhancement; today MacSurf typically runs one window. */
static GWorldPtr macos9_active_favicon_gworld = NULL;
static Rect macos9_active_favicon_src_rect;

struct gui_window *macos9_find_window(WindowRef w) { struct gui_window *g; for(g=window_list;g;g=g->next) if(g->window==w) return g; return NULL; }
struct gui_window *macos9_window_list_head(void) { return window_list; }
static void set_status_text(struct gui_window *g, const char *m) { if(!m) g->status[0]=0; else { strncpy(g->status,m,127); g->status[127]=0; } }
/* fixes294 — shift TE field's left by +20 to leave room for the favicon. */
static void compute_url_te_rect(const Rect *u, Rect *o) { o->left=(short)(u->left+20); o->top=(short)(u->top+2); o->right=(short)(u->right-4); o->bottom=(short)(u->bottom-2); }

/* fixes294 — return the favicon's paint rect inside a given url_rect. */
static void compute_favicon_rect(const Rect *u, Rect *o)
{
	short top;
	top = (short)(u->top + ((u->bottom - u->top) - 16) / 2);
	o->left = (short)(u->left + 3);
	o->right = (short)(u->left + 19);
	o->top = top;
	o->bottom = (short)(top + 16);
}

void macos9_window_layout(struct gui_window *g) {
	Rect c; short w, h, ux, ur, cb, ht; if(!g||!g->window) return;
	GetWindowBounds(g->window, 33, &c); w=(short)(c.right-c.left); h=(short)(c.bottom-c.top);
	ux=(short)(4+4*64); ur=(short)(w-4); SetRect(&g->url_rect, ux, 8, ur, 30);
	ht=(short)(h-15); cb=(short)(ht-16); SetRect(&g->content_rect, 0, 38, (short)(w-15), cb); SetRect(&g->status_rect, 0, cb, (short)(w-15), ht);
	if(g->vscroll) { MoveControl(g->vscroll, (short)(w-15), 37); SizeControl(g->vscroll, 16, (short)(cb-36)); }
	if(g->hscroll) { MoveControl(g->hscroll, -1, ht); SizeControl(g->hscroll, (short)(w-13), 16); }
}

void macos9_window_invalidate_all(struct gui_window *g) { Rect r; if(!g||!g->window)return; GetWindowBounds(g->window, 33, &r); r.right=(short)(r.right-r.left); r.bottom=(short)(r.bottom-r.top); r.left=0; r.top=0; InvalWindowRect(g->window, &r); }
void macos9_window_invalidate_content(struct gui_window *g) { if(!g||!g->window)return; InvalWindowRect(g->window, &g->content_rect); }

/* fixes76c -- invalidate a single rect, clipped to content_rect.
 * x, y are window coords (already include the content_rect.top /
 * scroll offset because redraw.c receives that offset from main.c
 * and walks the box tree adding it). Used by the animation tick to
 * avoid full-content redraws (whole-page flashing on OS 9 hardware).
 *
 * fixes76b shipped this with a page-coord->window-coord conversion
 * (`content_rect.top + (py - scroll_y)`) that double-applied the
 * chrome offset: the invalidated rect ended up 38 px below the
 * actual box. Visible symptom: top half of each animated badge
 * never repainted, showing one frozen opacity above the live one. */
void macos9_window_invalidate_rect(struct gui_window *g, int px, int py, int pw, int ph) {
#ifdef __MACOS9__
	Rect r;
	int wx0, wy0, wx1, wy1;
	if (!g || !g->window) return;
	wx0 = px;
	wy0 = py;
	wx1 = px + pw;
	wy1 = py + ph;
	if (wx0 < g->content_rect.left)  wx0 = g->content_rect.left;
	if (wy0 < g->content_rect.top)   wy0 = g->content_rect.top;
	if (wx1 > g->content_rect.right) wx1 = g->content_rect.right;
	if (wy1 > g->content_rect.bottom)wy1 = g->content_rect.bottom;
	if (wx1 <= wx0 || wy1 <= wy0) return;
	r.left = (short)wx0; r.top = (short)wy0;
	r.right = (short)wx1; r.bottom = (short)wy1;
	InvalWindowRect(g->window, &r);
#else
	(void)g; (void)px; (void)py; (void)pw; (void)ph;
#endif
}

void macos9_window_update_scrollbars(struct gui_window *g) {
	int vw, vh, mx, my; if(!g) return;
	vw=g->content_rect.right-g->content_rect.left; vh=g->content_rect.bottom-g->content_rect.top;
	mx=g->content_width-vw; my=g->content_height-vh; if(mx<0) mx=0; if(my<0) my=0;
#ifdef __MACOS9__
	SetPortWindowPort(g->window);
	if(g->vscroll) { SetControlMaximum(g->vscroll, (short)(my>32767?32767:my)); SetControlValue(g->vscroll, (short)g->scroll_y); HiliteControl(g->vscroll, (short)(my>0?0:255)); Draw1Control(g->vscroll); }
	if(g->hscroll) { SetControlMaximum(g->hscroll, (short)(mx>32767?32767:mx)); SetControlValue(g->hscroll, (short)g->scroll_x); HiliteControl(g->hscroll, (short)(mx>0?0:255)); Draw1Control(g->hscroll); }
#endif
}

void macos9_window_scroll_to(struct gui_window *g, int nx, int ny) { if(!g) return; g->scroll_x=nx; g->scroll_y=ny; macos9_window_update_scrollbars(g); macos9_window_invalidate_content(g); }
void macos9_window_scroll_by(struct gui_window *g, int dx, int dy) { if(g) macos9_window_scroll_to(g, g->scroll_x+dx, g->scroll_y+dy); }

void macos9_window_handle_scrollbar_click(struct gui_window *g, ControlRef c, short p, void *lp) {
#ifdef __MACOS9__
	Point pt;
	short step = 48, page = 200;
	int cur, mx;
	if(!g || !c || !lp) return;
	pt = *(Point*)lp;
	SetPortWindowPort(g->window);
	cur = (c == g->vscroll) ? g->scroll_y : g->scroll_x;
	mx  = (c == g->vscroll) ? GetControlMaximum(g->vscroll) : GetControlMaximum(g->hscroll);
	switch (p) {
	case 20: cur -= step; break;          /* up/left arrow */
	case 21: cur += step; break;          /* down/right arrow */
	case 22: cur -= page; break;          /* page up/left */
	case 23: cur += page; break;          /* page down/right */
	case 129:                              /* thumb drag */
		TrackControl(c, pt, NULL);
		cur = GetControlValue(c);
		break;
	default:
		TrackControl(c, pt, NULL);
		cur = GetControlValue(c);
		break;
	}
	if (cur < 0) cur = 0;
	if (cur > mx) cur = mx;
	if (c == g->vscroll) macos9_window_scroll_to(g, g->scroll_x, cur);
	else                 macos9_window_scroll_to(g, cur, g->scroll_y);
#endif
}

void macos9_window_te_activate_url(struct gui_window *g) { if(!g||!g->url_te||g->url_field_active) return; SetPortWindowPort(g->window); TEActivate(g->url_te); g->url_field_active=1; InvalWindowRect(g->window, &g->url_rect); }
void macos9_window_te_deactivate_url(struct gui_window *g) { if(!g||!g->url_te||!g->url_field_active) return; SetPortWindowPort(g->window); TEDeactivate(g->url_te); g->url_field_active=0; InvalWindowRect(g->window, &g->url_rect); }

static void set_url_te_text(struct gui_window *g, const char *u) {
	CharsHandle h;
	long new_len;
	long cur_len;
	if(!g||!g->url_te||!u) return;
	/* fixes109 — dedupe. NetSurf core calls gui_window->set_url repeatedly
	 * during navigation (initial, after redirect, on every history nav, on
	 * some progress events). The old unconditional InvalRect on url_rect
	 * triggered an updateEvt → browser_window_redraw → draw_url_bar cycle
	 * on every call, even when the URL string was byte-identical. On a
	 * loading page the URL bar would visibly pulse for many seconds with
	 * nothing changing — that was a big part of the "sticky" feeling. Now:
	 * compare against the current TE buffer and skip if equal. */
	new_len = (long)strlen(u);
	h = TEGetText(g->url_te);
	if (h != NULL) {
		cur_len = GetHandleSize((Handle)h);
		if (cur_len == new_len &&
		    (new_len == 0 || memcmp(*h, u, (size_t)new_len) == 0)) {
			return;
		}
	}
	SetPortWindowPort(g->window);
	TESetText(u, new_len, g->url_te);
	TECalText(g->url_te);
	InvalWindowRect(g->window, &g->url_rect);
}

void macos9_window_navigate(struct gui_window *g, const char *u) {
	struct nsurl *n;
	nserror nav_e;
	long uu_len;
	int i;
	MS_LOG("navigate:");
	MS_LOG(u ? u : "(null)");
	if(!g||!u||!u[0]) { MS_LOG("nav: no g or empty u"); return; }
	if(!g->bw) { MS_LOG("nav: no bw"); return; }
	uu_len = (long)strlen(u);
	macsurf_debug_log_writef("nav: url len=%ld bytes", uu_len);
	for(i = 0; u[i] != 0; i++) {
		unsigned char c = (unsigned char)u[i];
		if (c < 0x20 || c == 0x7F) {
			macsurf_debug_log_writef("nav: ctrl-char at %d = 0x%02x", i, (int)c);
		}
	}
	set_url_te_text(g,u);
	set_status_text(g,"Loading...");
	if(g->window) InvalWindowRect(g->window, &g->status_rect);
	if(nsurl_create(u,&n)!=NSERROR_OK) { MS_LOG("nav: nsurl_create FAIL"); return; }
	MS_LOG("nav: calling browser_window_navigate");
	{
		/* fixes161a — mark the next http_setup() as DOCUMENT so the
		 * resource governor gives it document-class priority, regardless
		 * of URL suffix. Single-shot: consumed by the first setup call. */
		extern void macos9_http_mark_next_as_document(void);
		extern void macsurf_site_navigation_reset(void);
		macos9_http_mark_next_as_document();
		/* fixes168f — clear per-page heavy latch + rgov skip counters
		 * so the next page is assessed fresh. */
		macsurf_site_navigation_reset();
	}
	nav_e = browser_window_navigate(g->bw, n, NULL, BW_NAVIGATE_HISTORY, NULL, NULL, NULL);
	macsurf_debug_log_writef("nav: bw_navigate returned %d", (int)nav_e);
	nsurl_unref(n);
	MS_LOG("nav: done");
}

void macos9_window_address_bar_submit(struct gui_window *g) {
	CharsHandle h; long l; char r[1024], f[1024];
	long i, j;
	MS_LOG("URL submit fired");
	if(!g||!g->url_te) { MS_LOG("submit: no g or url_te"); return; }
	h=TEGetText(g->url_te); if(!h) { MS_LOG("submit: TEGetText null"); return; }
	l=GetHandleSize((Handle)h); if(l<=0) { MS_LOG("submit: empty"); return; }
	if(l>1023) l=1023; HLock((Handle)h); memcpy(r,*h,(size_t)l); HUnlock((Handle)h); r[l]=0;
	/* Strip control chars (CR/LF/tab/embedded NUL) and leading whitespace. */
	j = 0;
	for (i = 0; i < l; i++) {
		unsigned char c = (unsigned char)r[i];
		if (c >= 0x20 && c != 0x7F) {
			r[j++] = (char)c;
		}
	}
	r[j] = 0;
	/* Trim trailing spaces. */
	while (j > 0 && r[j-1] == ' ') { j--; r[j] = 0; }
	/* Trim leading spaces. */
	{ long k = 0; while (r[k] == ' ') k++; if (k > 0) memmove(r, r+k, (size_t)(j - k + 1)); }
	macsurf_debug_log_writef("submit: cleaned url='%s'", r);
	if (r[0] == 0) { MS_LOG("submit: empty after clean"); return; }
	/* fixes249 — default scheme is https://. Modern web is HTTPS-only;
	 * defaulting to http meant typed-by-name domains (google.com,
	 * apple.com, etc.) hit dead http:// endpoints and routed to
	 * about:fetcherror. Sites that only serve plain HTTP (some retro-
	 * focused servers) still work — users must type the full
	 * "http://example.com" explicitly. The HTTPS fetcher's keep-alive +
	 * dead-host blocklist keeps the upgrade cheap on hosts where TLS
	 * succeeds, and the salvage path (fixes243) renders partial
	 * responses on hosts where TLS half-works. */
	if (!strstr(r, "://")) {
		/* Mark with explicit https scheme. */
		sprintf(f, "https://%s", r);
		/* fixes249b — register for HTTP fallback so retro HTTP-only
		 * sites still work after the auto-upgrade. The HTTPS fetcher's
		 * hctx_fail consumes this mark and emits FETCH_REDIRECT to the
		 * http:// equivalent if the TLS path fails. */
		{
			extern void macsurf_auto_upgrade_mark(const char *url);
			macsurf_auto_upgrade_mark(f);
		}
	} else {
		strcpy(f, r);
	}
	macos9_window_navigate(g,f);
}

void macos9_window_back(struct gui_window *g) { if(g&&g->bw&&browser_window_history_back_available(g->bw)) { browser_window_history_back(g->bw, false); macos9_window_update_button_states(g); } }
void macos9_window_forward(struct gui_window *g) { if(g&&g->bw&&browser_window_history_forward_available(g->bw)) { browser_window_history_forward(g->bw, false); macos9_window_update_button_states(g); } }
extern int macsurf_http_skip_next_cache;
void macos9_window_reload(struct gui_window *g) { if(g&&g->bw) { macsurf_http_skip_next_cache = 1; browser_window_reload(g->bw, true); } }
void macos9_window_home(struct gui_window *g) { macos9_window_navigate(g, MACSURF_HOME_URL); }

void macos9_window_update_button_states(struct gui_window *g) {
#ifdef __MACOS9__
	if(!g) return; SetPortWindowPort(g->window);
	if(g->back_btn) { HiliteControl(g->back_btn, (short)(g->bw && browser_window_history_back_available(g->bw)?0:255)); Draw1Control(g->back_btn); }
	if(g->forward_btn) { HiliteControl(g->forward_btn, (short)(g->bw && browser_window_history_forward_available(g->bw)?0:255)); Draw1Control(g->forward_btn); }
	if(g->reload_btn) { HiliteControl(g->reload_btn, (short)(g->bw && browser_window_has_content(g->bw)?0:255)); Draw1Control(g->reload_btn); }
	if(g->home_btn) Draw1Control(g->home_btn);
#endif
}

void macos9_window_resize(struct gui_window *g) {
	Rect tr; TERec *ptr; if(!g) return; macos9_window_layout(g);
#ifdef __MACOS9__
	if(g->url_te) { compute_url_te_rect(&g->url_rect, &tr); ptr = *g->url_te; ptr->destRect = tr; ptr->viewRect = tr; TECalText(g->url_te); }
#endif
	macos9_window_update_scrollbars(g); g->needs_reformat=1; macos9_window_invalidate_all(g);
}

void macos9_windows_te_idle(void) {
#ifdef __MACOS9__
	struct gui_window *g; GrafPtr op; GetPort(&op);
	for(g=window_list; g; g=g->next) { if(g->url_field_active && g->url_te) { SetPortWindowPort(g->window); TEIdle(g->url_te); } }
	SetPort(op);
#endif
}

void macos9_windows_process_deferred(void) {
	struct gui_window *g; for(g=window_list;g;g=g->next) if(g->needs_reformat && g->bw && !g->reformat_in_progress) {
		g->reformat_in_progress=1; g->needs_reformat=0; browser_window_schedule_reformat(g->bw); g->reformat_in_progress=0;
	}
}

struct gui_window *initial_win = NULL;

struct gui_window *macos9_create_initial_window(void) {
	struct browser_window *bw = NULL;
	nserror e;
	MS_LOG("create_initial: calling browser_window_create");
	e = browser_window_create(BW_CREATE_HISTORY, NULL, NULL, NULL, &bw);
	if (e != NSERROR_OK) { MS_LOG("create_initial: browser_window_create FAIL"); return NULL; }
	MS_LOG("create_initial: bw attached");
	initial_win = window_list;
	return window_list;
}

static struct gui_window *macos9_window_create(struct browser_window *bw, struct gui_window *ex, gui_window_create_flags f) {
	struct gui_window *g=(struct gui_window *)calloc(1,sizeof(*g)); Rect b; short x; if(!g) return NULL;
	/* fixes124: open at desktop-class default size (1024x768)
	 * so real-window-width media queries naturally match the
	 * desktop branch on modern responsive sites. Clamped to
	 * the actual screen bounds with margin so a G3 iBook at
	 * 800x600 still gets a usable window without overflow.
	 * Uses Carbon-safe GetQDGlobalsScreenBits (qd globals
	 * struct is unavailable to Carbon apps). */
	{
		BitMap bm;
		Rect sb;
		short sw;
		short sh;
		short want_w;
		short want_h;
		short left;
		short top;
		short right;
		short bot;
		GetQDGlobalsScreenBits(&bm);
		sb = bm.bounds;
		sw = (short)(sb.right - sb.left);
		sh = (short)(sb.bottom - sb.top);
		want_w = 1024;
		want_h = 768;
		left = 40;
		top = 50;
		if ((short)(sw - left - 20) < want_w) {
			want_w = (short)(sw - left - 20);
		}
		if ((short)(sh - top - 30) < want_h) {
			want_h = (short)(sh - top - 30);
		}
		if (want_w < 480) want_w = 480;
		if (want_h < 360) want_h = 360;
		right = (short)(left + want_w);
		bot = (short)(top + want_h);
		SetRect(&b, left, top, right, bot);
	}
	g->bw=bw; if(CreateNewWindow(6, 0x1F, &b, &g->window)!=0) { free(g); return NULL; }
	SetWRefCon(g->window,(long)g); SetPortWindowPort(g->window); SetWTitle(g->window,(const unsigned char*)"\pMacSurf");
	g->next=window_list; window_list=g; 
	x=4; SetRect(&b,x,8,(short)(x+60),30); g->back_btn=NewControl(g->window,&b,(const unsigned char*)"\pBack",1,0,0,1,368,(long)g); x=(short)(x+64);
	SetRect(&b,x,8,(short)(x+60),30); g->forward_btn=NewControl(g->window,&b,(const unsigned char*)"\pFwd",1,0,0,1,368,(long)g); x=(short)(x+64);
	SetRect(&b,x,8,(short)(x+60),30); g->reload_btn=NewControl(g->window,&b,(const unsigned char*)"\pReload",1,0,0,1,368,(long)g); x=(short)(x+64);
	SetRect(&b,x,8,(short)(x+60),30); g->home_btn=NewControl(g->window,&b,(const unsigned char*)"\pHome",1,0,0,1,368,(long)g);
	SetRect(&b,0,0,16,16); g->vscroll=NewControl(g->window,&b,(const unsigned char*)"\p",1,0,0,0,384,(long)g);
	g->hscroll=NewControl(g->window,&b,(const unsigned char*)"\p",1,0,0,0,384,(long)g);
	macos9_window_layout(g);
#ifdef __MACOS9__
	/* Show + select the window FIRST so subsequent Toolbox calls
	 * run in a fully realized port/state. The previous order
	 * (create TE before show) is the suspected cause of the
	 * "URL field unresponsive on initial window" regression. */
	ShowWindow(g->window); SelectWindow(g->window);
	SetPortWindowPort(g->window);
	compute_url_te_rect(&g->url_rect,&b); g->url_te=TENew(&b,&b);
	if(g->url_te) {
		TESetText(MACSURF_HOME_URL,(long)strlen(MACSURF_HOME_URL),g->url_te);
		TECalText(g->url_te);
		TEActivate(g->url_te);
		TESetSelect(0, 32767, g->url_te);  /* select-all so first keystroke replaces */
	}
	GetWindowPortBounds(g->window,&b); b.right=(short)(b.right-b.left); b.bottom=(short)(b.bottom-b.top); b.left=0; b.top=0;
	InvalWindowRect(g->window,&b);
#endif
	g->url_field_active=1; macos9_window_update_scrollbars(g); macos9_window_update_button_states(g);
	return g;
}

void macos9_window_destroy(struct gui_window *g) { struct gui_window **p; for(p=&window_list;*p;p=&(*p)->next) if(*p==g) { *p=g->next; break; }
#ifdef __MACOS9__
	if(g->content_gworld) { DisposeGWorld(g->content_gworld); g->content_gworld = NULL; }
#endif
	if(g->window) DisposeWindow(g->window); free(g); }
/* fixes100 — honour NetSurf's per-update dirty rect.
 *
 * Pre-fixes100 this function logged the supplied rect and then
 * unconditionally InvalWindowRect'd the entire content area. That
 * meant every keystroke (caret blink, textarea text change), every
 * scroll-bar value update, every late image arrival, every layout
 * tweak triggered a full content viewport repaint — on the duck-duck
 * search results page that's ~170 DrawText calls and ~600 box-tree
 * visits per character typed, ~200-300ms per keystroke on G3/G4.
 *
 * NetSurf passes r in document/layout coords (pre-scroll). Convert
 * to window coords: window = layout + content_rect.{top,left} -
 * scroll_{y,x}. Then clip to content_rect so we never invalidate
 * outside the viewport (a stray invalidate over the URL bar would
 * trigger TextEdit overdraw bugs). Null r still means full content,
 * matching the prior contract for callers that ask for a full
 * repaint (NEW_CONTENT, reformat, etc). */
static nserror macos9_gw_invalidate(struct gui_window *g, const struct rect *r) {
	if(!g||!g->window) return 0;
	if (r != NULL) {
		Rect ir;
		int wx0, wy0, wx1, wy1;
		macsurf_debug_log_writef("gw_invalidate: r=(%d,%d,%d,%d)",
			r->x0, r->y0, r->x1, r->y1);
		wx0 = r->x0 + g->content_rect.left - g->scroll_x;
		wy0 = r->y0 + g->content_rect.top  - g->scroll_y;
		wx1 = r->x1 + g->content_rect.left - g->scroll_x;
		wy1 = r->y1 + g->content_rect.top  - g->scroll_y;
		if (wx0 < g->content_rect.left)   wx0 = g->content_rect.left;
		if (wy0 < g->content_rect.top)    wy0 = g->content_rect.top;
		if (wx1 > g->content_rect.right)  wx1 = g->content_rect.right;
		if (wy1 > g->content_rect.bottom) wy1 = g->content_rect.bottom;
		if (wx1 <= wx0 || wy1 <= wy0) return 0;
		ir.left   = (short)wx0;
		ir.top    = (short)wy0;
		ir.right  = (short)wx1;
		ir.bottom = (short)wy1;
		InvalWindowRect(g->window, &ir);
	} else {
		MS_LOG("gw_invalidate: r=NULL (full)");
		InvalWindowRect(g->window, &g->content_rect);
	}
	return 0;
}
static bool macos9_gw_get_scroll(struct gui_window *g, int *x, int *y) { if(x) *x=g->scroll_x; if(y) *y=g->scroll_y; return 1; }
static nserror macos9_gw_set_scroll(struct gui_window *g, const struct rect *r) { if(r) macos9_window_scroll_to(g,r->x0,r->y0); return 0; }
static nserror macos9_gw_get_dimensions(struct gui_window *g, int *w, int *h) { if(!g) return 0; if(w) *w=g->content_rect.right-g->content_rect.left; if(h) *h=g->content_rect.bottom-g->content_rect.top; return 0; }
static nserror macos9_gw_event(struct gui_window *g, enum gui_window_event e) {
	struct hlcache_handle *cur = (g && g->bw) ? browser_window_get_content(g->bw) : NULL;
	macsurf_debug_log_writef("gw_event: e=%d current_content=%p", (int)e, cur);
	if(e==GW_EVENT_UPDATE_EXTENT||e==GW_EVENT_NEW_CONTENT||e==GW_EVENT_STOP_THROBBER) {
		int w=0, h=0; if(g->bw && browser_window_get_extents(g->bw, false, &w, &h)==NSERROR_OK) { g->content_width=w; g->content_height=h; }
		if(e==GW_EVENT_NEW_CONTENT) {
			g->scroll_x=0; g->scroll_y=0;
			if(g->bw) {
				browser_window_schedule_reformat(g->bw);
				MS_LOG("gw_event: NEW_CONTENT -> schedule_reformat");
			}
		}
		macsurf_debug_log_writef("gw_event: invalidate w=%d h=%d scroll=(%d,%d)", g->content_width, g->content_height, g->scroll_x, g->scroll_y);
		macos9_window_update_scrollbars(g); macos9_window_update_button_states(g); macos9_window_invalidate_all(g);
	}
	return 0;
}
static void macos9_gw_set_title(struct gui_window *g, const char *t) {
	Str255 p;
	size_t in_l;
	size_t out_l;
	char mac_buf[256];
	if (!g || !g->window || !t) return;
	in_l = strlen(t);
	/* fixes219 — NetSurf hands us UTF-8; SetWTitle wants MacRoman.
	 * Without the conversion en-dashes, smart quotes, etc. land as
	 * mojibake in the title bar (",A," instead of "–"). */
	out_l = macos9_utf8_to_macroman(t, in_l, mac_buf, sizeof mac_buf);
	if (out_l > 255) out_l = 255;
	p[0] = (unsigned char)out_l;
	memcpy(p + 1, mac_buf, out_l);
	SetWTitle(g->window, p);
}
static nserror macos9_gw_set_url(struct gui_window *g, struct nsurl *u) { const char *s; if(g&&u&&g->url_te&&(s=nsurl_access(u))) set_url_te_text(g,s); return 0; }
static void macos9_gw_set_status(struct gui_window *g, const char *t) {
	/* fixes109 — dedupe. NetSurf core fires set_status on every fetch
	 * progress callback, every hover, every link-tracking transition.
	 * Many of those fire with the same string repeatedly (e.g. "Loading
	 * https://X..." while bytes accumulate). Unconditional InvalRect on
	 * status_rect was painting the status bar dozens of times per
	 * second on a loading page, costing CPU and showing as the pulsing
	 * status-bar redraw loop in the fixes107 log. Skip the InvalRect
	 * unless the visible text actually changed. */
	if (g == NULL || t == NULL) return;
	if (strcmp(g->status, t) == 0) return;
	strncpy(g->status, t, 127);
	g->status[127] = 0;
	if (g->window) InvalWindowRect(g->window, &g->status_rect);
}

static void macos9_gw_set_pointer(struct gui_window *g, enum gui_pointer_shape shape)
{
#ifdef __MACOS9__
	(void)g;
	switch (shape) {
	case GUI_POINTER_POINT:
		SetThemeCursor(kThemePointingHandCursor);
		break;
	case GUI_POINTER_CARET:
		SetThemeCursor(kThemeIBeamCursor);
		break;
	case GUI_POINTER_WAIT:
	case GUI_POINTER_PROGRESS:
		SetThemeCursor(kThemeWatchCursor);
		break;
	case GUI_POINTER_CROSS:
		SetThemeCursor(kThemeCrossCursor);
		break;
	case GUI_POINTER_MOVE:
		SetThemeCursor(kThemeOpenHandCursor);
		break;
	/* fixes278 (#79): extended cursor mapping. Limited to constants
	 * known to exist in early CarbonLib (1.x baseline on Mac OS 9.1).
	 * kThemeContextualMenuArrowCursor and kThemeResizeLeftRightCursor
	 * are present from CarbonLib 1.0. kThemeNotAllowedCursor was added
	 * in CarbonLib 1.5; we approximate with kThemeArrowCursor for now
	 * rather than risk a link error on the 9.1 G3 baseline. */
	case GUI_POINTER_HELP:
	case GUI_POINTER_NOT_ALLOWED:
	case GUI_POINTER_NO_DROP:
		SetThemeCursor(kThemeArrowCursor);
		break;
	case GUI_POINTER_MENU:
		SetThemeCursor(kThemeContextualMenuArrowCursor);
		break;
	case GUI_POINTER_UP:
	case GUI_POINTER_DOWN:
	case GUI_POINTER_LEFT:
	case GUI_POINTER_RIGHT:
		SetThemeCursor(kThemeResizeLeftRightCursor);
		break;
	default:
		SetThemeCursor(kThemeArrowCursor);
		break;
	}
#else
	(void)g; (void)shape;
#endif
}

/* fixes294 — load the baked-in 16.png once into a GWorld kept alive for
 * the lifetime of the process.  Called from main.c's startup path.
 * Idempotent: if called twice, the second call no-ops.  On any failure
 * the loaded flag stays 0 and the paint helper bails. */
void macos9_window_load_default_favicon(void)
{
#ifdef __MACOS9__
	unsigned char *rgba = NULL;
	unsigned w = 0, h = 0;
	unsigned err;
	OSErr oerr;
	GWorldPtr saved_port;
	GDHandle saved_gdh;
	PixMapHandle pm;
	long dst_rowbytes;
	long row, col;
	unsigned char *src_row, *dst_row;

	if (macos9_default_favicon_loaded) return;

	err = lodepng_decode32(&rgba, &w, &h,
		macos9_default_favicon_png,
		macos9_default_favicon_png_len);
	if (err != 0 || rgba == NULL || w == 0 || h == 0) {
		macsurf_debug_log_writef("favicon: lodepng err=%u w=%u h=%u",
			err, w, h);
		if (rgba != NULL) free(rgba);
		return;
	}

	SetRect(&macos9_default_favicon_src_rect, 0, 0, (short)w, (short)h);

	GetGWorld(&saved_port, &saved_gdh);
	oerr = NewGWorld(&macos9_default_favicon_gworld, 32,
		&macos9_default_favicon_src_rect, NULL, NULL, 0);
	if (oerr != noErr || macos9_default_favicon_gworld == NULL) {
		macsurf_debug_log_writef("favicon: NewGWorld err=%d", (int)oerr);
		free(rgba);
		SetGWorld(saved_port, saved_gdh);
		return;
	}
	pm = GetGWorldPixMap(macos9_default_favicon_gworld);
	if (pm == NULL || !LockPixels(pm)) {
		DisposeGWorld(macos9_default_favicon_gworld);
		macos9_default_favicon_gworld = NULL;
		free(rgba);
		SetGWorld(saved_port, saved_gdh);
		return;
	}
	dst_rowbytes = (long)((*pm)->rowBytes & 0x3FFF);
	for (row = 0; row < (long)h; row++) {
		src_row = rgba + row * (long)w * 4L;
		dst_row = (unsigned char *)GetPixBaseAddr(pm) + row * dst_rowbytes;
		for (col = 0; col < (long)w; col++) {
			unsigned char r = src_row[col * 4 + 0];
			unsigned char gn = src_row[col * 4 + 1];
			unsigned char b = src_row[col * 4 + 2];
			/* XRGB, alpha threshold deferred to optional mask path */
			dst_row[col * 4 + 0] = 0xFF;
			dst_row[col * 4 + 1] = r;
			dst_row[col * 4 + 2] = gn;
			dst_row[col * 4 + 3] = b;
		}
	}
	UnlockPixels(pm);
	SetGWorld(saved_port, saved_gdh);
	free(rgba);

	macos9_default_favicon_loaded = 1;
	macsurf_debug_log_writef("favicon: loaded w=%u h=%u gworld=%p",
		w, h, (void *)macos9_default_favicon_gworld);
#endif
}

/* fixes294 — paint the cached favicon GWorld inside url_rect, called
 * from draw_url_bar AFTER TEUpdate so the icon is on top of the white
 * field background.  No allocation, no LockPixels — single CopyBits.
 * Full GetGWorld/SetGWorld bracket. */
void macos9_window_draw_favicon(struct gui_window *g)
{
#ifdef __MACOS9__
	extern const BitMap *GetPortBitMapForCopyBits(CGrafPtr port);
	GWorldPtr saved_port;
	GDHandle saved_gdh;
	const BitMap *src_bm;
	const BitMap *dst_bm;
	CGrafPtr win_port;
	GWorldPtr src_gworld;
	Rect *src_rect_ptr;
	Rect dst_rect;

	if (g == NULL || g->window == NULL) return;

	/* fixes295 Phase 1b — prefer active per-site favicon over default. */
	if (macos9_active_favicon_gworld != NULL) {
		src_gworld = macos9_active_favicon_gworld;
		src_rect_ptr = &macos9_active_favicon_src_rect;
	} else if (macos9_default_favicon_loaded &&
		   macos9_default_favicon_gworld != NULL) {
		src_gworld = macos9_default_favicon_gworld;
		src_rect_ptr = &macos9_default_favicon_src_rect;
	} else {
		return;
	}

	compute_favicon_rect(&g->url_rect, &dst_rect);

	GetGWorld(&saved_port, &saved_gdh);
	SetPortWindowPort(g->window);
	win_port = GetWindowPort(g->window);
	if (win_port == NULL) { SetGWorld(saved_port, saved_gdh); return; }

	dst_bm = GetPortBitMapForCopyBits(win_port);
	src_bm = GetPortBitMapForCopyBits((CGrafPtr)src_gworld);
	if (src_bm == NULL || dst_bm == NULL) {
		SetGWorld(saved_port, saved_gdh);
		return;
	}

	CopyBits(src_bm, dst_bm, src_rect_ptr, &dst_rect, srcCopy, NULL);

	SetGWorld(saved_port, saved_gdh);
#else
	(void)g;
#endif
}

/* fixes295 Phase 1b — release the active per-site favicon GWorld and
 * revert to the default.  Called on set_icon(NULL), navigate-away, and
 * window destroy. */
static void active_favicon_release(void)
{
#ifdef __MACOS9__
	if (macos9_active_favicon_gworld != NULL) {
		DisposeGWorld(macos9_active_favicon_gworld);
		macos9_active_favicon_gworld = NULL;
	}
	SetRect(&macos9_active_favicon_src_rect, 0, 0, 0, 0);
#endif
}

/* fixes295 Phase 1b — pull the favicon bitmap from the hlcache_handle,
 * bake it into a fresh GWorld at the bitmap's natural size, swap into
 * macos9_active_favicon_gworld replacing any previous per-site icon. */
static int active_favicon_build(struct hlcache_handle *icon)
{
#ifdef __MACOS9__
	extern struct bitmap *content_get_bitmap(struct hlcache_handle *h);
	extern unsigned char *macos9_bitmap_get_buffer(void *bitmap);
	extern int macos9_bitmap_get_width(void *bitmap);
	extern int macos9_bitmap_get_height(void *bitmap);
	extern size_t macos9_bitmap_get_rowstride(void *bitmap);

	struct bitmap *bm;
	unsigned char *buf;
	int bw, bh;
	long rowstride;
	GWorldPtr new_gw = NULL;
	OSErr oerr;
	GWorldPtr saved_port;
	GDHandle saved_gdh;
	PixMapHandle pm;
	long dst_rowbytes;
	long row, col;
	unsigned char *src_row, *dst_row;
	Rect new_src_rect;

	if (icon == NULL) return 0;
	bm = content_get_bitmap(icon);
	if (bm == NULL) {
		MS_LOG("active_favicon_build: content_get_bitmap=NULL");
		return 0;
	}
	buf = macos9_bitmap_get_buffer((void *)bm);
	bw = macos9_bitmap_get_width((void *)bm);
	bh = macos9_bitmap_get_height((void *)bm);
	rowstride = (long)macos9_bitmap_get_rowstride((void *)bm);
	if (buf == NULL || bw <= 0 || bh <= 0) return 0;
	if (bw > 256 || bh > 256) return 0;

	SetRect(&new_src_rect, 0, 0, (short)bw, (short)bh);

	GetGWorld(&saved_port, &saved_gdh);
	oerr = NewGWorld(&new_gw, 32, &new_src_rect, NULL, NULL, 0);
	if (oerr != noErr || new_gw == NULL) {
		SetGWorld(saved_port, saved_gdh);
		return 0;
	}
	pm = GetGWorldPixMap(new_gw);
	if (pm == NULL || !LockPixels(pm)) {
		DisposeGWorld(new_gw);
		SetGWorld(saved_port, saved_gdh);
		return 0;
	}
	dst_rowbytes = (long)((*pm)->rowBytes & 0x3FFF);
	for (row = 0; row < bh; row++) {
		src_row = buf + row * rowstride;
		dst_row = (unsigned char *)GetPixBaseAddr(pm) + row * dst_rowbytes;
		for (col = 0; col < bw; col++) {
			unsigned char r = src_row[col * 4 + 0];
			unsigned char gn = src_row[col * 4 + 1];
			unsigned char b = src_row[col * 4 + 2];
			dst_row[col * 4 + 0] = 0xFF;
			dst_row[col * 4 + 1] = r;
			dst_row[col * 4 + 2] = gn;
			dst_row[col * 4 + 3] = b;
		}
	}
	UnlockPixels(pm);
	SetGWorld(saved_port, saved_gdh);

	/* Swap atomically: dispose old active (if any), install new. */
	active_favicon_release();
	macos9_active_favicon_gworld = new_gw;
	macos9_active_favicon_src_rect = new_src_rect;
	macsurf_debug_log_writef(
		"active_favicon_build: OK %dx%d gworld=%p",
		bw, bh, (void *)new_gw);
	return 1;
#else
	(void)icon;
	return 0;
#endif
}

/* fixes295 Phase 1b — NetSurf set_icon callback. */
static void macos9_gw_set_icon(struct gui_window *g, struct hlcache_handle *icon)
{
#ifdef __MACOS9__
	if (g == NULL || g->window == NULL) return;
	macsurf_debug_log_writef("set_icon: icon=%p", (void *)icon);
	if (icon == NULL) {
		active_favicon_release();
	} else if (!active_favicon_build(icon)) {
		/* Build failed (bitmap not yet decoded, non-PNG, etc.) — keep
		 * whatever active we had; will fall back to default if none. */
	}
	InvalWindowRect(g->window, &g->url_rect);
#else
	(void)g; (void)icon;
#endif
}

static struct gui_window_table wt = {
	macos9_window_create, macos9_window_destroy, macos9_gw_invalidate, macos9_gw_get_scroll,
	macos9_gw_set_scroll, macos9_gw_get_dimensions, macos9_gw_event, macos9_gw_set_title,
	macos9_gw_set_url, macos9_gw_set_icon, macos9_gw_set_status, macos9_gw_set_pointer, (void*)0, (void*)0, (void*)0, (void*)0, (void*)0, (void*)0, (void*)0, (void*)0
};
struct gui_window_table *macos9_window_table = &wt;
