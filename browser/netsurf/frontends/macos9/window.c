#include <stdlib.h>
#include <string.h>
#include "utils/ns_errors.h"
#include "utils/log.h"
#include "utils/nsurl.h"
#include "netsurf/types.h"
#include "netsurf/window.h"
#include "netsurf/browser_window.h"
#include "utils/nsoption.h"    /* preferences: window_width/height */
#include "macos9.h"
#include "macsurf_config.h"
#ifdef __MACOS9__
#include <Scrap.h>	/* fixes376 - desk-scrap I/O for URL-field cut/copy/paste */
#endif
#include "macsurf_debug.h"
#include "macsurf_memory.h"    /* macsurf_recon_mem() */
#include "macsurf_osver.h"     /* macsurf_os_is_osx() -- no call sites here as
                                * of fixes939 (the OS X TE bisect was reverted
                                * once both suspects were cleared); kept because
                                * gating the URL field is the tier-1e fallback
                                * if the antialiasing gate does not fix it. */

#ifdef __MACOS9__
#include <MacWindows.h>
#include <Controls.h>
#include <Appearance.h>
#include <Quickdraw.h>
#include <TextEdit.h>
#endif

static struct gui_window *window_list = NULL;
static struct gui_window *macos9_window_create(struct browser_window *bw, struct gui_window *ex, gui_window_create_flags f);

/* fixes294 - Phase 0 favicon plumbing.
 *
 * Lessons from the failed fixes292/293 attempts:
 *   - Inserting a Rect field in the middle of struct gui_window caused
 *     CW8 missed-recompile corruption (other .c files reading
 *     content_rect / status_rect at stale offsets).  This attempt
 *     keeps ALL state in file-scope statics - no struct changes at all.
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

/* fixes297 - toolbar button icons.  Four 16x16-ish PNGs baked as static
 * arrays and decoded once at startup into a permanent app-heap GWorld
 * each.  Painted over the existing platinum Carbon buttons after
 * DrawControls so the click target stays the real ControlRef and we
 * just add an icon on top of the platinum background.  Skip painting
 * when the icon GWorld failed to load (graceful fallback to text-only
 * button). */
#include "toolbar_icons_data.h"
#include "loader_frames_data.h"   /* fixes726 - animated loading spinner frames */
static GWorldPtr macos9_btn_back_gworld = NULL;
static GWorldPtr macos9_btn_forward_gworld = NULL;
static GWorldPtr macos9_btn_refresh_gworld = NULL;
static GWorldPtr macos9_btn_home_gworld = NULL;
static Rect macos9_btn_back_src_rect;
static Rect macos9_btn_forward_src_rect;
static Rect macos9_btn_refresh_src_rect;
static Rect macos9_btn_home_src_rect;
/* fixes724 - Stop (X). Coloured when the page is loading (stoppable), the
 * grey variant when idle (non-active). */
static GWorldPtr macos9_btn_stop_gworld = NULL;
static Rect      macos9_btn_stop_src_rect;
static GWorldPtr macos9_btn_stop_g_gworld = NULL;
static Rect      macos9_btn_stop_g_src_rect;
/* fixes297d - disabled-state variants for back / forward (only ones with
 * an "unavailable" state at the top of history).  refresh + home are
 * always available so don't need greyed variants. */
static GWorldPtr macos9_btn_back_g_gworld = NULL;
static GWorldPtr macos9_btn_forward_g_gworld = NULL;
static Rect macos9_btn_back_g_src_rect;
static Rect macos9_btn_forward_g_src_rect;
/* fixes297e - refresh's "alternate" state for loading animation.  Not a
 * disabled-state variant (refresh is always actionable); used to toggle
 * the reload icon during page loads. */
static GWorldPtr macos9_btn_refresh_g_gworld = NULL;
static Rect macos9_btn_refresh_g_src_rect;
static int macos9_reload_animating = 0;
static int macos9_reload_frame = 0;
/* fixes726 - decoded loader-animation frames (shared across windows, like the
 * toolbar icons). Populated in macos9_window_load_toolbar_icons; NULL entries
 * are simply skipped so a partial decode degrades gracefully. */
#ifdef __MACOS9__
static GWorldPtr macos9_loader_gworld[MACOS9_LOADER_FRAMES];
static Rect      macos9_loader_src_rect[MACOS9_LOADER_FRAMES];
static int       macos9_loader_frames_loaded = 0;
#endif
/* fixes297f - home dim variant when current page == home URL. */
static GWorldPtr macos9_btn_home_g_gworld = NULL;
static Rect macos9_btn_home_g_src_rect;
static int  macos9_toolbar_icons_loaded = 0;
/* fixes725 - nav button the pointer is currently over (or NULL), for the
 * hover-highlight frame. Set by macos9_window_update_hover. */
static ControlRef macos9_hovered_btn = NULL;

/* fixes295 Phase 1b - active per-site favicon GWorld.  When NetSurf
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

/* fixes612 - expose the front window's content viewport (device px) so the
 * core html_get_dimensions() media-query path has a real width even when the
 * CONTENT_MSG_GETDIMS broadcast returns nothing (content not yet bound to a
 * browser_window at CSS-select time). Without it media.width is 0, every
 * width media query collapses to the mobile branch (max-width:900 always
 * true), and desktop two-column layouts never apply. */
void macos9_frontend_viewport(int *w, int *h)
{
	struct gui_window *g = window_list;
	int vw = 0, vh = 0;
	if (g != NULL) {
		vw = g->content_rect.right - g->content_rect.left;
		vh = g->content_rect.bottom - g->content_rect.top;
	}
	if (w != NULL) *w = vw;
	if (h != NULL) *h = vh;
}
/* fixes320j - accessor so the JS bridge can reach bw to schedule
 * a reformat after JS DOM mutation. */
struct browser_window *macos9_gw_bw(struct gui_window *g) { return g ? g->bw : NULL; }
static void set_status_text(struct gui_window *g, const char *m) { if(!m) g->status[0]=0; else { strncpy(g->status,m,127); g->status[127]=0; } }
/*
 * fixes944 (OS X tier 2) - a GDevice that is actually usable for colour.
 *
 * Root cause, finally measured rather than guessed. On Mac OS X 10.3 the
 * SCREEN device returned by GetMainDevice() has no usable colour/inverse
 * table: dereferencing it yields 0x74140001, which is unmapped. Every
 * RGBForeColor resolves its colour index through the CURRENT GDevice, so any
 * colour call made while the screen device is current dies inside
 * InternalColor2Index. That is the whole OS X crash -- it looked like a
 * TextEdit bug only because TESetText happened to be the first caller.
 *
 * The evidence chain:
 *   fixes942  BOOT te: maindev=0008109C   <- and 0x0008109C is EXACTLY r4 in
 *                                            every crash dump (938/939/941/942)
 *   fixes943  probe gwdev=002FF378 ... RGBForeColor on GWorld SURVIVED
 *
 * So classic QuickDraw colour is NOT broken here -- only Quartz's stand-in for
 * the screen GDevice is. A GWorld we allocate ourselves gets a real GDevice
 * (in the app heap, 0x002F.., beside our other allocations) and colour works
 * against it perfectly.
 *
 * SetGWorld() takes port and device INDEPENDENTLY, so we can keep drawing into
 * the window's port while resolving colour through a device that is not
 * poisoned. This owns one tiny 32-bit GWorld for the life of the process,
 * purely to have a valid GDevice to point at.
 *
 * On Mac OS 9 this returns GetMainDevice() and nothing changes.
 */
static GWorldPtr g_safe_gdev_gw = NULL;   /* owns the device; never drawn into */
static GDHandle  g_safe_gdev    = NULL;   /* cached; NULL until first use      */

GDHandle macos9_safe_gdevice(void)
{
#ifdef __MACOS9__
	if (!macsurf_os_is_osx()) return GetMainDevice();

	if (g_safe_gdev == NULL) {
		CGrafPtr save_port;
		GDHandle save_dev;
		Rect r;
		OSErr e;

		GetGWorld(&save_port, &save_dev);
		SetRect(&r, 0, 0, 8, 8);
		e = NewGWorld(&g_safe_gdev_gw, 32, &r, NULL, NULL, 0);
		if (e == noErr && g_safe_gdev_gw != NULL) {
			g_safe_gdev = GetGWorldDevice(g_safe_gdev_gw);
		}
		SetGWorld(save_port, save_dev);
		macsurf_debug_log_writef("RECON GDEV safe=%p main=%p err=%d",
			(void *)g_safe_gdev, (void *)GetMainDevice(), (int)e);
		macsurf_debug_log_flush();
	}
	/* Fail open to the screen device: on OS 9 that is simply correct, and on
	 * OS X a failed NewGWorld leaves us no better option than before. */
	return (g_safe_gdev != NULL) ? g_safe_gdev : GetMainDevice();
#else
	return NULL;
#endif
}

/* fixes294 - shift TE field's left by +20 to leave room for the favicon. */
/* fixes302 - TextEdit rect inside the url field.
 *   left  = 2px bevel + 16px favicon + 6px gap  -> text never touches the
 *           left bevel and clears the favicon.
 *   right = clear the 2px right bevel + a few px margin.
 *   top/bottom vertically centre a ~16px (Geneva 12) line in the field. */
static void compute_url_te_rect(const Rect *u, Rect *o) {
	short h = (short)(u->bottom - u->top);
	short pad = (short)((h - 16) / 2);
	if (pad < 3) pad = 3;
	o->left   = (short)(u->left + 24);
	o->right  = (short)(u->right - 6);
	o->top    = (short)(u->top + pad);
	o->bottom = (short)(u->bottom - pad);
}

/* fixes300 - solid Mac OS 9 Platinum grey (#D6D6D6) toolbar background.
 * Replaces the fixes298 vertical gradient.  Buttons sit directly on this
 * surface with no white "tab" backing - the icon's own colours provide
 * contrast. */
/* fixes727 - soft vertical toolbar gradient. The old flat #D6D6D6 platinum
 * read as "stock"; a gentle top-lighter/bottom-darker sheen plus a bright top
 * highlight and a bottom shadow bevel make the toolbar read as a raised,
 * designed surface. The gradient centre lands on #D6 (0xD6) at the button band
 * so the icons (matted to #D6) still blend seamlessly. */
#define MACOS9_TOOLBAR_H   48
#define MACOS9_TB_TOP_GREY 0xDC   /* 220 - lit top */
#define MACOS9_TB_BOT_GREY 0xD0   /* 208 - shaded bottom */

#ifdef __MACOS9__
static unsigned short macos9_tb_grey_at(short y)
{
	long v;
	if (y < 0) y = 0;
	if (y >= MACOS9_TOOLBAR_H) y = (short)(MACOS9_TOOLBAR_H - 1);
	v = MACOS9_TB_TOP_GREY +
	    (long)(MACOS9_TB_BOT_GREY - MACOS9_TB_TOP_GREY) * y / MACOS9_TOOLBAR_H;
	return (unsigned short)((v << 8) | v);   /* 8-bit -> 16-bit component */
}

/* Fill a rect with the toolbar gradient (one PaintRect per scanline). Used for
 * the whole bar and for each icon-button slot so nothing shows a flat patch. */
static void macos9_tb_fill_gradient(const Rect *r)
{
	short y;
	RGBColor c;
	Rect ln;
	ln.left = r->left; ln.right = r->right;
	for (y = r->top; y < r->bottom; y++) {
		unsigned short g16 = macos9_tb_grey_at(y);
		c.red = g16; c.green = g16; c.blue = g16;
		RGBForeColor(&c);
		ln.top = y; ln.bottom = (short)(y + 1);
		PaintRect(&ln);
	}
}
#endif

void macos9_window_draw_toolbar_bg(struct gui_window *g)
{
#ifdef __MACOS9__
	Rect w_bounds;
	Rect bar;
	RGBColor saved_fg;
	GWorldPtr saved_port;
	GDHandle saved_gdh;

	if (g == NULL || g->window == NULL) return;

	GetGWorld(&saved_port, &saved_gdh);
	SetPortWindowPort(g->window);
	GetForeColor(&saved_fg);

	GetWindowBounds(g->window, 33, &w_bounds);
	bar.left = 0;
	bar.top = 0;
	bar.right = (short)(w_bounds.right - w_bounds.left);
	bar.bottom = (short)(g->content_rect.top);
	macos9_tb_fill_gradient(&bar);

	/* fixes727 - depth bevel: a bright highlight along the very top edge and
	 * a soft shadow just above the crisp separator at the bottom. */
	{
		RGBColor hi  = {0xF0F0, 0xF0F0, 0xF0F0};   /* top highlight */
		RGBColor sh  = {0xBEBE, 0xBEBE, 0xBEBE};   /* bottom shadow */
		RGBColor sep = {0x8888, 0x8888, 0x8888};   /* separator */
		Rect line;
		line.left = 0; line.right = bar.right;
		/* top highlight */
		line.top = 0; line.bottom = 1;
		RGBForeColor(&hi); PaintRect(&line);
		/* bottom shadow + separator */
		line.top = (short)(bar.bottom - 2); line.bottom = (short)(bar.bottom - 1);
		RGBForeColor(&sh); PaintRect(&line);
		line.top = (short)(bar.bottom - 1); line.bottom = bar.bottom;
		RGBForeColor(&sep); PaintRect(&line);
	}

	RGBForeColor(&saved_fg);
	SetGWorld(saved_port, saved_gdh);
#else
	(void)g;
#endif
}

/* fixes294 - return the favicon's paint rect inside a given url_rect. */
static void compute_favicon_rect(const Rect *u, Rect *o)
{
	short top;
	top = (short)(u->top + ((u->bottom - u->top) - 16) / 2);
	o->left = (short)(u->left + 4);
	o->right = (short)(u->left + 20);
	o->top = top;
	o->bottom = (short)(top + 16);
}

void macos9_window_layout(struct gui_window *g) {
	Rect c; short w, h, ux, ur, cb, ht; if(!g||!g->window) return;
	GetWindowBounds(g->window, 33, &c); w=(short)(c.right-c.left); h=(short)(c.bottom-c.top);
	/* fixes303/fixes723/fixes724 - dense "tool belt", 5 buttons (Back,
	 * Forward, Stop, Refresh, Home). 36 wide at a 38-pixel pitch (2px gap),
	 * so x=4,42,80,118,156 and the home button's right edge is 192. The URL
	 * field sits 2px past it (x=194) aligned to the button band (y=6..42).
	 * The 1px separator at y=47 (drawn by macos9_window_draw_toolbar_bg from
	 * content_rect.top-1) closes the 48px toolbar. */
	ux=(short)(4 + 4*38 + 36 + 2);
	/* fixes726/fixes727 - reserve a 40px Netscape-style throbber slot on the
	 * far right of the toolbar (8px gap from the URL field). Bigger than the
	 * 36px buttons so it reads clearly; spans y=4..44 within the 48px bar. */
	ur=(short)(w - 4 - MACOS9_LOADER_SIZE - 8);
	if (ur < ux + 40) ur = (short)(ux + 40);   /* keep URL field usable on tiny windows */
	SetRect(&g->url_rect, ux, 6, ur, 42);
	SetRect(&g->loader_rect, (short)(w - 4 - MACOS9_LOADER_SIZE), 4,
		(short)(w - 4), (short)(4 + MACOS9_LOADER_SIZE));
	ht=(short)(h-15); cb=(short)(ht-16); SetRect(&g->content_rect, 0, 48, (short)(w-15), cb); SetRect(&g->status_rect, 0, cb, (short)(w-15), ht);
	if(g->vscroll) { MoveControl(g->vscroll, (short)(w-15), 47); SizeControl(g->vscroll, 16, (short)(cb-46)); }
	if(g->hscroll) { MoveControl(g->hscroll, -1, ht); SizeControl(g->hscroll, (short)(w-13), 16); }
}

void macos9_window_invalidate_all(struct gui_window *g) { Rect r; if(!g||!g->window)return; GetWindowBounds(g->window, 33, &r); r.right=(short)(r.right-r.left); r.bottom=(short)(r.bottom-r.top); r.left=0; r.top=0; InvalWindowRect(g->window, &r); }
void macos9_window_invalidate_content(struct gui_window *g) { if(!g||!g->window)return; InvalWindowRect(g->window, &g->content_rect); }
/* fixes630: request a re-layout on the next null-event pass. Used when an
 * async webfont file arrives so the MEASURE path re-runs and sizes the icon
 * boxes with the now-known glyph advances. The main loop coalesces it. */
void macos9_window_request_reformat(struct gui_window *g) { if(g) g->needs_reformat = 1; }

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

void macos9_window_scroll_to(struct gui_window *g, int nx, int ny) {
	int vw, vh, mx, my;
	if(!g) return;
	/* fixes643 (#195): CLAMP the scroll target to [0, max] at this single
	 * choke point. Every scroll path routes through here (arrow keys via
	 * scroll_by, scroll-bar drag, core set_scroll, End=0x7FFFFFFF). Without
	 * the clamp, a left/up arrow at the edge drove scroll NEGATIVE, and the
	 * paint origin (content_rect.left - scroll_x) then shoved the ENTIRE
	 * page off-canvas sideways/down - the reported "viewport pushed to a
	 * direction" bug. Same max math as macos9_window_update_scrollbars. */
	vw = g->content_rect.right - g->content_rect.left;
	vh = g->content_rect.bottom - g->content_rect.top;
	mx = g->content_width - vw; my = g->content_height - vh;
	if(mx < 0) mx = 0; if(my < 0) my = 0;
	if(nx < 0) nx = 0; if(nx > mx) nx = mx;
	if(ny < 0) ny = 0; if(ny > my) ny = my;
	{
		/* fixes1013 - fire `scroll` at the page, but only on a real
		 * CHANGE. This is the single choke point every scroll path routes
		 * through (arrow keys, scroll-bar drag, core set_scroll, End,
		 * window.scrollTo), which makes it the one correct place -- and
		 * also means it is called with unchanged values often enough that
		 * dispatching unconditionally would fire a burst of no-op events
		 * during a drag.
		 *
		 * Why this matters beyond "scroll handlers now work": fixes1011
		 * made getBoundingClientRect return real geometry, so a site's own
		 * lazy-load test (rect.top < innerHeight) began correctly answering
		 * "below the fold" for images it used to load eagerly by accident
		 * when every rect was zero. Those images then wait for a scroll
		 * event -- which nothing fired. Hence "images loaded fine before".
		 *
		 * AFTER the scrollbar/invalidate work, so a handler that measures
		 * sees the new position, and the gate inside means a page with no
		 * scroll listener pays nothing. */
		int moved = (g->scroll_x != nx) || (g->scroll_y != ny);
		g->scroll_x = nx; g->scroll_y = ny;
		macos9_window_update_scrollbars(g);
		macos9_window_invalidate_content(g);
		if (moved) {
			extern void macsurf_qjs_fire_scroll(void);
			macsurf_qjs_fire_scroll();
		}
	}
}
void macos9_window_scroll_by(struct gui_window *g, int dx, int dy) { if(g) macos9_window_scroll_to(g, g->scroll_x+dx, g->scroll_y+dy); }

/* Auto-scroll during text-selection drag: if the cursor is near the top or
 * bottom edge of the content area, scroll the page (throttled).  Extracted
 * from the 9-level-deep StillDown() loop in main.c:macos9_handle_mouse_down. */
void macos9_drag_autoscroll(struct gui_window *gw, Point curp,
		unsigned long *last_scroll_tick)
{
	int edge = 0;
	if (gw == NULL || last_scroll_tick == NULL) return;
	if (curp.v < gw->content_rect.top + 6)
		edge = -1;
	else if (curp.v > gw->content_rect.bottom - 6)
		edge = 1;
	if (edge != 0) {
		unsigned long st = TickCount();
		if (st - *last_scroll_tick >= 2) {
			*last_scroll_tick = st;
			macos9_window_scroll_by(gw, 0, edge * 40);
		}
	}
}

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
	case 129: {                            /* thumb drag - LIVE (fixes749 #215) */
		/* The old TrackControl(c,pt,NULL) blocked until release, so the view
		 * only jumped at the end (and the live Appearance CDEF proc-386 crashes
		 * on real hardware - see the gotcha in CLAUDE.md). Instead poll the
		 * thumb ourselves: map the mouse to a value, scroll, and repaint the
		 * content live (throttled), all with the safe proc-384 control. */
		Rect cb;
		short arrow = 16;
		unsigned long lastdraw = 0;
		GetControlBounds(c, &cb);
		while (StillDown()) {
			Point mp;
			long span, rel, val;
			GetMouse(&mp);
			if (c == g->vscroll) {
				span = (long)((cb.bottom - arrow) - (cb.top + arrow));
				rel  = (long)(mp.v - (cb.top + arrow));
			} else {
				span = (long)((cb.right - arrow) - (cb.left + arrow));
				rel  = (long)(mp.h - (cb.left + arrow));
			}
			if (span < 1) span = 1;
			if (rel < 0) rel = 0;
			if (rel > span) rel = span;
			val = rel * (long)mx / span;
			if ((short)val != GetControlValue(c)) {
				SetControlValue(c, (short)val);
				if (c == g->vscroll)
					macos9_window_scroll_to(g, g->scroll_x, (int)val);
				else
					macos9_window_scroll_to(g, (int)val, g->scroll_y);
				macos9_throttled_repaint(g, &lastdraw);
			}
		}
		cur = GetControlValue(c);
		break;
	}
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

/* Shared by scrollbar drag + text-selection drag loops: repaint the window
 * if >= 2 ticks have passed since *last_tick.  Updates *last_tick on paint.
 * Extracted from the two StillDown() polling sites that duplicated this. */
void macos9_throttled_repaint(struct gui_window *gw, unsigned long *last_tick)
{
	unsigned long nowt;
	if (gw == NULL || last_tick == NULL) return;
	nowt = TickCount();
	if (nowt - *last_tick >= 2) {
		EventRecord uev;
		*last_tick = nowt;
		uev.what = updateEvt;
		uev.message = (long)(unsigned long)gw->window;
		macos9_handle_update(&uev);
	}
}

void macos9_urlsug_hide(struct gui_window *g);   /* fixes763 fwd - defined below */
void macos9_window_te_activate_url(struct gui_window *g) { if(!g||!g->url_te||g->url_field_active) return; SetPortWindowPort(g->window); TEActivate(g->url_te); g->url_field_active=1; InvalWindowRect(g->window, &g->url_rect); }
void macos9_window_te_deactivate_url(struct gui_window *g) { if(!g||!g->url_te||!g->url_field_active) return; macos9_urlsug_hide(g); SetPortWindowPort(g->window); TEDeactivate(g->url_te); g->url_field_active=0; InvalWindowRect(g->window, &g->url_rect); }

/* fixes756 (#229) - give the single-line URL field a WIDE destRect so a long
 * URL lays out on one line extending past the visible viewRect instead of
 * wrapping/clipping with no way to reach the end. TESelView (called after
 * each keystroke) then scrolls the caret into view. Only the right edge is
 * stretched; left is kept at the view edge so a freshly-set URL shows from
 * the start (resets any horizontal scroll left over from a prior long URL). */
/* fixes938 - lock the TEHandle across the master-pointer write.
 *
 * This dereferenced an UNLOCKED relocatable handle and then wrote through the
 * resulting TERec*, which is out of step with the rest of this file (the text
 * handles at the TEGetText sites are HLock/HUnlock'd properly). Nothing
 * allocates between the deref and the writes today, so this is latent rather
 * than the cause of the OS X TESetText crash -- but it is exactly the shape
 * of bug that becomes real the moment someone adds an allocating call here,
 * and Mac OS X's allocator is far more willing to move blocks than OS 9's.
 * HGetState/HSetState rather than a bare HLock/HUnlock so a caller that
 * already locked the handle stays locked on return. */
static void set_url_te_geometry(TEHandle te, const Rect *view) {
	TERec *p;
	Rect d;
	SignedByte st;
	if (te == NULL || view == NULL) return;
	st = HGetState((Handle)te);
	HLock((Handle)te);
	p = *te;
	d = *view;
	d.right = (short)(view->left + 8000);
	p->viewRect = *view;
	p->destRect = d;
	HSetState((Handle)te, st);
}

static void set_url_te_text(struct gui_window *g, const char *u) {
	CharsHandle h;
	long new_len;
	long cur_len;
	if(!g||!g->url_te||!u) return;
	/* fixes109 - dedupe. NetSurf core calls gui_window->set_url repeatedly
	 * during navigation (initial, after redirect, on every history nav, on
	 * some progress events). The old unconditional InvalRect on url_rect
	 * triggered an updateEvt → browser_window_redraw → draw_url_bar cycle
	 * on every call, even when the URL string was byte-identical. On a
	 * loading page the URL bar would visibly pulse for many seconds with
	 * nothing changing - that was a big part of the "sticky" feeling. Now:
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
	/* fixes756 (#229) - keep the wide destRect and reset scroll to the start
	 * so a newly-navigated URL shows the protocol/host, not a stale offset. */
	{
		Rect view;
		compute_url_te_rect(&g->url_rect, &view);
		set_url_te_geometry(g->url_te, &view);
	}
	InvalWindowRect(g->window, &g->url_rect);
}

/* fixes1198 - display-only URL bar update for history.pushState/replaceState: sets the
 * text shown in the address field without touching navigation state (no
 * fetch, no browser_window_navigate). Thin public wrapper over the same
 * dedupe/geometry logic macos9_window_navigate and the set_url callback
 * already use. */
void macos9_window_set_url_display(struct gui_window *g, const char *u) {
	if (!g || !u) return;
	set_url_te_text(g, u);
}

void macos9_window_navigate(struct gui_window *g, const char *u) {
	struct nsurl *n;
	nserror nav_e;
	long uu_len;
	int i;
	MS_LOG("navigate:");
	MS_LOG(u ? u : "(null)");
	/* fixes711 (#207): heap/VM snapshot immediately before this page loads,
	 * so a blank on THIS navigation is bracketed by the memory state that
	 * produced it. The URL is on the two MS_LOG lines just above. */
	macsurf_recon_mem("nav");
	if(!g||!u||!u[0]) { MS_LOG("nav: no g or empty u"); return; }
	if(!g->bw) { MS_LOG("nav: no bw"); return; }
	/* fixes705 - an explicit user navigation means "load this now": clear any
	 * session dead-host / terminal mark for the target so a host that
	 * transiently failed earlier this session gets a fresh attempt instead of
	 * fast-failing to a blank page. Sub-resource fetches don't come through
	 * here, so their in-page storm protection is untouched. */
	{
		extern void macos9_https_forget_host(const char *url);
		macos9_https_forget_host(u);
	}
	uu_len = (long)strlen(u);
	macsurf_debug_log_writef("nav: url len=%ld bytes", uu_len);
	for(i = 0; u[i] != 0; i++) {
		unsigned char c = (unsigned char)u[i];
		if (c < 0x20 || c == 0x7F) {
			macsurf_debug_log_writef("nav: ctrl-char at %d = %d", i, (int)c);
		}
	}
	set_url_te_text(g,u);
	set_status_text(g,"Loading...");
	if(g->window) InvalWindowRect(g->window, &g->status_rect);
	/* fixes449: log the pointer value before nsurl_create so that if
	 * lwc__intern fires a LWC-INTERN or CHAIN guard we can correlate
	 * the crash address against the string actually being interned. */
	macsurf_debug_log_writef(
		"nav: pre-create ptr=%p len=%d", (void *)u, (int)uu_len);
	if(nsurl_create(u,&n)!=NSERROR_OK) { MS_LOG("nav: nsurl_create FAIL"); return; }
	MS_LOG("nav: calling browser_window_navigate");
	{
		/* fixes161a - mark the next http_setup() as DOCUMENT so the
		 * resource governor gives it document-class priority, regardless
		 * of URL suffix. Single-shot: consumed by the first setup call. */
		extern void macos9_http_mark_next_as_document(void);
		extern void macsurf_site_navigation_reset(void);
		macos9_http_mark_next_as_document();
		/* fixes168f - clear per-page heavy latch + rgov skip counters
		 * so the next page is assessed fresh. */
		macsurf_site_navigation_reset();
	}
	/* fixes366a - reset the profile clock at the navigation entry
	 * point, BEFORE the fetch starts. Every macsurf_profile_stamp()
	 * call downstream (TLS, fetch, parse, cascade, layout, paint, JS)
	 * is then a delta from the moment the user kicked off this nav. */
	macsurf_profile_reset();
	macsurf_profile_stamp("nav: browser_window_navigate entry");
	nav_e = browser_window_navigate(g->bw, n, NULL, BW_NAVIGATE_HISTORY, NULL, NULL, NULL);
	macsurf_debug_log_writef("nav: bw_navigate returned %d", (int)nav_e);
	nsurl_unref(n);
	MS_LOG("nav: done");
}

/* fixes376 - Cut / Copy / Paste / Select-All on the URL TextEdit field,
 * synced with the Carbon desk scrap. The URL field holds MacRoman bytes and
 * the scrap 'TEXT' flavor is MacRoman, so no UTF-8 conversion is needed on
 * this path (that conversion lives only in clipboard.c's core callbacks).
 * edit_item is one of the ITEM_EDIT_* selectors from macos9.h. */
void macos9_url_te_edit(struct gui_window *g, short edit_item)
{
#ifdef __MACOS9__
	TEHandle te;
	TEPtr    tp;
	short    selStart;
	short    selEnd;

	if (g == NULL || g->url_te == NULL)
		return;
	te = g->url_te;

	SetPortWindowPort(g->window);

	switch (edit_item) {

	case ITEM_EDIT_SELECT_ALL:
		TESetSelect(0, 32767, te);
		InvalWindowRect(g->window, &g->url_rect);
		break;

	case ITEM_EDIT_COPY:
	case ITEM_EDIT_CUT: {
		CharsHandle hText;
		ScrapRef    scrap;
		char       *copybuf;
		short       selLen;

		tp = *te;
		selStart = tp->selStart;
		selEnd   = tp->selEnd;
		if (selEnd <= selStart)
			break;			/* empty selection */
		selLen = (short)(selEnd - selStart);

		hText = TEGetText(te);
		if (hText == NULL)
			break;

		copybuf = (char *)malloc((size_t)selLen);
		if (copybuf != NULL) {
			HLock((Handle)hText);
			memcpy(copybuf, (*hText) + selStart, (size_t)selLen);
			HUnlock((Handle)hText);

			if (ClearCurrentScrap() == noErr &&
			    GetCurrentScrap(&scrap) == noErr) {
				(void)PutScrapFlavor(scrap, kScrapFlavorTypeText,
					kScrapFlavorMaskNone,
					(Size)selLen, copybuf);
			}
			free(copybuf);
		}

		if (edit_item == ITEM_EDIT_CUT) {
			TEDelete(te);
			TECalText(te);
			InvalWindowRect(g->window, &g->url_rect);
		}
		break;
	}

	case ITEM_EDIT_PASTE: {
		ScrapRef scrap;
		Size     n;
		char    *buf;

		if (GetCurrentScrap(&scrap) != noErr)
			break;
		n = 0;
		if (GetScrapFlavorSize(scrap, kScrapFlavorTypeText, &n) != noErr)
			break;
		if (n <= 0)
			break;

		buf = (char *)malloc((size_t)n);
		if (buf == NULL)
			break;
		if (GetScrapFlavorData(scrap, kScrapFlavorTypeText, &n, buf)
				== noErr && n > 0) {
			TEDelete(te);		/* replace selection */
			TEInsert(buf, (long)n, te);
			TECalText(te);
			InvalWindowRect(g->window, &g->url_rect);
		}
		free(buf);
		break;
	}

	default:
		break;
	}
#else
	(void)g; (void)edit_item;
#endif
}

/* fixes762 - inline address-bar autocomplete from visit history. After a
 * forward keystroke, find the most-recent history URL whose host/path (scheme
 * and optional leading "www." stripped) begins with what the user typed, fill
 * the remainder into the field, and SELECT that added tail so the next
 * keystroke replaces it (classic type-ahead). Returns 1 if a completion was
 * inserted. Submit defaults a bare host to https:// (fixes249) with http
 * fallback (fixes317), so accepting a suggestion navigates correctly. Skipped
 * when the user is typing an explicit scheme (contains "://"). */
static const char *ac_strip_scheme(const char *u)
{
	if (u == NULL) return u;
	if (strncasecmp(u, "https://", 8) == 0) return u + 8;
	if (strncasecmp(u, "http://", 7) == 0) return u + 7;
	return u;
}

int macos9_url_autocomplete(struct gui_window *g)
{
	extern int macos9_history_count(void);
	extern const char *macos9_history_entry_url(int i);
	CharsHandle h;
	long len;
	char typed[512];
	int i, n;
	size_t tl;
	if (g == NULL || g->url_te == NULL) return 0;
	h = TEGetText(g->url_te);
	if (h == NULL) return 0;
	len = GetHandleSize((Handle)h);
	if (len < 2 || len >= (long)sizeof(typed)) return 0;
	memcpy(typed, *h, (size_t)len);
	typed[len] = '\0';
	if (strstr(typed, "://") != NULL) return 0;   /* explicit URL - don't fight it */
	tl = (size_t)len;
	n = macos9_history_count();
	for (i = 0; i < n; i++) {
		const char *hurl = macos9_history_entry_url(i);
		const char *hs, *base;
		size_t bl;
		if (hurl == NULL) continue;
		hs = ac_strip_scheme(hurl);
		base = NULL;
		if (strncasecmp(hs, typed, tl) == 0) {
			base = hs;
		} else if (strncasecmp(hs, "www.", 4) == 0 &&
			   strncasecmp(hs + 4, typed, tl) == 0) {
			base = hs + 4;
		}
		if (base == NULL) continue;
		bl = strlen(base);
		if (bl <= tl) continue;   /* nothing to add */
		{
			char shown[1024];
			Rect vr;
			if (bl >= sizeof(shown)) continue;
			memcpy(shown, base, bl);
			shown[bl] = '\0';
			SetPortWindowPort(g->window);
			TESetText(shown, (long)bl, g->url_te);
			TECalText(g->url_te);
			compute_url_te_rect(&g->url_rect, &vr);
			set_url_te_geometry(g->url_te, &vr);  /* wide destRect, start visible */
			TESetSelect((long)tl, (long)bl, g->url_te);
			InvalWindowRect(g->window, &g->url_rect);
		}
		return 1;
	}
	return 0;
}

/* ---- fixes763: address-bar suggestion dropdown ---------------------------
 * An in-window overlay drawn just below the URL bar listing up to URLSUG_MAX
 * history matches for what the user typed. Down/Up arrows move the highlight
 * (and fill the field); a click picks a row; Enter accepts the field; Esc /
 * navigation / clicking away dismisses it. Kept in the main window (not a
 * separate window) so it needs no extra activation/event routing. */
#define URLSUG_MAX  6
#define URLSUG_ROWH 18
static char g_urlsug[URLSUG_MAX][320];
static int  g_urlsug_n = 0;
static int  g_urlsug_sel = -1;
static struct gui_window *g_urlsug_gw = NULL;

int macos9_urlsug_active(struct gui_window *g)
{
	return (g_urlsug_n > 0 && g_urlsug_gw == g);
}

static void macos9_urlsug_rect(struct gui_window *g, Rect *out)
{
	out->left   = g->url_rect.left;
	out->top    = (short)(g->url_rect.bottom + 1);
	out->right  = g->url_rect.right;
	out->bottom = (short)(out->top + g_urlsug_n * URLSUG_ROWH + 2);
}

void macos9_urlsug_hide(struct gui_window *g)
{
	if (g != NULL && g_urlsug_gw == g && g_urlsug_n > 0 && g->window != NULL) {
		Rect r;
		macos9_urlsug_rect(g, &r);
		InvalWindowRect(g->window, &r);   /* restore the content underneath */
	}
	g_urlsug_n = 0; g_urlsug_sel = -1; g_urlsug_gw = NULL;
}

static int macos9_url_field_text(struct gui_window *g, char *buf, size_t cap)
{
	CharsHandle h;
	long len;
	if (g == NULL || g->url_te == NULL) return 0;
	h = TEGetText(g->url_te);
	if (h == NULL) return 0;
	len = GetHandleSize((Handle)h);
	if (len < 0 || len >= (long)cap) return 0;
	memcpy(buf, *h, (size_t)len);
	buf[len] = '\0';
	return 1;
}

/* Rebuild the suggestion list from `typed` (scheme-stripped, de-duped). */
static int macos9_urlsug_build(struct gui_window *g, const char *typed)
{
	extern int macos9_history_count(void);
	extern const char *macos9_history_entry_url(int i);
	int i, n, cnt = 0;
	size_t tl;
	if (g == NULL || typed == NULL) { macos9_urlsug_hide(g); return 0; }
	tl = strlen(typed);
	if (tl < 2 || strstr(typed, "://") != NULL) { macos9_urlsug_hide(g); return 0; }
	n = macos9_history_count();
	for (i = 0; i < n && cnt < URLSUG_MAX; i++) {
		const char *hurl = macos9_history_entry_url(i);
		const char *hs, *base;
		int dup, k;
		if (hurl == NULL) continue;
		hs = ac_strip_scheme(hurl);
		base = NULL;
		if (strncasecmp(hs, typed, tl) == 0) base = hs;
		else if (strncasecmp(hs, "www.", 4) == 0 &&
			 strncasecmp(hs + 4, typed, tl) == 0) base = hs + 4;
		if (base == NULL || strlen(base) >= sizeof(g_urlsug[0])) continue;
		dup = 0;
		for (k = 0; k < cnt; k++)
			if (strcmp(g_urlsug[k], base) == 0) { dup = 1; break; }
		if (dup) continue;
		strcpy(g_urlsug[cnt], base);
		cnt++;
	}
	g_urlsug_n = cnt;
	g_urlsug_sel = -1;
	g_urlsug_gw = (cnt > 0) ? g : NULL;
	if (cnt == 0) macos9_urlsug_hide(g);
	return cnt;
}

void macos9_urlsug_draw(struct gui_window *g)
{
	Rect box, row;
	int i;
	RGBColor black, white, hi, txt;
	if (!macos9_urlsug_active(g) || g->window == NULL) return;
	macos9_urlsug_rect(g, &box);
	SetPortWindowPort(g->window);
	black.red = black.green = black.blue = 0;
	white.red = white.green = white.blue = 0xFFFF;
	hi.red = 0xD800; hi.green = 0xE400; hi.blue = 0xFFFF;   /* light-blue selection */
	txt.red = txt.green = txt.blue = 0x1400;
	RGBForeColor(&white); PaintRect(&box);
	RGBForeColor(&black); FrameRect(&box);
	TextFont(kFontIDGeneva); TextSize(11); TextFace(0);
	for (i = 0; i < g_urlsug_n; i++) {
		int len;
		row.left   = (short)(box.left + 1);
		row.right  = (short)(box.right - 1);
		row.top    = (short)(box.top + 1 + i * URLSUG_ROWH);
		row.bottom = (short)(row.top + URLSUG_ROWH);
		if (i == g_urlsug_sel) { RGBForeColor(&hi); PaintRect(&row); }
		RGBForeColor(&txt);
		MoveTo((short)(row.left + 5), (short)(row.top + 13));
		len = (int)strlen(g_urlsug[i]);
		if (len > 160) len = 160;
		DrawText(g_urlsug[i], 0, (short)len);
	}
	/* reset fg/bg so a later CopyBits blit isn't tinted (colorize gotcha) */
	RGBForeColor(&black); RGBBackColor(&white);
}

/* Click hit-test: returns clicked row (0..n-1) or -1 if p is outside. */
int macos9_urlsug_hittest(struct gui_window *g, Point p)
{
	Rect box;
	int row;
	if (!macos9_urlsug_active(g)) return -1;
	macos9_urlsug_rect(g, &box);
	if (!PtInRect(p, &box)) return -1;
	row = (p.v - (box.top + 1)) / URLSUG_ROWH;
	if (row < 0) row = 0;
	if (row >= g_urlsug_n) row = g_urlsug_n - 1;
	return row;
}

const char *macos9_urlsug_row_text(int i)
{
	if (i < 0 || i >= g_urlsug_n) return NULL;
	return g_urlsug[i];
}

/* Down (+1) / Up (-1): move highlight, fill the field with that row. */
int macos9_urlsug_move(struct gui_window *g, int dir)
{
	Rect box;
	if (!macos9_urlsug_active(g)) return 0;
	if (g_urlsug_sel < 0) g_urlsug_sel = (dir > 0) ? 0 : g_urlsug_n - 1;
	else {
		g_urlsug_sel += dir;
		if (g_urlsug_sel < 0) g_urlsug_sel = g_urlsug_n - 1;
		if (g_urlsug_sel >= g_urlsug_n) g_urlsug_sel = 0;
	}
	set_url_te_text(g, g_urlsug[g_urlsug_sel]);
	if (g->url_te) TESetSelect(32767, 32767, g->url_te);
	macos9_urlsug_rect(g, &box);
	if (g->window) InvalWindowRect(g->window, &box);
	return 1;
}

/* Forward-typing entry point: build the dropdown from the raw typed text, then
 * inline-complete, then draw. Returns the inline-complete result. */
int macos9_url_typeahead(struct gui_window *g)
{
	char typed[512];
	int r;
	if (!macos9_url_field_text(g, typed, sizeof(typed))) { macos9_urlsug_hide(g); return 0; }
	macos9_urlsug_build(g, typed);
	r = macos9_url_autocomplete(g);
	macos9_urlsug_draw(g);
	return r;
}

/* Editing (backspace/delete) entry point: rebuild the dropdown for the new
 * field text without inline-completing. */
void macos9_urlsug_refresh(struct gui_window *g)
{
	char typed[512];
	if (!macos9_url_field_text(g, typed, sizeof(typed))) { macos9_urlsug_hide(g); return; }
	macos9_urlsug_build(g, typed);
	if (macos9_urlsug_active(g)) macos9_urlsug_draw(g);
}

/* Mouse-down (window-local point): if it lands on a dropdown row, put that URL
 * in the field and navigate. Returns 1 if the click was consumed. */
int macos9_urlsug_click(struct gui_window *g, Point p)
{
	int row;
	if (!macos9_urlsug_active(g)) return 0;
	row = macos9_urlsug_hittest(g, p);
	if (row < 0) return 0;
	set_url_te_text(g, g_urlsug[row]);
	if (g->url_te) TESetSelect(32767, 32767, g->url_te);
	macos9_window_address_bar_submit(g);   /* navigates + hides the dropdown */
	return 1;
}

void macos9_window_address_bar_submit(struct gui_window *g) {
	CharsHandle h; long l; char r[1024], f[1024];
	long i, j;
	MS_LOG("URL submit fired");
	if(!g||!g->url_te) { MS_LOG("submit: no g or url_te"); return; }
	macos9_urlsug_hide(g);   /* fixes763 - dismiss the suggestion dropdown */
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
	/* fixes317a - repair single-slash schemes. User typos or TextEdit
	 * slash-mangling occasionally land "https:/example.com/" (one slash)
	 * in r. The no-scheme check below uses strstr(r,"://") which misses
	 * the single-slash form, then prepends "https://" → URL becomes
	 * "https://https:/example.com/" which nsurl parses as host=https,
	 * path=/example.com/. Repair to canonical "https://" before the
	 * scheme check. Same for http:/. */
	if (strncmp(r, "https:/", 7) == 0 && r[7] != '/' && j + 1 < (long)sizeof(r)) {
		memmove(r + 8, r + 7, (size_t)(j - 7 + 1));
		r[7] = '/';
		j++;
		macsurf_debug_log_writef("submit: repaired single-slash https → '%s'", r);
	} else if (strncmp(r, "http:/", 6) == 0 && r[6] != '/' && j + 1 < (long)sizeof(r)) {
		memmove(r + 7, r + 6, (size_t)(j - 6 + 1));
		r[6] = '/';
		j++;
		macsurf_debug_log_writef("submit: repaired single-slash http → '%s'", r);
	}
	/* fixes249 - default scheme is https://. Modern web is HTTPS-only;
	 * defaulting to http meant typed-by-name domains (google.com,
	 * apple.com, etc.) hit dead http:// endpoints and routed to
	 * about:fetcherror. Sites that only serve plain HTTP still work
	 * because fixes317 now ALWAYS attempts the other scheme on failure
	 * (regardless of what the user typed), one shot per scheme per host
	 * per navigation, bounce-loop-safe. */
	/* fixes351 (#99) - proper scheme detection that handles opaque
	 * schemes (about:, data:, javascript:, mailto:, file:, resource:,
	 * about:blank, etc.) too, not just hierarchical ones. The previous
	 * strstr(r,"://") heuristic only saw the `//` after http/https/file/
	 * ftp etc. and forced an https:// prepend onto everything else -
	 * `about:cache` got mangled to `https://about:cache`, nsurl parsed
	 * that as host=about, path=cache, fetcher 404'd, page went blank.
	 *
	 * RFC 3986 scheme grammar: ALPHA *( ALPHA / DIGIT / "+" / "-" / "." )
	 * followed by ":". Walk the URL looking for that shape; if the colon
	 * lands before any "/", "?", "#", "@", or whitespace, the leading
	 * token is a scheme and the URL is already scheme-prefixed. Otherwise
	 * it's a bare hostname/path and gets the https:// default.
	 */
	{
		long i;
		int has_scheme = 0;
		if (r[0] != 0 &&
		    ((r[0] >= 'a' && r[0] <= 'z') ||
		     (r[0] >= 'A' && r[0] <= 'Z'))) {
			for (i = 1; r[i] != 0; i++) {
				char c = r[i];
				if (c == ':') {
					has_scheme = 1;
					break;
				}
				if ((c >= 'a' && c <= 'z') ||
				    (c >= 'A' && c <= 'Z') ||
				    (c >= '0' && c <= '9') ||
				    c == '+' || c == '-' || c == '.')
					continue;
				break;
			}
		}
		if (!has_scheme) {
			sprintf(f, "https://%s", r);
		} else {
			strcpy(f, r);
		}
	}
	/* fixes304 - URL-bar Enter bypasses the disk cache for the first
	 * fetch of the new navigation (one-shot, cleared by macos9_cache_store
	 * after the network response lands). Without this, re-typing a URL on
	 * a page you just edited server-side keeps serving the stale cached
	 * HTML, and any updated <img> / <link> refs never get re-requested.
	 * Reload already sets this flag; URL-bar nav now matches. Link clicks
	 * within a page (browser_window internal nav) still use the cache. */
	{
		extern int macsurf_http_skip_next_cache;
		macsurf_http_skip_next_cache = 1;
	}
	macos9_window_navigate(g,f);
}

void macos9_window_back(struct gui_window *g) { if(g&&g->bw&&browser_window_history_back_available(g->bw)) { browser_window_history_back(g->bw, false); macos9_window_update_button_states(g); } }
void macos9_window_forward(struct gui_window *g) { if(g&&g->bw&&browser_window_history_forward_available(g->bw)) { browser_window_history_forward(g->bw, false); macos9_window_update_button_states(g); } }
extern int macsurf_http_skip_next_cache;
void macos9_window_stop(struct gui_window *g) { if(g&&g->bw) { browser_window_stop(g->bw); macos9_window_update_button_states(g); } }  /* fixes724 */
void macos9_window_reload(struct gui_window *g) { if(g&&g->bw) { macsurf_http_skip_next_cache = 1; browser_window_reload(g->bw, true); } }
void macos9_window_home(struct gui_window *g) { macos9_window_navigate(g, macos9_home_url()); }

void macos9_window_update_button_states(struct gui_window *g) {
#ifdef __MACOS9__
	if(!g) return; SetPortWindowPort(g->window);
	if(g->back_btn) { HiliteControl(g->back_btn, (short)(g->bw && browser_window_history_back_available(g->bw)?0:255)); Draw1Control(g->back_btn); }
	if(g->forward_btn) { HiliteControl(g->forward_btn, (short)(g->bw && browser_window_history_forward_available(g->bw)?0:255)); Draw1Control(g->forward_btn); }
	if(g->stop_btn) { HiliteControl(g->stop_btn, (short)(g->bw && browser_window_stop_available(g->bw)?0:255)); Draw1Control(g->stop_btn); }  /* fixes724 */
	if(g->reload_btn) { HiliteControl(g->reload_btn, (short)(g->bw && browser_window_has_content(g->bw)?0:255)); Draw1Control(g->reload_btn); }
	if(g->home_btn) Draw1Control(g->home_btn);
	/* fixes297c - repaint icon overlay over every freshly-redrawn
	 * platinum button.  Without this, every navigation / state-change
	 * call from NetSurf core flashes the platinum chrome without the
	 * icon on top.  Cheap (single CopyBits per button).  Same helper
	 * we use after DrawControls in the update handler. */
	macos9_window_draw_toolbar_icons(g);
#endif
}

void macos9_window_resize(struct gui_window *g) {
	Rect tr; if(!g) return; macos9_window_layout(g);
#ifdef __MACOS9__
	if(g->url_te) { compute_url_te_rect(&g->url_rect, &tr); set_url_te_geometry(g->url_te, &tr); TECalText(g->url_te); } /* fixes756 (#229) wide destRect */
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

/* fixes366n - throttle the full-window REPAINT only.
 *
 * The mactrove repaint storm (~63 full bw_redraws / homepage load) came
 * from gw_event calling macos9_window_invalidate_all on every arriving
 * subresource (UPDATE_EXTENT / NEW_CONTENT) - layout itself is <1ms
 * (layout_us) and the heap is healthy, the cost is the repeated paints
 * (image blits, ~170ms each). Coalesce those into one repaint per
 * debounce window.
 *
 * fixes366m additionally tried to throttle the REFORMAT and deadlocked
 * into a 232s / 660-reformat loop: a reformat fires UPDATE_EXTENT,
 * which 366m had wired to re-arm needs_reformat, so the throttle
 * re-reformatted forever at the debounce cadence. Lesson: the reformat
 * path must stay as the pre-366m baseline - driven by NEW_CONTENT only
 * and coalesced by NetSurf's own scheduler, which converges. ONLY the
 * repaint is throttled here, and a repaint (invalidate) never fires
 * UPDATE_EXTENT, so there is no feedback loop. */
#define MACSURF_REPAINT_DEBOUNCE_TICKS 20  /* ~333ms @ 60 ticks/sec */
static unsigned long g_last_repaint_tick = 0;
static struct gui_window *g_repaint_pending_gw = NULL;

/* Request a coalesced full repaint of g (serviced, throttled, in
 * macos9_windows_process_deferred). */
static void macos9_window_request_repaint(struct gui_window *g) {
	g_repaint_pending_gw = g;
}

/* Repaint the pending window now, bypassing the throttle (load
 * complete - the final frame should not wait out the debounce). */
static void macos9_window_flush_repaint_now(void) {
	if (g_repaint_pending_gw != NULL && g_repaint_pending_gw->window != NULL) {
		macos9_window_invalidate_all(g_repaint_pending_gw);
#ifdef __MACOS9__
		g_last_repaint_tick = TickCount();
#endif
	}
	g_repaint_pending_gw = NULL;
}

void macos9_windows_process_deferred(void) {
	struct gui_window *g;
	/* reformat deferral - unchanged from the pre-366m baseline (RESIZE
	 * sets needs_reformat; NetSurf's scheduler coalesces the actual
	 * reformats). Must NOT be throttled (366m looped). */
	for(g=window_list;g;g=g->next) if(g->needs_reformat && g->bw && !g->reformat_in_progress) {
		g->reformat_in_progress=1; g->needs_reformat=0; browser_window_schedule_reformat(g->bw); g->reformat_in_progress=0;
	}
#ifdef __MACOS9__
	/* throttled repaint flush: at most one full invalidate per window
	 * per debounce window. First repaint after a quiet gap is immediate. */
	if (g_repaint_pending_gw != NULL && g_repaint_pending_gw->window != NULL) {
		unsigned long now = TickCount();
		if (g_last_repaint_tick == 0 ||
		    (now - g_last_repaint_tick) >= MACSURF_REPAINT_DEBOUNCE_TICKS) {
			macos9_window_invalidate_all(g_repaint_pending_gw);
			g_repaint_pending_gw = NULL;
			g_last_repaint_tick = now;
		}
	}
#endif
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
		/* preferences (macos9_prefs.c): user-set default new-window
		 * size; 0 = the built-in 1024x768 default. Still clamped to
		 * the screen below. */
		if (nsoption_int(window_width) > 0)
			want_w = (short)nsoption_int(window_width);
		if (nsoption_int(window_height) > 0)
			want_h = (short)nsoption_int(window_height);
		/* fixes859 (#287) - left 40 -> 8 and the right margin 20 -> 8.
		 * On the 1024x768 baseline this window used to open 964 wide, and
		 * after the 15px scrollbar that left a 949px viewport -- confirmed
		 * exactly by the fixes858 probe (mediaw=949 mediah=609, both
		 * predicted from these constants).  Desktop CSS breakpoints cluster
		 * right there: hackaday's is 59.5em = 952 CSS px once em resolves
		 * correctly (fixes859), so 949 missed the desktop branch BY THREE
		 * PIXELS and fell back to mobile -- no nav, 42px logo, 2.1rem title.
		 * 60px of a 1024px screen was going to margins we do not need; 8+8
		 * yields a 1008px window -> 993px viewport, clearing 952 with room
		 * and still leaving the window visibly framed on screen.  top/30
		 * are left alone: vertical has no breakpoint riding on it.
		 * NOTE both halves matter -- widening alone does nothing while
		 * @media still demands 1269px, and the em fix alone still lands 3px
		 * short in a 949px viewport. */
		left = 8;
		top = 50;
		if ((short)(sw - left - 8) < want_w) {
			want_w = (short)(sw - left - 8);
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
	/* fixes300/fixes723 - 36x36 square buttons (was 32x32, ~12.5% bigger).
	 * Vertically centered inside the 48-tall toolbar: top=6, bottom=42,
	 * height=36. 2px gap → 38px pitch → x=4,42,80,118, home's right edge
	 * 154, URL bar starts at x=156. paint_toolbar_icon auto-scales the
	 * 25x25 source to btn-8 = 28x28 centered, 4px padding every side. */
	/* fixes303 - tight 2px gap between buttons (38px pitch on 36px
	 * buttons) for a dense Netscape-7-style tool belt. */
	/* fixes724 - five buttons: Back, Forward, Stop, Refresh, Home. */
	x=4; SetRect(&b,x,6,(short)(x+36),42); g->back_btn=NewControl(g->window,&b,(const unsigned char*)"\p",1,0,0,0,256,(long)g); x=(short)(x+38);
	SetRect(&b,x,6,(short)(x+36),42); g->forward_btn=NewControl(g->window,&b,(const unsigned char*)"\p",1,0,0,0,256,(long)g); x=(short)(x+38);
	SetRect(&b,x,6,(short)(x+36),42); g->stop_btn=NewControl(g->window,&b,(const unsigned char*)"\p",1,0,0,0,256,(long)g); x=(short)(x+38);
	SetRect(&b,x,6,(short)(x+36),42); g->reload_btn=NewControl(g->window,&b,(const unsigned char*)"\p",1,0,0,0,256,(long)g); x=(short)(x+38);
	SetRect(&b,x,6,(short)(x+36),42); g->home_btn=NewControl(g->window,&b,(const unsigned char*)"\p",1,0,0,0,256,(long)g);
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
	/* fixes944 (OS X tier 2) - window port, but a GDevice whose colour table
	 * is real. See macos9_safe_gdevice() above for the full evidence chain; in
	 * short, GetMainDevice() on 10.3 hands back a GDevice with an unmapped
	 * colour table, and EVERY RGBForeColor resolves through the CURRENT device,
	 * so any colour call made while the screen device is current dies inside
	 * InternalColor2Index. fixes943 proved a GWorld's own device works fine.
	 *
	 * SetGWorld() takes port and device independently, so the window stays the
	 * drawing target while colour resolves through the good device. Setting
	 * fg/bg explicitly is the same house rule the plot_bitmap blits follow
	 * (CLAUDE.md's CopyBits-colorizing note): never assume the port's colour
	 * state. On OS 9 macos9_safe_gdevice() IS GetMainDevice(), so this is
	 * exactly the behaviour that has always shipped. */
	{
		RGBColor fg_blk, bg_wht;
		CGrafPtr wport = GetWindowPort(g->window);
		GDHandle usedev = macos9_safe_gdevice();

		SetGWorld(wport, usedev);
		fg_blk.red = 0; fg_blk.green = 0; fg_blk.blue = 0;
		bg_wht.red = 0xFFFF; bg_wht.green = 0xFFFF; bg_wht.blue = 0xFFFF;
		RGBForeColor(&fg_blk);
		RGBBackColor(&bg_wht);
		macsurf_debug_log_writef("BOOT te: port=%p dev=%p colours SET ok",
			(void *)wport, (void *)usedev);
		macsurf_debug_log_flush();
	}
	/* fixes302 - set the URL field font (Geneva 12) before TENew so the
	 * TERec captures it; TextEdit measures and draws with the stored font. */
	TextFont(kFontIDGeneva); TextSize(12); TextFace(0);
	compute_url_te_rect(&g->url_rect,&b); g->url_te=TENew(&b,&b);
	if(g->url_te) {
		/* fixes938 (OS X tier 1c) - BISECT the TextEdit setup on Mac OS X.
		 *
		 * fixes937's breadcrumbs put the 10.3 crash exactly here: startup
		 * reached "BOOT launch: create empty window, defer home nav", then
		 * EXC_BAD_ACCESS at 0x74140001 with the backtrace
		 *   macos9_window_create -> TESetText -> TECalText -> StdExit
		 *     -> RGBForeColor -> SetPortRGBForeColor -> InternalColor2Index
		 * i.e. TextEdit called into the port's colour machinery while
		 * recalculating, and that faulted. The port is the window's own
		 * (SetPortWindowPort above) and WRefCon was set before TENew, so the
		 * documented TENew/WRefCon gotcha is already satisfied.
		 *
		 * Two calls here go BEYOND stock TextEdit usage and both run
		 * immediately before the fault, so they are the bisect candidates:
		 *   - set_url_te_geometry() forces an 8000px-wide destRect
		 *     (fixes756 #229, for URL-bar horizontal scrolling)
		 *   - TEAutoView(true) enables auto-scroll, which makes TextEdit
		 *     DRAW/scroll during TESetText -- and drawing is what reaches
		 *     RGBForeColor.
		 * Skip both on OS X only. If TESetText then survives, one of these
		 * is the trigger and we re-add them individually. If it still dies,
		 * TextEdit itself is unusable here and tier 1d gates the field off.
		 *
		 * OS 9 behaviour is byte-for-byte unchanged. */
		/* fixes939 - RESTORED unconditionally. fixes938 gated these two off
		 * on OS X and 10.3 crashed anyway, with an identical backtrace and
		 * identical registers, so both are proven innocent. Putting them back
		 * returns OS X to the same code path as OS 9 and leaves the
		 * antialiased-text global (see main.c) as the single remaining
		 * difference between the two platforms. */
		set_url_te_geometry(g->url_te, &b);   /* fixes756 (#229) wide destRect */
		TEAutoView(true, g->url_te);          /* enable TESelView caret auto-scroll */
		MS_LOG("BOOT te: pre TESetText");
		TESetText(macos9_home_url(),(long)strlen(macos9_home_url()),g->url_te);
		MS_LOG("BOOT te: post TESetText");
		TECalText(g->url_te);
		MS_LOG("BOOT te: post TECalText");
		TEActivate(g->url_te);
		MS_LOG("BOOT te: post TEActivate");
		TESetSelect(0, 32767, g->url_te);  /* select-all so first keystroke replaces */
		MS_LOG("BOOT te: post TESetSelect");
	}
	GetWindowPortBounds(g->window,&b); b.right=(short)(b.right-b.left); b.bottom=(short)(b.bottom-b.top); b.left=0; b.top=0;
	InvalWindowRect(g->window,&b);
#endif
	g->url_field_active=1; macos9_window_update_scrollbars(g); macos9_window_update_button_states(g);
	return g;
}

void macos9_window_destroy(struct gui_window *g) { struct gui_window **p; for(p=&window_list;*p;p=&(*p)->next) if(*p==g) { *p=g->next; break; }
	/* fixes523 - kill the reload-icon animation before freeing g.  The
	 * tick (macos9_reload_anim_tick) reschedules itself every 200ms keyed
	 * on this gui_window* and is gated only by the global
	 * macos9_reload_animating flag.  If the window is torn down mid-load
	 * (navigate away / content abort / close) before GW_EVENT_STOP_THROBBER
	 * fires, a still-queued tick would fire against this freed g, deref
	 * g->window / g->reload_btn (freed-struct read, the r4=1 signature),
	 * and reschedule itself - driving repaint/re-entry against dead
	 * content.  Clear the flag and drop EVERY scheduled callback owned by
	 * g (cancel-by-owner, fixes517) so nothing can fire against it. */
	macos9_reload_animating = 0;
	macos9_schedule_cancel_owner(g);
#ifdef __MACOS9__
	if(g->content_gworld) { DisposeGWorld(g->content_gworld); g->content_gworld = NULL; }
#endif
	if(g->window) DisposeWindow(g->window); free(g); }
/* fixes100 - honour NetSurf's per-update dirty rect.
 *
 * Pre-fixes100 this function logged the supplied rect and then
 * unconditionally InvalWindowRect'd the entire content area. That
 * meant every keystroke (caret blink, textarea text change), every
 * scroll-bar value update, every late image arrival, every layout
 * tweak triggered a full content viewport repaint - on the duck-duck
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
static void macos9_window_draw_loader(struct gui_window *g);   /* fixes726 fwd */
static void macos9_window_draw_progress(struct gui_window *g); /* fixes727 fwd */

/* fixes297e - schedule callback that advances the throbber + progress-bar
 * animation frame and reschedules itself while the page is still loading.
 * Driven by macos9_schedule (NetSurf's timer registry). Param is the
 * gui_window*. */
static void macos9_reload_anim_tick(void *p)
{
#ifdef __MACOS9__
	struct gui_window *g = (struct gui_window *)p;
	struct gui_window *w;
	int alive;
	struct hlcache_handle *cur;
	/* fixes523 - bail BEFORE any deref if our window is gone.  This tick
	 * reschedules itself off the global macos9_reload_animating flag, so a
	 * stale entry left in the scheduler would otherwise read a freed
	 * gui_window* (g->window / g->reload_btn) and requeue forever, driving
	 * repaint/re-entry against dead content (the r4=00000001 freed-struct
	 * signature).  Confirm g is still a live, listed window first; if it
	 * isn't, clear the flag and do NOT reschedule. */
	if (!macos9_reload_animating) return;
	if (macos9_quitting) { macos9_reload_animating = 0; return; }
	alive = 0;
	for (w = window_list; w != NULL; w = w->next) {
		if (w == g) { alive = 1; break; }
	}
	if (!alive || g == NULL) { macos9_reload_animating = 0; return; }
	/* If the window's content is dead (no current content), there is
	 * nothing loading - stop the animation rather than spin against a
	 * torn-down hlcache handle. */
	cur = (g->bw != NULL) ? browser_window_get_content(g->bw) : NULL;
	if (cur == NULL) { macos9_reload_animating = 0; return; }
	macos9_reload_frame++;
	/* fixes726/fixes727 - advance the throbber + progress bar. Draw them
	 * DIRECTLY (small, self-bracketed regions) instead of invalidating, so
	 * a per-frame tick does not force a full toolbar chrome repaint. */
	if (g->window != NULL) {
		macos9_window_draw_loader(g);
		macos9_window_draw_progress(g);
	}
	if (macos9_reload_animating) {
		macos9_schedule(120, macos9_reload_anim_tick, p);
	}
#else
	(void)p;
#endif
}

static void macos9_gw_remove_caret(struct gui_window *g); /* fixes663 fwd */

static nserror macos9_gw_event(struct gui_window *g, enum gui_window_event e) {
	struct hlcache_handle *cur = (g && g->bw) ? browser_window_get_content(g->bw) : NULL;
	macsurf_debug_log_writef("gw_event: e=%d current_content=%p", (int)e, cur);
	/* fixes663 (#191): a field lost the caret (blur / focus change) - clear
	 * it so it does not linger after the user clicks out of the field. */
	if (e == GW_EVENT_REMOVE_CARET) {
		macos9_gw_remove_caret(g);
	}
	/* fixes726 - right-hand animated loading-spinner hook (was the refresh
	 * button spin). START begins the frame timer; STOP clears it and erases
	 * the spinner from its slot. */
	if (e == GW_EVENT_START_THROBBER) {
		macos9_reload_animating = 1;
		macos9_reload_frame = 0;
		macos9_schedule(120, macos9_reload_anim_tick, g);
#ifdef __MACOS9__
		if (g != NULL && g->window != NULL) {
			InvalWindowRect(g->window, &g->loader_rect);
		}
#endif
	}
	if (e == GW_EVENT_STOP_THROBBER) {
		macos9_reload_animating = 0;
#ifdef __MACOS9__
		if (g != NULL && g->window != NULL) {
			InvalWindowRect(g->window, &g->loader_rect);
		}
#endif
	}
	if(e==GW_EVENT_UPDATE_EXTENT||e==GW_EVENT_NEW_CONTENT||e==GW_EVENT_STOP_THROBBER) {
		int w=0, h=0; if(g->bw && browser_window_get_extents(g->bw, false, &w, &h)==NSERROR_OK) { g->content_width=w; g->content_height=h; }
		if(e==GW_EVENT_NEW_CONTENT) {
			g->scroll_x=0; g->scroll_y=0;
			g->caret_active=0; /* fixes663: drop a stale caret on navigation */
			/* reformat on NEW_CONTENT only - the pre-366m semantics.
			 * NetSurf's scheduler coalesces repeated calls and the
			 * reformat sequence converges. (366m wrongly drove reformat
			 * from UPDATE_EXTENT too, which a reformat re-fires -> loop.) */
			if(g->bw) browser_window_schedule_reformat(g->bw);
		}
		/* fixes366n - coalesce the repaint storm: request a THROTTLED
		 * full repaint instead of an immediate invalidate_all on every
		 * event. Scrollbar/button state stay live (cheap, Draw1Control,
		 * no content invalidate). STOP_THROBBER = load done -> repaint
		 * now so the final frame doesn't wait out the debounce. */
		macos9_window_update_scrollbars(g); macos9_window_update_button_states(g);
		macos9_window_request_repaint(g);
		if(e==GW_EVENT_STOP_THROBBER) {
			macos9_window_flush_repaint_now();
		}
	}
	return 0;
}
/* fixes320h - when JS sets document.title, NetSurf core's reformat
 * pipeline calls our set_title vtable entry shortly after with the
 * HTML <title> value, overwriting the JS-set title and producing the
 * "title flashes for a split second and resets" symptom.
 *
 * Lock the title once JS sets it. The vtable entry checks this flag
 * and skips its SetWTitle when JS has claimed the title. The lock
 * stays for the rest of the document's lifetime; navigation clears
 * it (see macos9_gw_set_url below). */
static int g_title_locked_by_js = 0;

void macos9_gw_set_title_unlock(void) { g_title_locked_by_js = 0; }

static void macos9__set_title_impl(struct gui_window *g, const char *t) {
	Str255 p;
	size_t in_l;
	size_t out_l;
	char mac_buf[256];
	if (!g || !g->window || !t) return;
	in_l = strlen(t);
	out_l = macos9_utf8_to_macroman(t, in_l, mac_buf, sizeof mac_buf);
	if (out_l > 255) out_l = 255;
	p[0] = (unsigned char)out_l;
	memcpy(p + 1, mac_buf, out_l);
	SetWTitle(g->window, p);
	/* fixes698 (#47) - record the visit in the persistent history store.
	 * This is the point where both the committed URL (from the browser
	 * window) and the human page title are known; macos9_history_record
	 * pulls the URL itself and ignores non-http(s) schemes. */
	macos9_history_record(g, t);
}

/* fixes319 - non-static so the JS bridge can drive document.title from
 * scripts. Vtable entry path: respects the JS lock. */
void macos9_gw_set_title(struct gui_window *g, const char *t) {
	if (g_title_locked_by_js) return;
	macos9__set_title_impl(g, t);
}

/* fixes320h - separate entry for the JS bridge so it can claim the
 * title without going through the locked vtable path. Sets the lock
 * before applying so subsequent NetSurf core calls are no-ops. */
void macos9_gw_set_title_from_js(struct gui_window *g, const char *t) {
	g_title_locked_by_js = 1;
	macos9__set_title_impl(g, t);
}
static nserror macos9_gw_set_url(struct gui_window *g, struct nsurl *u) {
	const char *s;
	/* fixes320h - navigation releases the JS title lock so the new
	 * page's HTML <title> applies through the normal vtable path. */
	g_title_locked_by_js = 0;
	/* fixes451: new URL = new page, reset the per-page profile gate */
	g->profile_emitted = 0;
	/* fixes640: ARM the PERFACC latch at nav-start (not when the poll first
	 * observes stop_available==true) so a cache-hit / synchronous-burst load
	 * that completes between two poll passes still emits its summary. The
	 * poll emits when stop_available goes false with perf_load_active set. */
	g->perf_load_active = 1;
	g->perf_summary_emitted = 0;
	/* fixes640b: do NOT call macsurf_profile_reset() here. set_url fires
	 * MULTIPLE times per load (redirect chains, late URL commits), so a reset
	 * here wiped the early-phase accumulators (parse/cascade/tls/js) MID-load,
	 * leaving only the tail (layout/paint) - observed as parse=0 cascade=0 in
	 * the first baseline. Per-load freshness is instead guaranteed by zeroing
	 * the accumulators at the END of macsurf_profile_emit_phases (so the next
	 * load starts clean regardless of nav type - this also covers the
	 * click-nav case the review flagged, without a mid-load wipe). */
	if(g&&u&&g->url_te&&(s=nsurl_access(u))) set_url_te_text(g,s);
	return 0;
}
static void macos9_gw_set_status(struct gui_window *g, const char *t) {
	/* fixes109 - dedupe. NetSurf core fires set_status on every fetch
	 * progress callback, every hover, every link-tracking transition.
	 * Many of those fire with the same string repeatedly (e.g. "Loading
	 * https://X..." while bytes accumulate). Unconditional InvalRect on
	 * status_rect was painting the status bar dozens of times per
	 * second on a loading page, costing CPU and showing as the pulsing
	 * status-bar redraw loop in the fixes107 log. Skip the InvalRect
	 * unless the visible text actually changed. */
	if (g == NULL || t == NULL) return;
	if (strcmp(g->status, t) == 0) return;
	/* fixes369c (#167) - log every DISTINCT status string. This is
	 * NetSurf core's load-lifecycle narration (Fetching… / Loading N
	 * objects / Done / error text) - the previously-uninstrumented middle
	 * between the fetcher trace and the bw_redraw counters. If the page
	 * stalls it shows the last state reached; if it fetches but never
	 * paints, it shows "Done" with no redraw following. Deduped above, so
	 * no spam. */
	macsurf_debug_log_writef("status: %s", t);
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

/* fixes294 - load the baked-in 16.png once into a GWorld kept alive for
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
		macsurf_debug_log_writef("favicon: lodepng err=%d w=%d h=%d",
			(int)err, (int)w, (int)h);
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
	/* fixes717: %u is unsupported by macsurf_debug_log_writef (printed
	 * literal "%u"); use %d with int casts. w/h are lodepng unsigned. */
	macsurf_debug_log_writef("favicon: loaded w=%d h=%d gworld=%p",
		(int)w, (int)h, (void *)macos9_default_favicon_gworld);
#endif
}

/* fixes294 - paint the cached favicon GWorld inside url_rect, called
 * from draw_url_bar AFTER TEUpdate so the icon is on top of the white
 * field background.  No allocation, no LockPixels - single CopyBits.
 * Full GetGWorld/SetGWorld bracket. */
void macos9_window_draw_favicon(struct gui_window *g)
{
#ifdef __MACOS9__
	GWorldPtr saved_port;
	GDHandle saved_gdh;
	const BitMap *src_bm;
	const BitMap *dst_bm;
	CGrafPtr win_port;
	GWorldPtr src_gworld;
	Rect *src_rect_ptr;
	Rect dst_rect;

	if (g == NULL || g->window == NULL) return;

	/* fixes295 Phase 1b - prefer active per-site favicon over default. */
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

	/* fixes728 - reset fg=black/bg=white before the blit. Classic QuickDraw
	 * colorizes CopyBits toward the port foreground (the fixes301j gotcha);
	 * without this the per-site favicon was tinted by whatever colour the
	 * previous draw left (the "favicon overlay-coloured after load" bug). The
	 * default favicon only looked right because draw_url_bar happened to leave
	 * fg=black. */
	{
		RGBColor blk = {0, 0, 0};
		RGBColor wht = {0xFFFF, 0xFFFF, 0xFFFF};
		RGBColor sfg, sbg;
		GetForeColor(&sfg); GetBackColor(&sbg);
		RGBForeColor(&blk); RGBBackColor(&wht);
		CopyBits(src_bm, dst_bm, src_rect_ptr, &dst_rect, srcCopy, NULL);
		RGBForeColor(&sfg); RGBBackColor(&sbg);
	}

	SetGWorld(saved_port, saved_gdh);
#else
	(void)g;
#endif
}

/* fixes295 Phase 1b - release the active per-site favicon GWorld and
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

/* fixes295 Phase 1b - pull the favicon bitmap from the hlcache_handle,
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
			/* fixes736 - matte transparent favicon pixels to WHITE (the URL
			 * field background) instead of forcing them opaque with the
			 * under-alpha colour. Site favicons are typically 16x16 with a
			 * transparent background; the old "force alpha 0xFF" made that
			 * background show its palette-entry colour, washing a solid
			 * colour over the whole icon (the "favicon overlay-coloured"
			 * bug). The blit is opaque srcCopy onto the white pill, so
			 * white-matted transparency reads as clean transparency. */
			unsigned char a  = src_row[col * 4 + 3];
			unsigned char r  = (a < 24) ? 0xFF : src_row[col * 4 + 0];
			unsigned char gn = (a < 24) ? 0xFF : src_row[col * 4 + 1];
			unsigned char b  = (a < 24) ? 0xFF : src_row[col * 4 + 2];
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

/* fixes295 Phase 1b - NetSurf set_icon callback. */
static void macos9_gw_set_icon(struct gui_window *g, struct hlcache_handle *icon)
{
#ifdef __MACOS9__
	if (g == NULL || g->window == NULL) return;
	macsurf_debug_log_writef("set_icon: icon=%p", (void *)icon);
	if (icon == NULL) {
		active_favicon_release();
	} else if (!active_favicon_build(icon)) {
		/* Build failed (bitmap not yet decoded, non-PNG, etc.) - keep
		 * whatever active we had; will fall back to default if none. */
	}
	InvalWindowRect(g->window, &g->url_rect);
#else
	(void)g; (void)icon;
#endif
}

/* fixes297 - decode a single PNG byte array into a permanent app-heap
 * GWorld + populate the src rect.  Helper for macos9_window_load_toolbar_icons.
 * Returns 1 on success, 0 on failure (out_gw stays NULL, caller skips paint
 * for that button - graceful fallback to text-only). */
static int macos9_decode_png_to_gworld(const unsigned char *png_bytes,
		unsigned long png_len, GWorldPtr *out_gw, Rect *out_src_rect)
{
#ifdef __MACOS9__
	extern unsigned lodepng_decode32(unsigned char **out, unsigned *w,
			unsigned *h, const unsigned char *in,
			unsigned long insize);
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

	*out_gw = NULL;
	err = lodepng_decode32(&rgba, &w, &h, png_bytes, png_len);
	if (err != 0 || rgba == NULL || w == 0 || h == 0) {
		if (rgba != NULL) free(rgba);
		return 0;
	}
	SetRect(out_src_rect, 0, 0, (short)w, (short)h);

	GetGWorld(&saved_port, &saved_gdh);
	oerr = NewGWorld(out_gw, 32, out_src_rect, NULL, NULL, 0);
	if (oerr != noErr || *out_gw == NULL) {
		free(rgba);
		SetGWorld(saved_port, saved_gdh);
		return 0;
	}
	pm = GetGWorldPixMap(*out_gw);
	if (pm == NULL || !LockPixels(pm)) {
		DisposeGWorld(*out_gw); *out_gw = NULL;
		free(rgba);
		SetGWorld(saved_port, saved_gdh);
		return 0;
	}
	/* fixes303 - bake the toolbar-grey background (#D6D6D6) into pixels
	 * the PNG marked transparent. The buttons are blitted via opaque
	 * CopyBits srcCopy, so transparent rounded corners would otherwise
	 * paint whatever RGB the PNG stored under alpha=0 (typically white
	 * → visible halo against the platinum toolbar). Matting once at
	 * decode time turns those corners into the exact toolbar grey so the
	 * icons sit on the toolbar seamlessly. */
	dst_rowbytes = (long)((*pm)->rowBytes & 0x3FFF);
	for (row = 0; row < (long)h; row++) {
		src_row = rgba + row * (long)w * 4L;
		dst_row = (unsigned char *)GetPixBaseAddr(pm) + row * dst_rowbytes;
		for (col = 0; col < (long)w; col++) {
			unsigned char a  = src_row[col * 4 + 3];
			unsigned char r  = (a < 8) ? 0xD6 : src_row[col * 4 + 0];
			unsigned char gn = (a < 8) ? 0xD6 : src_row[col * 4 + 1];
			unsigned char b  = (a < 8) ? 0xD6 : src_row[col * 4 + 2];
			dst_row[col * 4 + 0] = 0xFF;
			dst_row[col * 4 + 1] = r;
			dst_row[col * 4 + 2] = gn;
			dst_row[col * 4 + 3] = b;
		}
	}
	UnlockPixels(pm);
	SetGWorld(saved_port, saved_gdh);
	free(rgba);
	return 1;
#else
	(void)png_bytes; (void)png_len; (void)out_gw; (void)out_src_rect;
	return 0;
#endif
}

/* fixes297 - decode all four toolbar icons.  Best-effort: any failure
 * leaves its GWorld NULL and the corresponding button stays text-only. */
void macos9_window_load_toolbar_icons(void)
{
#ifdef __MACOS9__
	int ok = 0;
	if (macos9_toolbar_icons_loaded) return;
	ok += macos9_decode_png_to_gworld(macos9_btn_back_png,
		macos9_btn_back_png_len,
		&macos9_btn_back_gworld, &macos9_btn_back_src_rect);
	ok += macos9_decode_png_to_gworld(macos9_btn_forward_png,
		macos9_btn_forward_png_len,
		&macos9_btn_forward_gworld, &macos9_btn_forward_src_rect);
	ok += macos9_decode_png_to_gworld(macos9_btn_refresh_png,
		macos9_btn_refresh_png_len,
		&macos9_btn_refresh_gworld, &macos9_btn_refresh_src_rect);
	ok += macos9_decode_png_to_gworld(macos9_btn_home_png,
		macos9_btn_home_png_len,
		&macos9_btn_home_gworld, &macos9_btn_home_src_rect);
	/* fixes297d - disabled-state variants */
	ok += macos9_decode_png_to_gworld(macos9_btn_back_g_png,
		macos9_btn_back_g_png_len,
		&macos9_btn_back_g_gworld, &macos9_btn_back_g_src_rect);
	ok += macos9_decode_png_to_gworld(macos9_btn_forward_g_png,
		macos9_btn_forward_g_png_len,
		&macos9_btn_forward_g_gworld, &macos9_btn_forward_g_src_rect);
	/* fixes297e - refresh loading-state variant */
	ok += macos9_decode_png_to_gworld(macos9_btn_refresh_g_png,
		macos9_btn_refresh_g_png_len,
		&macos9_btn_refresh_g_gworld, &macos9_btn_refresh_g_src_rect);
	/* fixes297f - home dim variant for at-home detection */
	ok += macos9_decode_png_to_gworld(macos9_btn_home_g_png,
		macos9_btn_home_g_png_len,
		&macos9_btn_home_g_gworld, &macos9_btn_home_g_src_rect);
	/* fixes724 - Stop (X) coloured + grey */
	ok += macos9_decode_png_to_gworld(macos9_btn_stop_png,
		macos9_btn_stop_png_len,
		&macos9_btn_stop_gworld, &macos9_btn_stop_src_rect);
	ok += macos9_decode_png_to_gworld(macos9_btn_stop_g_png,
		macos9_btn_stop_g_png_len,
		&macos9_btn_stop_g_gworld, &macos9_btn_stop_g_src_rect);
	macos9_toolbar_icons_loaded = 1;
	macsurf_debug_log_writef("toolbar icons loaded ok=%d/8", ok);
	/* fixes726 - decode the animated loader frames. Best-effort per frame;
	 * NULL GWorlds are skipped at paint time. */
	{
		int i, lok = 0;
		for (i = 0; i < MACOS9_LOADER_FRAMES; i++) {
			macos9_loader_gworld[i] = NULL;
			if (macos9_decode_png_to_gworld(macos9_loader_frame_png[i],
					macos9_loader_frame_len[i],
					&macos9_loader_gworld[i],
					&macos9_loader_src_rect[i])) {
				lok++;
			}
		}
		macos9_loader_frames_loaded = 1;
		macsurf_debug_log_writef("loader frames loaded ok=%d/%d",
			lok, (int)MACOS9_LOADER_FRAMES);
	}
#endif
}

/* fixes297 - paint a single icon GWorld over a control's left edge.
 * The icon is centered vertically inside the control's bounds and
 * inset 4px from the left.  Full GetGWorld/SetGWorld bracket. */
static void paint_toolbar_icon(WindowRef window, ControlRef ctrl,
		GWorldPtr src_gw, const Rect *src_rect)
{
#ifdef __MACOS9__
	Rect ctrl_bounds;
	Rect dst_rect;
	short icon_w, icon_h;
	short cy;
	GWorldPtr saved_port;
	GDHandle saved_gdh;
	const BitMap *src_bm;
	const BitMap *dst_bm;
	CGrafPtr win_port;

	if (window == NULL || ctrl == NULL || src_gw == NULL || src_rect == NULL) return;

	GetControlBounds(ctrl, &ctrl_bounds);
	icon_w = (short)(src_rect->right - src_rect->left);
	icon_h = (short)(src_rect->bottom - src_rect->top);
	(void)icon_w; (void)icon_h;
	/* fixes300 - paint into a fixed 4px-padded box inside the 32x32
	 * button bounds.  Source icons are mixed sizes (25x25, 35x35);
	 * CopyBits downscales/upscales as needed so the visible icon is
	 * always button_size - 8 in each dimension, with 4px breathing
	 * room on every side. */
	{
		short btn_w = (short)(ctrl_bounds.right - ctrl_bounds.left);
		short btn_h = (short)(ctrl_bounds.bottom - ctrl_bounds.top);
		short pad = 4;
		short dw = (short)(btn_w - pad * 2);
		short dh = (short)(btn_h - pad * 2);
		if (dw < 1) dw = btn_w;
		if (dh < 1) dh = btn_h;
		dst_rect.left = (short)(ctrl_bounds.left + (btn_w - dw) / 2);
		dst_rect.top = (short)(ctrl_bounds.top + (btn_h - dh) / 2);
		dst_rect.right = (short)(dst_rect.left + dw);
		dst_rect.bottom = (short)(dst_rect.top + dh);
		cy = dst_rect.top;
	}

	GetGWorld(&saved_port, &saved_gdh);
	SetPortWindowPort(window);
	win_port = GetWindowPort(window);
	if (win_port == NULL) { SetGWorld(saved_port, saved_gdh); return; }

	/* fixes303 - paint the button slot to the exact toolbar grey
	 * (#D6D6D6) and skip the per-button frame. Combined with the matted
	 * icon corners (also #D6D6D6), the buttons no longer have white
	 * halos OR individual frames - they read as a dense "tool belt"
	 * row of icons on the toolbar rather than four chrome buttons. */
	{
		RGBColor saved_fg, saved_bg;
		GetForeColor(&saved_fg); GetBackColor(&saved_bg);
		/* fixes727 - paint the slot with the toolbar gradient (not a flat
		 * grey patch) so the button band matches the surrounding sheen. */
		macos9_tb_fill_gradient(&ctrl_bounds);
		/* fixes727 - soft hover highlight: a light rounded plate behind the
		 * icon when the pointer is over this button (the orange edge is
		 * added later in macos9_window_draw_toolbar_icons). */
		if (ctrl == macos9_hovered_btn) {
			RGBColor hl = {0xEAEA, 0xE7E7, 0xE0E0};   /* warm light grey */
			Rect hr = ctrl_bounds;
			InsetRect(&hr, 2, 2);
			RGBForeColor(&hl);
			PaintRoundRect(&hr, 10, 10);
		}
		RGBForeColor(&saved_fg); RGBBackColor(&saved_bg);
	}

	dst_bm = GetPortBitMapForCopyBits(win_port);
	src_bm = GetPortBitMapForCopyBits((CGrafPtr)src_gw);
	if (src_bm != NULL && dst_bm != NULL) {
		/* fixes729a - force fg=black/bg=white for the blit. Classic
		 * QuickDraw colorizes CopyBits toward the port foreground
		 * (fixes301j); the icons were inheriting whatever colour the
		 * previous chrome/content draw left, tinting the whole tool belt
		 * pale-yellow and shifting between redraws (the "glitchy fade").
		 * The slot-fill block above restored the (possibly coloured) entry
		 * fg, so reset it here right before the copy. */
		RGBColor blk = {0, 0, 0};
		RGBColor wht = {0xFFFF, 0xFFFF, 0xFFFF};
		RGBColor sfg, sbg;
		GetForeColor(&sfg); GetBackColor(&sbg);
		RGBForeColor(&blk); RGBBackColor(&wht);
		CopyBits(src_bm, dst_bm, src_rect, &dst_rect, srcCopy, NULL);
		RGBForeColor(&sfg); RGBBackColor(&sbg);
	}
	SetGWorld(saved_port, saved_gdh);
#else
	(void)window; (void)ctrl; (void)src_gw; (void)src_rect;
#endif
}

/* fixes726/fixes727 - Netscape-style throbber in the far-right toolbar slot
 * (g->loader_rect). It is ALWAYS visible: the full frame set cycles while a
 * page is loading, and it rests on frame 0 (the idle pose) when done - never
 * an empty gap. The frame index is macos9_reload_frame, advanced by
 * macos9_reload_anim_tick. Frames are matted to toolbar grey, so blitting one
 * fully erases the previous frame (no separate clear needed). */
static void macos9_window_draw_loader(struct gui_window *g)
{
#ifdef __MACOS9__
	GrafPtr   saved_port;
	CGrafPtr  win_port;
	Rect      slot;
	int       idx;
	GWorldPtr src_gw;

	if (g == NULL || g->window == NULL || !macos9_loader_frames_loaded) return;
	slot = g->loader_rect;

	idx = macos9_reload_animating
		? (macos9_reload_frame % MACOS9_LOADER_FRAMES)
		: 0;   /* idle rest pose */
	src_gw = macos9_loader_gworld[idx];
	if (src_gw == NULL) src_gw = macos9_loader_gworld[0];
	if (src_gw == NULL) return;

	GetPort(&saved_port);
	SetPortWindowPort(g->window);
	win_port = GetWindowPort(g->window);
	if (win_port == NULL) { SetPort(saved_port); return; }
	{
		const BitMap *src_bm = GetPortBitMapForCopyBits((CGrafPtr)src_gw);
		const BitMap *dst_bm = GetPortBitMapForCopyBits(win_port);
		RGBColor saved_fg, saved_bg;
		RGBColor blk = {0,0,0}, wht = {0xFFFF,0xFFFF,0xFFFF};
		GetForeColor(&saved_fg); GetBackColor(&saved_bg);
		/* fg=black/bg=white so CopyBits doesn't colorize (plot_bitmap
		 * gotcha in CLAUDE.md). */
		RGBForeColor(&blk); RGBBackColor(&wht);
		if (src_bm != NULL && dst_bm != NULL) {
			CopyBits(src_bm, dst_bm,
				&macos9_loader_src_rect[idx], &slot,
				srcCopy, NULL);
		}
		RGBForeColor(&saved_fg); RGBBackColor(&saved_bg);
	}
	SetPort(saved_port);
#else
	(void)g;
#endif
}

/* fixes727 - page-load progress bar: a slim orange strip along the bottom edge
 * of the toolbar that creeps forward while a page loads (paired with the
 * throbber). Monotonic time-based creep toward ~92% (no per-byte wiring); the
 * STOP_THROBBER full repaint clears it. Drawn directly from the animation tick
 * AND from the chrome repaint, so both paths keep it current. No-op when idle
 * (the toolbar-bg repaint restores the bevel). */
static void macos9_window_draw_progress(struct gui_window *g)
{
#ifdef __MACOS9__
	GrafPtr  saved_port;
	Rect     w_bounds, strip, fill;
	long     track_w, seg_w, span, pos, seg_l, seg_r, step;
	RGBColor saved_fg;
	RGBColor track  = {0xC8C8, 0xC8C8, 0xC8C8};
	RGBColor orange = {0xF4F4, 0x8484, 0x1616};

	if (g == NULL || g->window == NULL) return;
	if (!macos9_reload_animating) return;

	GetPort(&saved_port);
	SetPortWindowPort(g->window);
	GetForeColor(&saved_fg);
	GetWindowBounds(g->window, 33, &w_bounds);

	strip.left   = 0;
	strip.right  = (short)(w_bounds.right - w_bounds.left);
	strip.bottom = (short)(g->content_rect.top - 1);   /* above the separator */
	strip.top    = (short)(strip.bottom - 3);          /* 3px tall */

	/* fixes732 - INDETERMINATE sweep. A ~28%-wide orange segment travels
	 * left->right across the track and wraps, so it can never pin at a fake
	 * percentage (this engine can't compute true load %). Continuous motion
	 * reads as "still working"; the old creep-to-92%-and-freeze looked stuck
	 * on long loads (68kmla). */
	track_w = (long)strip.right - (long)strip.left;
	if (track_w < 8) track_w = 8;
	seg_w = track_w * 28 / 100;
	if (seg_w < 8) seg_w = 8;
	span = track_w + seg_w;
	step = track_w / 10;
	if (step < 4) step = 4;
	pos   = ((long)macos9_reload_frame * step) % span;  /* segment trailing edge */
	seg_r = (long)strip.left + pos;
	seg_l = seg_r - seg_w;
	if (seg_l < (long)strip.left)  seg_l = (long)strip.left;
	if (seg_r > (long)strip.right) seg_r = (long)strip.right;

	RGBForeColor(&track);
	PaintRect(&strip);
	if (seg_r > seg_l) {
		fill = strip;
		fill.left  = (short)seg_l;
		fill.right = (short)seg_r;
		RGBForeColor(&orange);
		PaintRect(&fill);
	}

	RGBForeColor(&saved_fg);
	SetPort(saved_port);
#else
	(void)g;
#endif
}

/* fixes297 - paint all four toolbar icons over their buttons.  Called
 * after DrawControls in the update handler, so the platinum button
 * backgrounds and labels are already drawn underneath. */
void macos9_window_draw_toolbar_icons(struct gui_window *g)
{
#ifdef __MACOS9__
	GWorldPtr back_gw;
	Rect *back_rect;
	GWorldPtr fwd_gw;
	Rect *fwd_rect;
	int back_avail;
	int fwd_avail;

	if (g == NULL || g->window == NULL || !macos9_toolbar_icons_loaded) return;

	/* fixes297d - pick coloured icon when history nav is available,
	 * greyed icon when not.  Falls back to the coloured icon when the
	 * grey variant failed to load. */
	back_avail = (g->bw != NULL) &&
		browser_window_history_back_available(g->bw);
	fwd_avail = (g->bw != NULL) &&
		browser_window_history_forward_available(g->bw);

	if (back_avail || macos9_btn_back_g_gworld == NULL) {
		back_gw = macos9_btn_back_gworld;
		back_rect = &macos9_btn_back_src_rect;
	} else {
		back_gw = macos9_btn_back_g_gworld;
		back_rect = &macos9_btn_back_g_src_rect;
	}
	if (fwd_avail || macos9_btn_forward_g_gworld == NULL) {
		fwd_gw = macos9_btn_forward_gworld;
		fwd_rect = &macos9_btn_forward_src_rect;
	} else {
		fwd_gw = macos9_btn_forward_g_gworld;
		fwd_rect = &macos9_btn_forward_g_src_rect;
	}

	paint_toolbar_icon(g->window, g->back_btn, back_gw, back_rect);
	paint_toolbar_icon(g->window, g->forward_btn, fwd_gw, fwd_rect);
	/* fixes724 - Stop: orange X while the page is loading (stoppable), grey
	 * X when idle (non-active). */
	{
		int stop_avail = (g->bw != NULL) &&
			browser_window_stop_available(g->bw);
		if (stop_avail || macos9_btn_stop_g_gworld == NULL) {
			paint_toolbar_icon(g->window, g->stop_btn,
				macos9_btn_stop_gworld, &macos9_btn_stop_src_rect);
		} else {
			paint_toolbar_icon(g->window, g->stop_btn,
				macos9_btn_stop_g_gworld,
				&macos9_btn_stop_g_src_rect);
		}
	}
	/* fixes725 - refresh is always the coloured icon. The old loading-state
	 * toggle (fixes297e) is removed; a dedicated animated loader icon on the
	 * right of the nav bar will replace it. */
	paint_toolbar_icon(g->window, g->reload_btn,
		macos9_btn_refresh_gworld, &macos9_btn_refresh_src_rect);
	/* fixes725 - home is always the coloured icon. The at-home dim variant
	 * (fixes297f) is removed; it read as a broken greyed-out button. */
	paint_toolbar_icon(g->window, g->home_btn,
		macos9_btn_home_gworld, &macos9_btn_home_src_rect);

	/* fixes725 - hover highlight: a 1px rounded accent frame around whichever
	 * nav button the mouse is over (set by macos9_window_update_hover). A full
	 * icon repaint above already erased any previous frame. */
	if (macos9_hovered_btn != NULL &&
	    (macos9_hovered_btn == g->back_btn ||
	     macos9_hovered_btn == g->forward_btn ||
	     macos9_hovered_btn == g->stop_btn ||
	     macos9_hovered_btn == g->reload_btn ||
	     macos9_hovered_btn == g->home_btn)) {
		GrafPtr  sp;
		Rect     hbnd;
		RGBColor hi = {0xF4F4, 0x8484, 0x1616};   /* icon orange */
		RGBColor sfg;
		GetPort(&sp);
		SetPortWindowPort(g->window);
		GetControlBounds(macos9_hovered_btn, &hbnd);
		InsetRect(&hbnd, 2, 2);
		GetForeColor(&sfg);
		RGBForeColor(&hi);
		PenSize(1, 1);
		FrameRoundRect(&hbnd, 8, 8);
		RGBForeColor(&sfg);
		SetPort(sp);
	}

	/* fixes726 - paint the Netscape-style throbber in its right-hand slot. */
	macos9_window_draw_loader(g);
	/* fixes727 - page-load progress bar along the toolbar's bottom edge. */
	macos9_window_draw_progress(g);
#else
	(void)g;
#endif
}

/* fixes725 - mouse-over tracking for the nav buttons. Called from the event
 * loop's idle pass for the front window. Sets macos9_hovered_btn to whichever
 * nav button the pointer is over (or NULL) and, only when it changes, repaints
 * the toolbar icons so the hover frame appears/moves/clears. */
void macos9_window_update_hover(struct gui_window *g)
{
#ifdef __MACOS9__
	Point      p;
	Rect       bnd;
	GrafPtr    sp;
	ControlRef newh = NULL;
	ControlRef btns[5];
	int        i;

	if (g == NULL || g->window == NULL) return;
	GetPort(&sp);
	SetPortWindowPort(g->window);
	GetMouse(&p);   /* window-local, matching GetControlBounds */
	SetPort(sp);
	btns[0] = g->back_btn;  btns[1] = g->forward_btn; btns[2] = g->stop_btn;
	btns[3] = g->reload_btn; btns[4] = g->home_btn;
	for (i = 0; i < 5; i++) {
		if (btns[i] != NULL) {
			GetControlBounds(btns[i], &bnd);
			if (PtInRect(p, &bnd)) { newh = btns[i]; break; }
		}
	}
	if (newh != macos9_hovered_btn) {
		macos9_hovered_btn = newh;
		macos9_window_draw_toolbar_icons(g);
	}
#else
	(void)g;
#endif
}

/* fixes663 (#191): in-page text caret. NetSurf draws page-field carets ONLY
 * when the field carries TEXTAREA_INTERNAL_CARET; otherwise it delegates the
 * VISIBLE caret to this place_caret gui callback - and, crucially, the same
 * path sets keyboard focus (browser_window_place_caret sets root_bw->focus,
 * which is what browser_window_key_press routes on). The earlier attempt of
 * setting TEXTAREA_INTERNAL_CARET drew a caret but SUPPRESSED the CARET_UPDATE
 * message that drives html_set_focus, so arrows/Tab/copy and blur-cleanup all
 * broke - hence this proper frontend implementation instead. */
static void macos9_caret_invalidate(struct gui_window *g)
{
	struct rect r;
	if (g == NULL) return;
	r.x0 = g->caret_x - 1;
	r.y0 = g->caret_y;
	r.x1 = g->caret_x + 2;
	r.y1 = g->caret_y + g->caret_h + 1;
	macos9_gw_invalidate(g, &r);
}

static void macos9_caret_blink_tick(void *p)
{
	struct gui_window *g = (struct gui_window *)p;
	if (g == NULL || !g->caret_active) return; /* stop: do not reschedule */
	g->caret_on = !g->caret_on;
	macos9_caret_invalidate(g);
	macos9_schedule(500, macos9_caret_blink_tick, g);
}

static void macos9_gw_place_caret(struct gui_window *g, int x, int y,
		int height, const struct rect *clip)
{
	int was_active;
	(void)clip;
	if (g == NULL) return;
	was_active = g->caret_active;
	if (was_active) {
		macos9_caret_invalidate(g); /* erase the old position */
	}
	g->caret_x = x;
	g->caret_y = y;
	g->caret_h = height;
	g->caret_active = 1;
	g->caret_on = 1; /* show immediately when placed/moved */
	macos9_caret_invalidate(g); /* draw the new position */
	if (!was_active) {
		macos9_schedule(500, macos9_caret_blink_tick, g);
	}
}

static void macos9_gw_remove_caret(struct gui_window *g)
{
	if (g == NULL || !g->caret_active) return;
	g->caret_active = 0;
	macos9_caret_invalidate(g); /* erase (caret is drawn only while active) */
}

#ifdef __MACOS9__
/* fixes721 (#144 file upload) - build a full HFS path "Volume:dir:...:leaf"
 * from an FSSpec by walking up parent dir IDs (PBGetCatInfoSync), so the
 * multipart builder can fopen() the file the user picked. Returns 0 on
 * success. fixes838 (#167): un-static'd so cookie persistence can build the
 * full path MSL fopen honours (the ':MacSurfData:' colon-relative path did
 * not round-trip; a full Volume:...:leaf path does, as this upload path
 * proves). */
int macos9_fsspec_to_path(const FSSpec *spec, char *out, long cap)
{
	Str255      parts[48];
	int         nparts = 0;
	long        dirid = spec->parID;
	short       vref = spec->vRefNum;
	CInfoPBRec  pb;
	long        pos, i;
	int         L;

	BlockMoveData(spec->name, parts[nparts], (long)spec->name[0] + 1);
	nparts++;
	while (dirid != fsRtParID && nparts < 48) {
		Str255 nm;
		memset(&pb, 0, sizeof pb);
		pb.dirInfo.ioNamePtr   = nm;
		pb.dirInfo.ioVRefNum   = vref;
		pb.dirInfo.ioDrDirID   = dirid;
		pb.dirInfo.ioFDirIndex = -1;   /* info about the dir itself */
		if (PBGetCatInfoSync(&pb) != noErr) break;
		BlockMoveData(nm, parts[nparts], (long)nm[0] + 1);
		nparts++;
		dirid = pb.dirInfo.ioDrParID;
	}
	pos = 0;
	for (i = nparts - 1; i >= 0; i--) {
		L = parts[i][0];
		if (pos + L + 2 >= cap) return -1;
		memcpy(out + pos, parts[i] + 1, (size_t)L);
		pos += L;
		if (i > 0) out[pos++] = ':';
	}
	out[pos] = '\0';
	return 0;
}

/* fixes721 - file_gadget_open gui callback. Core calls this when the user
 * clicks an <input type=file>. Show a Navigation Services Open dialog (the
 * same generation as the fixes313a NavPutFile save dialog, which links under
 * CarbonLib), then hand the chosen file's path to core via
 * html_set_file_gadget_filename; the fetcher reads it at submit. */
static void macos9_window_file_gadget_open(struct gui_window *gw,
		struct hlcache_handle *hl, struct form_control *gadget)
{
	NavDialogOptions opts;
	NavReplyRecord   reply;
	OSErr            err;
	OSErr            aeerr;
	FSSpec           spec;
	AEKeyword        kw;
	DescType         dt;
	Size             sz;
	char             path[1024];
	extern void html_set_file_gadget_filename(struct hlcache_handle *,
			struct form_control *, const char *);

	(void)gw;
	/* fixes721b - "RECON" prefix so these lines survive the crash-only log
	 * gate (nuclear is off in shipping builds), letting a reporter's log show
	 * exactly where the picker failed. */
	if (NavGetDefaultDialogOptions(&opts) != noErr) {
		macsurf_debug_log_writef("RECON filegadget: NavGetDefaultDialogOptions FAIL");
		return;
	}
	err = NavGetFile(NULL, &reply, &opts, NULL, NULL, NULL, NULL, NULL);
	macsurf_debug_log_writef("RECON filegadget: NavGetFile err=%d valid=%d",
		(int)err, (int)(err == noErr ? reply.validRecord : 0));
	if (err != noErr) return;
	if (!reply.validRecord) { NavDisposeReply(&reply); return; }
	/* CarbonLib's NavGetFile reply may carry the selection as typeFSS OR
	 * typeFSRef; try FSS first, fall back to FSRef -> FSSpec. */
	aeerr = AEGetNthPtr(&reply.selection, 1, typeFSS, &kw, &dt,
			&spec, sizeof spec, &sz);
	if (aeerr != noErr) {
		FSRef ref;
		OSErr e2 = AEGetNthPtr(&reply.selection, 1, typeFSRef, &kw, &dt,
				&ref, sizeof ref, &sz);
		if (e2 == noErr)
			e2 = FSGetCatalogInfo(&ref, kFSCatInfoNone, NULL, NULL,
					&spec, NULL);
		aeerr = e2;
	}
	NavDisposeReply(&reply);
	macsurf_debug_log_writef("RECON filegadget: AE extract err=%d", (int)aeerr);
	if (aeerr != noErr) return;
	if (macos9_fsspec_to_path(&spec, path, (long)sizeof path) != 0) {
		macsurf_debug_log_writef("RECON filegadget: fsspec_to_path FAIL");
		return;
	}
	macsurf_debug_log_writef("RECON filegadget: path=%s", path);
	html_set_file_gadget_filename(hl, gadget, path);
	macsurf_debug_log_writef("RECON filegadget: filename set OK");
}
#else
static void macos9_window_file_gadget_open(struct gui_window *gw,
		struct hlcache_handle *hl, struct form_control *gadget)
{
	(void)gw; (void)hl; (void)gadget;
}
#endif

static struct gui_window_table wt = {
	macos9_window_create, macos9_window_destroy, macos9_gw_invalidate, macos9_gw_get_scroll,
	macos9_gw_set_scroll, macos9_gw_get_dimensions, macos9_gw_event, macos9_gw_set_title,
	macos9_gw_set_url, macos9_gw_set_icon, macos9_gw_set_status, macos9_gw_set_pointer, macos9_gw_place_caret,
	/* drag_start */ (void*)0, /* save_link */ (void*)0, /* create_form_select_menu */ (void*)0,
	/* fixes721 file_gadget_open */ macos9_window_file_gadget_open,
	/* drag_save_object */ (void*)0, /* drag_save_selection */ (void*)0, /* console_log */ (void*)0
};
struct gui_window_table *macos9_window_table = &wt;
