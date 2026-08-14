/*
 * MacSurf - Preferences window + persistence (macos9_prefs.c)
 *
 * Every browser setting was hardcoded at boot in main.c with no
 * user-adjustable UI. This module is the Preferences feature:
 *
 *   1. macos9_prefs_set_defaults() - the nsoption_init callback.
 *      The boot baseline (the old main.c hardcoded block) is applied
 *      here, INTO THE DEFAULT TABLE (nsoption_init temporarily
 *      redirects the global nsoptions onto the defaults table before
 *      calling it). Persistence then stores only USER DELTAS, so a
 *      choice that equals the factory default (e.g. JavaScript OFF)
 *      is still written and survives relaunch.
 *
 *   2. macos9_prefs_load()/macos9_prefs_save() - the "MacSurf
 *      Preferences" file in MacSurfData via the nsoption key:value
 *      file format (nsoption_read/nsoption_write). The full
 *      "Volume:...:MacSurfData:MacSurf Preferences" path is built
 *      with the FSMakeFSSpec + macos9_fsspec_to_path pattern
 *      (fixes838 - colon-relative paths do not round-trip through
 *      MSL fopen).
 *
 *   3. macos9_prefs_show() - the Preferences window: programmatic
 *      Carbon controls only (no DLOG/DITL resources), mirroring
 *      macos9_chrome_extras.c. Gold gradient banner, category popup
 *      (General / Appearance / Content / Privacy / Network),
 *      per-category checkboxes, popup buttons, TextEdit fields and
 *      push buttons, Defaults / Cancel / OK. No action UPPs
 *      (TrackControl + GetControlValue), no live-track CDEFs
 *      (kControlScrollBarLiveProc crashes on real G3/G4 hardware).
 *
 * C89 / CW8-clean: no inline, no // comments, declarations at the
 * top of every block.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

#include "utils/ns_errors.h"
#include "utils/nsoption.h"
#include "utils/log.h"
#include "utils/nsurl.h"
#include "macos9.h"
#include "macsurf_config.h"	/* MACSURF_HOME_URL (macos9.h only references it) */
#include "macsurf_debug.h"

#ifdef __MACOS9__
#include <Carbon.h>
#endif

/* ====================================================================
 * Boot baseline - the values main.c used to hardcode. Kept in ONE
 * function so the Defaults button and the factory first-run path are
 * provably identical.
 */

void macos9_prefs_apply_defaults(void)
{
	/* fixes78: the image content handler (QuickTime Graphics
	 * Importers) is registered in macos9_image.c; enable image
	 * fetches so <img> elements trigger network fetches. */
	nsoption_set_bool(foreground_images, true);
	nsoption_set_bool(background_images, true);
	/* Enable author CSS so inline <style>/<link> rules apply. */
	nsoption_set_bool(author_level_css, true);
	/* fixes319 (#115-#121) - inline <script> execution. NetSurf core
	 * defaults to false; without this html_script_exec returns early
	 * and the JS bridge is dead code from core's perspective. */
	nsoption_set_bool(enable_javascript, true);
	/* fixes1115b (#265) - <select> dropdown menus. The core
	 * form-control <select> handler is gated on this option; without
	 * it <select> elements render as empty rectangles. The Amiga and
	 * framebuffer frontends set this true; the macos9 frontend never
	 * did. */
	nsoption_set_bool(core_select_menu, true);
	/* fixes91: raise concurrent-fetch caps (NetSurf defaults are
	 * max_fetchers=24 / max_fetchers_per_host=5). With our HTTP
	 * fetcher's MFS_INIT-at-setup state machine, slots stay non-IDLE
	 * past the point NetSurf's fetch_ring drains, so the caps must
	 * never bite or the stub fetcher hangs. */
	nsoption_set_int(max_fetchers, 128);
	/* fixes232: drop the per-host cap from 16 to 4 so the HTTPS
	 * keep-alive pool (fixes231) actually catches reuses - only the
	 * first 4 fetches per host are cold handshakes. */
	nsoption_set_int(max_fetchers_per_host, 4);
	/* fixes106/160d/430/460-463/731 - memory cache size. History:
	 * 2MB cap on 16MB partitions; 32MB on the 194MB partition; 4MB
	 * after heap exhaustion; 0 while chasing the blank-page bug;
	 * restored to 32MB once #207 was root-caused to the
	 * pointer-ceiling guards, not the cache. */
	nsoption_set_int(memory_cache_size, 32 * 1024 * 1024);
}

/* nsoption_init callback: mutates the DEFAULT table (nsoptions is
 * redirected onto defs for the duration of the call - see
 * nsoption.c nsoption_init). Must return NSERROR_OK. */
nserror macos9_prefs_set_defaults(struct nsoption_s *defs)
{
	(void)defs;
	macos9_prefs_apply_defaults();
	return NSERROR_OK;
}

/* ====================================================================
 * Persistence: "Volume:...:MacSurfData:MacSurf Preferences".
 */

#define PREFS_LEAF "MacSurf Preferences"

/* Build the absolute HFS path to the prefs file. 0 on success. The
 * ':MacSurfData:MacSurf Preferences' colon-relative path does NOT
 * round-trip through MSL fopen (proven on cookies, fixes838) - the
 * FSSpec-based absolute path does. */
static int prefs_fullpath(char *out, long cap)
{
#ifdef __MACOS9__
	short vRef;
	long dirID;
	FSSpec spec;
	OSErr err;
	unsigned char fname[32];
	size_t nlen;
	out[0] = '\0';
	if (macos9_data_dir_get(NULL, &vRef, &dirID) != noErr) return -1;
	nlen = strlen(PREFS_LEAF);
	if (nlen > 31) nlen = 31;
	fname[0] = (unsigned char)nlen;
	memcpy(fname + 1, PREFS_LEAF, nlen);
	err = FSMakeFSSpec(vRef, dirID, fname, &spec);
	/* fnfErr = file not created yet, but the spec is valid for
	 * fopen("w") - exactly the cookie-jar pattern. */
	if (err != noErr && err != fnfErr) return -1;
	if (macos9_fsspec_to_path(&spec, out, cap) != 0) return -1;
	return 0;
#else
	/* Linux syntax-check / harness builds: plain cwd file. */
	(void)cap;
	strcpy(out, "macsurf_prefs.txt");
	return 0;
#endif
}

/* Load the persisted prefs file over the current option table (the
 * boot baseline). Missing file = first run = defaults stand. Called
 * from main.c right after nsoption_init, BEFORE netsurf_init, so the
 * fetchers/llcache see user values from the very first fetch. */
void macos9_prefs_load(void)
{
	char path[1024];
	nserror rc;
	if (prefs_fullpath(path, (long)sizeof path) != 0) {
		macsurf_debug_log_writef("LIFE prefsload no-path");
		return;
	}
	rc = nsoption_read(path, NULL);
	macsurf_debug_log_writef("LIFE prefsload rc=%d path=%s",
		(int)rc, path);
}

/* fixes1189 - log every option that differs from the compiled-in default,
 * right after boot loads the persisted prefs file. A stale/experimental
 * "MacSurf Preferences" file is otherwise INDISTINGUISHABLE in the debug
 * log from a real code regression: everything downstream (cascade, JS,
 * image fetch) just quietly does what the option says, with no marker
 * that the option itself isn't what the defaults say it should be. This
 * cost a full regression investigation once (author_level_css and
 * enable_javascript both silently OFF from a leftover test-session
 * prefs file, restored code just doing its job of loading it) before
 * the file was found by hand. One line per delta, LIFE-prefixed so it
 * survives the failures-only gate; a clean "no deltas" run logs exactly
 * one summary line so the check is a single grep either way. */
void macos9_prefs_log_deltas(void)
{
	int i;
	int n = 0;

	if (nsoptions == NULL || nsoptions_default == NULL) {
		macsurf_debug_log_writef("LIFE prefsdelta unavailable (no table)");
		return;
	}

	for (i = 0; i < NSOPTION_LISTEND; i++) {
		struct nsoption_s *o = &nsoptions[i];
		struct nsoption_s *d = &nsoptions_default[i];

		if (o->type != d->type) continue;

		switch (o->type) {
		case OPTION_BOOL:
			if (o->value.b != d->value.b) {
				macsurf_debug_log_writef(
					"LIFE prefsdelta %s=%d default=%d",
					o->key, (int)o->value.b, (int)d->value.b);
				n++;
			}
			break;
		case OPTION_INTEGER:
			if (o->value.i != d->value.i) {
				macsurf_debug_log_writef(
					"LIFE prefsdelta %s=%d default=%d",
					o->key, o->value.i, d->value.i);
				n++;
			}
			break;
		case OPTION_UINT:
			if (o->value.u != d->value.u) {
				macsurf_debug_log_writef(
					"LIFE prefsdelta %s=%ld default=%ld",
					o->key, (long)o->value.u, (long)d->value.u);
				n++;
			}
			break;
		case OPTION_COLOUR:
			if (o->value.c != d->value.c) {
				macsurf_debug_log_writef(
					"LIFE prefsdelta %s=%ld default=%ld",
					o->key, (long)o->value.c, (long)d->value.c);
				n++;
			}
			break;
		default:
			/* OPTION_STRING - not compared; heap-string diffs are
			 * lower stakes here (the regression class this guards
			 * against is silently-disabled features, which are
			 * bool/int switches, not string values). */
			break;
		}
	}

	macsurf_debug_log_writef("LIFE prefsdelta summary n=%d", n);
}

/* Persist only the user's deltas vs the default table (nsoption_write
 * emits just the CHANGED options). */
void macos9_prefs_save(void)
{
	char path[1024];
	nserror rc;
	if (prefs_fullpath(path, (long)sizeof path) != 0) {
		macsurf_debug_log_writef("LIFE prefssave no-path");
		return;
	}
	rc = nsoption_write(path, NULL, NULL);
	macsurf_debug_log_writef("LIFE prefssave rc=%d path=%s",
		(int)rc, path);
}

/* ====================================================================
 * Home page - the user's homepage_url option, falling back to the
 * built-in MACSURF_HOME_URL (mactrove.com). This is the single source
 * of truth for all four home-URL sites: File > New Window, the
 * deferred launch-home load, the Home toolbar button and the URL
 * bar's initial text (main.c:408/1607, window.c:1223/1479).
 */

const char *macos9_home_url(void)
{
	if (nsoptions != NULL) {
		const char *h = nsoption_charp(homepage_url);
		if (h != NULL && h[0] != '\0') return h;
	}
	return MACSURF_HOME_URL;
}

/* Reflow every open browser window so live settings (font sizes,
 * fetcher caps, image toggles) take effect immediately on OK. */
void macos9_prefs_apply_live(void)
{
	struct gui_window *g;
	for (g = macos9_window_list_head(); g != NULL; g = g->next) {
		macos9_window_request_reformat(g);
		macos9_window_invalidate_all(g);
	}
}

#ifdef __MACOS9__

/* ====================================================================
 * Preferences window - programmatic Carbon controls.
 */

#define PREFS_W_W 460
#define PREFS_W_H 390
#define PREFS_BANNER_H 34
#define PREFS_PANEL_TOP 40
#define PREFS_PANEL_BOT 330

enum {
	PREFS_CAT_GENERAL = 0,
	PREFS_CAT_APPEAR,
	PREFS_CAT_CONTENT,
	PREFS_CAT_PRIVACY,
	PREFS_CAT_NETWORK,
	PREFS_CAT_COUNT
};

/* Popup-button value tables (font_size is in 0.1pt - 120 = 12pt). */
struct prefs_popup_def {
	const char **labels;
	const int *values;
	int count;
};

static const char *s_lbl_cat[] = {
	"General", "Appearance", "Content", "Privacy", "Network"
};

static const char *s_lbl_font[] = {
	"9 pt", "10 pt", "11 pt", "12 pt", "13 pt", "14 pt", "15 pt",
	"16 pt", "18 pt", "20 pt", "24 pt"
};
static const int s_val_font[] = {
	90, 100, 110, 120, 130, 140, 150, 160, 180, 200, 240
};
static const struct prefs_popup_def s_popup_font = {
	s_lbl_font, s_val_font, 11
};

static const char *s_lbl_minfont[] = {
	"8 pt", "9 pt", "10 pt", "11 pt", "12 pt", "14 pt", "16 pt",
	"20 pt"
};
static const int s_val_minfont[] = { 80, 90, 100, 110, 120, 140, 160, 200 };
static const struct prefs_popup_def s_popup_minfont = {
	s_lbl_minfont, s_val_minfont, 8
};

static const char *s_lbl_fetch[] = { "4", "8", "16", "24", "32", "48", "64", "128" };
static const int s_val_fetch[] = { 4, 8, 16, 24, 32, 48, 64, 128 };
static const struct prefs_popup_def s_popup_fetch = {
	s_lbl_fetch, s_val_fetch, 8
};

static const char *s_lbl_perhost[] = { "1", "2", "3", "4", "6", "8", "12", "16" };
static const int s_val_perhost[] = { 1, 2, 3, 4, 6, 8, 12, 16 };
static const struct prefs_popup_def s_popup_perhost = {
	s_lbl_perhost, s_val_perhost, 8
};

/* One prefs window at a time; Cmd-, while open just brings it up. */
static WindowRef g_prefs_open_win = NULL;

struct prefs_win {
	WindowRef win;
	/* category */
	ControlRef pp_cat;
	MenuHandle m_cat;
	int cat;
	/* General */
	TEHandle te_home;
	TEHandle te_ww;
	TEHandle te_wh;
	TEHandle active_te;
	/* Appearance */
	ControlRef pp_font;
	MenuHandle m_font;
	ControlRef pp_minfont;
	MenuHandle m_minfont;
	ControlRef ck_fg;
	ControlRef ck_bg;
	ControlRef ck_anim;
	/* Content */
	ControlRef ck_js;
	ControlRef ck_css;
	ControlRef ck_ads;
	ControlRef ck_popups;
	/* Privacy */
	ControlRef ck_dnt;
	ControlRef ck_ref;
	ControlRef ck_cookies;
	ControlRef btn_cache;
	ControlRef btn_hist;
	/* Network */
	ControlRef pp_fetch;
	MenuHandle m_fetch;
	ControlRef pp_perhost;
	MenuHandle m_perhost;
	/* bottom row */
	ControlRef btn_defaults;
	ControlRef btn_cancel;
	ControlRef btn_ok;
};

/* Layout. Rect order is {top, left, bottom, right}. */
static const Rect s_btn_ok_rect      = { 344, 336, 368, 452 };
static const Rect s_btn_cancel_rect  = { 344, 236, 368, 316 };
static const Rect s_btn_defaults_rect= { 344, 108, 368, 188 };
static const Rect s_pp_cat_rect      = { 44, 340, 66, 440 };
static const Rect s_te_home_rect     = { 84, 100, 106, 444 };
static const Rect s_te_ww_rect       = { 122, 150, 144, 210 };
static const Rect s_te_wh_rect       = { 152, 150, 174, 210 };
static const Rect s_pp_font_rect     = { 84, 150, 106, 260 };
static const Rect s_pp_minfont_rect  = { 114, 150, 136, 260 };
static const Rect s_pp_fetch_rect    = { 84, 150, 106, 260 };
static const Rect s_pp_perhost_rect  = { 114, 150, 136, 260 };
static const Rect s_ck_fg_rect       = { 84, 24, 106, 340 };
static const Rect s_ck_bg_rect       = { 114, 24, 136, 340 };
static const Rect s_ck_anim_rect     = { 144, 24, 166, 340 };
static const Rect s_ck_js_rect       = { 84, 24, 106, 340 };
static const Rect s_ck_css_rect      = { 114, 24, 136, 340 };
static const Rect s_ck_ads_rect      = { 144, 24, 166, 340 };
static const Rect s_ck_popups_rect   = { 174, 24, 196, 340 };
static const Rect s_ck_dnt_rect      = { 84, 24, 106, 340 };
static const Rect s_ck_ref_rect      = { 114, 24, 136, 340 };
static const Rect s_ck_cookies_rect  = { 144, 24, 166, 340 };
static const Rect s_btn_cache_rect   = { 206, 24, 230, 140 };
static const Rect s_btn_hist_rect    = { 206, 148, 230, 256 };

/* Popup menu IDs - clear of the menu bar (128-134), the bookmark
 * submenus (200-231) and the bookmark move popup (250). */
#define PREFS_MENU_ID_CAT     260
#define PREFS_MENU_ID_FONT    261
#define PREFS_MENU_ID_MINFONT 262
#define PREFS_MENU_ID_FETCH   263
#define PREFS_MENU_ID_PERHOST 264

static void c_to_pstring(const char *src, unsigned char *dest)
{
	size_t n = strlen(src);
	if (n > 255) n = 255;
	dest[0] = (unsigned char)n;
	memcpy(dest + 1, src, n);
}

/* Copy of chrome_vgrad (static in macos9_chrome_extras.c): vertical
 * per-row gradient for the banner. */
static void prefs_vgrad(const Rect *r, int r0, int g0, int b0,
		int r1, int g1, int b1)
{
	short y;
	short h = (short)(r->bottom - r->top);
	RGBColor c;
	Rect ln;
	if (h <= 0) return;
	ln.left = r->left;
	ln.right = r->right;
	for (y = 0; y < h; y++) {
		int rv = r0 + (r1 - r0) * y / h;
		int gv = g0 + (g1 - g0) * y / h;
		int bv = b0 + (b1 - b0) * y / h;
		c.red   = (unsigned short)((rv << 8) | rv);
		c.green = (unsigned short)((gv << 8) | gv);
		c.blue  = (unsigned short)((bv << 8) | bv);
		RGBForeColor(&c);
		ln.top = (short)(r->top + y);
		ln.bottom = (short)(ln.top + 1);
		PaintRect(&ln);
	}
}

/* Three tiny slider glyphs in the banner (QuickDraw only - no PNG
 * resource needed). */
static void prefs_slider_icon(short left, short top)
{
	RGBColor shade;
	RGBColor white;
	short i;
	shade.red = 0x8C8C; shade.green = 0x5A5A; shade.blue = 0x1010;
	white.red = white.green = white.blue = 0xFFFF;
	for (i = 0; i < 3; i++) {
		Rect tr;
		Rect kn;
		SetRect(&tr, left, (short)(top + i * 9),
			(short)(left + 24), (short)(top + i * 9 + 4));
		RGBForeColor(&shade);
		PaintRoundRect(&tr, 4, 4);
		SetRect(&kn, (short)(left + 2 + i * 4), (short)(top + i * 9 - 2),
			(short)(left + 2 + i * 4 + 8), (short)(top + i * 9 + 6));
		RGBForeColor(&white);
		PaintRoundRect(&kn, 6, 6);
	}
}

/* Copy of chrome_mgr_header minus the PNG icon (replaced by
 * prefs_slider_icon): gold gradient band + dark amber accent line +
 * white bold title. */
static void prefs_banner(const Rect *content, const char *title)
{
	Rect band;
	Rect ln;
	RGBColor saved_fg;
	RGBColor white;
	RGBColor accent;
	white.red = white.green = white.blue = 0xFFFF;
	accent.red = 0x8C8C; accent.green = 0x5A5A; accent.blue = 0x1010;
	band.left = content->left;
	band.right = content->right;
	band.top = content->top;
	band.bottom = (short)(content->top + PREFS_BANNER_H);
	GetForeColor(&saved_fg);
	prefs_vgrad(&band, 0xF0, 0xA8, 0x40, 0xD2, 0x82, 0x1E);
	ln.left = band.left; ln.right = band.right;
	ln.top = (short)(band.bottom - 1); ln.bottom = band.bottom;
	RGBForeColor(&accent); PaintRect(&ln);
	prefs_slider_icon((short)(band.left + 14), (short)(band.top + 11));
	RGBForeColor(&white);
	TextFont(1); TextFace(bold); TextSize(15);
	MoveTo(52, (short)(band.top + 23));
	DrawText(title, 0, (short)strlen(title));
	TextFace(normal);
	RGBForeColor(&saved_fg);
}

/* One-shot macro: build a Pascal-string titled control.
 * CW8 requires the procID to be a literal enum constant at each call site
 * (a variable, even cast, fails the anonymous-enum type check) so there
 * is no shared helper - each caller inlines the two-step conversion. */
#define PS_CTRL(ctrl, win, rect, ctitle, proc) \
	do { unsigned char _ps[256]; c_to_pstring((ctitle), _ps); \
	     (ctrl) = NewControl((win), (rect), _ps, 1, 0, 0, 0, (proc), 0); \
	} while(0)

static MenuHandle prefs_popup_menu(const struct prefs_popup_def *def,
		short id)
{
	MenuHandle m;
	int i;
	m = NewMenu(id, "\p");
	if (m == NULL) return NULL;
	for (i = 0; i < def->count; i++) {
		unsigned char pstr[256];
		c_to_pstring(def->labels[i], pstr);
		AppendMenu(m, pstr);
	}
	return m;
}

static MenuHandle prefs_cat_menu(void)
{
	MenuHandle m;
	int i;
	m = NewMenu(PREFS_MENU_ID_CAT, "\pShow:");
	if (m == NULL) return NULL;
	for (i = 0; i < PREFS_CAT_COUNT; i++) {
		unsigned char pstr[256];
		c_to_pstring(s_lbl_cat[i], pstr);
		AppendMenu(m, pstr);
	}
	return m;
}

/* Best-effort attach of the popup's menu handle; without it the CDEF
 * has nothing to draw as the value text. Standard since the
 * Appearance Manager (Mac OS 8.5). */
static void prefs_popup_attach(ControlRef c, MenuHandle m)
{
	(void)SetControlData(c, kControlEntireControl,
		kControlPopupButtonMenuHandleTag, sizeof(m), &m);
}

static void prefs_set_val(ControlRef c, int v)
{
	if (c != NULL) SetControlValue(c, (short)v);
}

static int prefs_get_val(ControlRef c)
{
	if (c == NULL) return 0;
	return GetControlValue(c);
}

static void prefs_set_vis(ControlRef c, int show)
{
	if (c == NULL) return;
	if (show) ShowControl(c);
	else HideControl(c);
}

static void prefs_disp_ctrl(ControlRef c)
{
	if (c != NULL) DisposeControl(c);
}

static void prefs_disp_menu(MenuHandle m)
{
	if (m != NULL) DisposeMenu(m);
}

/* Nearest entry index (1-based) for a value, so popups always show a
 * valid item even for values that fell between the offered steps. */
static short prefs_popup_item(const struct prefs_popup_def *def, int value)
{
	int i;
	int best = 1;
	int bestd = 0x7FFFFFFF;
	for (i = 0; i < def->count; i++) {
		int d = def->values[i] - value;
		if (d < 0) d = -d;
		if (d < bestd) { bestd = d; best = i + 1; }
	}
	return (short)best;
}

static int prefs_popup_get(ControlRef c, const struct prefs_popup_def *def)
{
	int idx = prefs_get_val(c) - 1;
	if (idx < 0) idx = 0;
	if (idx >= def->count) idx = def->count - 1;
	return def->values[idx];
}

/* Show/hide the per-category control set. TEs are not controls: they
 * are simply not drawn/hit-tested off their panel. */
static void prefs_panel_vis(struct prefs_win *pw)
{
	prefs_set_vis(pw->ck_fg,     pw->cat == PREFS_CAT_APPEAR);
	prefs_set_vis(pw->ck_bg,     pw->cat == PREFS_CAT_APPEAR);
	prefs_set_vis(pw->ck_anim,   pw->cat == PREFS_CAT_APPEAR);
	prefs_set_vis(pw->pp_font,   pw->cat == PREFS_CAT_APPEAR);
	prefs_set_vis(pw->pp_minfont,pw->cat == PREFS_CAT_APPEAR);
	prefs_set_vis(pw->ck_js,     pw->cat == PREFS_CAT_CONTENT);
	prefs_set_vis(pw->ck_css,    pw->cat == PREFS_CAT_CONTENT);
	prefs_set_vis(pw->ck_ads,    pw->cat == PREFS_CAT_CONTENT);
	prefs_set_vis(pw->ck_popups, pw->cat == PREFS_CAT_CONTENT);
	prefs_set_vis(pw->ck_dnt,    pw->cat == PREFS_CAT_PRIVACY);
	prefs_set_vis(pw->ck_ref,    pw->cat == PREFS_CAT_PRIVACY);
	prefs_set_vis(pw->ck_cookies,pw->cat == PREFS_CAT_PRIVACY);
	prefs_set_vis(pw->btn_cache, pw->cat == PREFS_CAT_PRIVACY);
	prefs_set_vis(pw->btn_hist,  pw->cat == PREFS_CAT_PRIVACY);
	prefs_set_vis(pw->pp_fetch,  pw->cat == PREFS_CAT_NETWORK);
	prefs_set_vis(pw->pp_perhost,pw->cat == PREFS_CAT_NETWORK);
}

static void prefs_set_cat(struct prefs_win *pw, int cat)
{
	Rect r;
	if (cat < 0 || cat >= PREFS_CAT_COUNT) return;
	if (cat == pw->cat) return;
	pw->cat = cat;
	prefs_set_val(pw->pp_cat, cat + 1);
	prefs_panel_vis(pw);
	SetRect(&r, 0, PREFS_PANEL_TOP, PREFS_W_W, PREFS_PANEL_BOT);
	InvalWindowRect(pw->win, &r);
}

/* Push every control's current state into the live option table. */
static void prefs_apply_from_ui(struct prefs_win *pw)
{
	/* home page: empty field -> NULL -> built-in home */
	if (pw->te_home != NULL) {
		long n = pw->te_home[0]->teLength;
		char *buf;
		if (n < 0) n = 0;
		buf = (char *)malloc((size_t)n + 1);
		if (buf != NULL) {
			if (n > 0)
				memcpy(buf, pw->te_home[0]->hText, (size_t)n);
			buf[n] = '\0';
			/* takes ownership; empty string becomes NULL */
			nsoption_set_charp(homepage_url, buf);
		}
	}
	/* new-window size (0 = automatic; clamps live in window.c) */
	{
		long n;
		char num[16];
		int v;
		if (pw->te_ww != NULL) {
			n = pw->te_ww[0]->teLength;
			if (n <= 0 || n >= (long)sizeof num) v = nsoption_int(window_width);
			else {
				memcpy(num, pw->te_ww[0]->hText, (size_t)n);
				num[n] = '\0';
				v = atoi(num);
			}
			if (v < 0) v = 0;
			if (v > 4096) v = 4096;
			nsoption_set_int(window_width, v);
		}
		if (pw->te_wh != NULL) {
			n = pw->te_wh[0]->teLength;
			if (n <= 0 || n >= (long)sizeof num) v = nsoption_int(window_height);
			else {
				memcpy(num, pw->te_wh[0]->hText, (size_t)n);
				num[n] = '\0';
				v = atoi(num);
			}
			if (v < 0) v = 0;
			if (v > 4096) v = 4096;
			nsoption_set_int(window_height, v);
		}
	}
	/* checkboxes */
	nsoption_set_bool(foreground_images,
		prefs_get_val(pw->ck_fg) != 0);
	nsoption_set_bool(background_images,
		prefs_get_val(pw->ck_bg) != 0);
	nsoption_set_bool(animate_images,
		prefs_get_val(pw->ck_anim) != 0);
	nsoption_set_bool(enable_javascript,
		prefs_get_val(pw->ck_js) != 0);
	nsoption_set_bool(author_level_css,
		prefs_get_val(pw->ck_css) != 0);
	nsoption_set_bool(block_advertisements,
		prefs_get_val(pw->ck_ads) != 0);
	nsoption_set_bool(disable_popups,
		prefs_get_val(pw->ck_popups) != 0);
	nsoption_set_bool(do_not_track,
		prefs_get_val(pw->ck_dnt) != 0);
	nsoption_set_bool(send_referer,
		prefs_get_val(pw->ck_ref) != 0);
	nsoption_set_bool(accept_cookies,
		prefs_get_val(pw->ck_cookies) != 0);
	/* popups */
	nsoption_set_int(font_size,
		prefs_popup_get(pw->pp_font, &s_popup_font));
	nsoption_set_int(font_min_size,
		prefs_popup_get(pw->pp_minfont, &s_popup_minfont));
	nsoption_set_int(max_fetchers,
		prefs_popup_get(pw->pp_fetch, &s_popup_fetch));
	nsoption_set_int(max_fetchers_per_host,
		prefs_popup_get(pw->pp_perhost, &s_popup_perhost));
}

/* Read the live option table into every control (open and Defaults). */
static void prefs_load_values(struct prefs_win *pw)
{
	char num[32];
	const char *home = macos9_home_url();
	if (pw->te_home != NULL) {
		TESetText(home, (long)strlen(home), pw->te_home);
		TESetSelect(0, 32767, pw->te_home);
	}
	if (pw->te_ww != NULL) {
		sprintf(num, "%ld", (long)nsoption_int(window_width));
		TESetText(num, (long)strlen(num), pw->te_ww);
		TESetSelect(0, 32767, pw->te_ww);
	}
	if (pw->te_wh != NULL) {
		sprintf(num, "%ld", (long)nsoption_int(window_height));
		TESetText(num, (long)strlen(num), pw->te_wh);
		TESetSelect(0, 32767, pw->te_wh);
	}
	prefs_set_val(pw->pp_font,
		prefs_popup_item(&s_popup_font, nsoption_int(font_size)));
	prefs_set_val(pw->pp_minfont,
		prefs_popup_item(&s_popup_minfont, nsoption_int(font_min_size)));
	prefs_set_val(pw->pp_fetch,
		prefs_popup_item(&s_popup_fetch, nsoption_int(max_fetchers)));
	prefs_set_val(pw->pp_perhost,
		prefs_popup_item(&s_popup_perhost,
			nsoption_int(max_fetchers_per_host)));
	prefs_set_val(pw->ck_fg, nsoption_bool(foreground_images) ? 1 : 0);
	prefs_set_val(pw->ck_bg, nsoption_bool(background_images) ? 1 : 0);
	prefs_set_val(pw->ck_anim, nsoption_bool(animate_images) ? 1 : 0);
	prefs_set_val(pw->ck_js, nsoption_bool(enable_javascript) ? 1 : 0);
	prefs_set_val(pw->ck_css, nsoption_bool(author_level_css) ? 1 : 0);
	prefs_set_val(pw->ck_ads, nsoption_bool(block_advertisements) ? 1 : 0);
	prefs_set_val(pw->ck_popups, nsoption_bool(disable_popups) ? 1 : 0);
	prefs_set_val(pw->ck_dnt, nsoption_bool(do_not_track) ? 1 : 0);
	prefs_set_val(pw->ck_ref, nsoption_bool(send_referer) ? 1 : 0);
	prefs_set_val(pw->ck_cookies, nsoption_bool(accept_cookies) ? 1 : 0);
	prefs_set_val(pw->pp_cat, pw->cat + 1);
}

static void prefs_paint(struct prefs_win *pw)
{
	Rect content;
	Rect panel;
	Rect r;
	RGBColor saved;
	RGBColor black_c;
	black_c.red = black_c.green = black_c.blue = 0;
	SetRect(&content, 0, 0, PREFS_W_W, PREFS_W_H);
	SetRect(&panel, 0, PREFS_PANEL_TOP, PREFS_W_W, PREFS_PANEL_BOT);
	EraseRect(&panel);
	prefs_banner(&content, "Preferences");
	GetForeColor(&saved);
	RGBForeColor(&black_c);
	TextFont(1);
	TextSize(12);
	MoveTo(300, 62); DrawString("\pShow:");
	switch (pw->cat) {
	case PREFS_CAT_GENERAL:
		MoveTo(16, 96); DrawString("\pHome page:");
		MoveTo(16, 134); DrawString("\pNew window width:");
		MoveTo(16, 164); DrawString("\pNew window height:");
		MoveTo(216, 164); DrawString("\p(0 = automatic)");
		break;
	case PREFS_CAT_APPEAR:
		MoveTo(16, 96); DrawString("\pDefault font size:");
		MoveTo(16, 126); DrawString("\pMinimum font size:");
		break;
	case PREFS_CAT_CONTENT:
		break;
	case PREFS_CAT_PRIVACY:
		break;
	case PREFS_CAT_NETWORK:
		MoveTo(16, 96); DrawString("\pMaximum simultaneous fetchers:");
		MoveTo(16, 126); DrawString("\pMaximum fetchers per host:");
		break;
	default:
		break;
	}
	RGBForeColor(&saved);
	DrawControls(pw->win);
	if (pw->cat == PREFS_CAT_GENERAL) {
		if (pw->te_home != NULL) {
			r = s_te_home_rect; FrameRect(&r); TEUpdate(&r, pw->te_home);
		}
		if (pw->te_ww != NULL) {
			r = s_te_ww_rect; FrameRect(&r); TEUpdate(&r, pw->te_ww);
		}
		if (pw->te_wh != NULL) {
			r = s_te_wh_rect; FrameRect(&r); TEUpdate(&r, pw->te_wh);
		}
	}
}

/* TrackControl + PopUpMenuSelect for a value popup. */
static void prefs_do_popup(struct prefs_win *pw, ControlRef c, MenuHandle m,
		Point lp)
{
	short part;
	short cur;
	Rect cr;
	Point gpt;
	long chosen;
	if (c == NULL || m == NULL) return;
	part = TrackControl(c, lp, NULL);
	if (part == 0) return;
	cur = GetControlValue(c);
	GetControlBounds(c, &cr);
	gpt.h = cr.left;
	gpt.v = cr.top;
	LocalToGlobal(&gpt);
	chosen = PopUpMenuSelect(m, gpt.v, gpt.h, cur);
	if (chosen != 0) {
		SetControlValue(c, (short)(chosen & 0xFFFF));
		Draw1Control(c);
	}
}

/* The category popup also switches the visible panel. */
static void prefs_do_popup_cat(struct prefs_win *pw, Point lp)
{
	short part;
	short cur;
	Rect cr;
	Point gpt;
	long chosen;
	if (pw->pp_cat == NULL || pw->m_cat == NULL) return;
	part = TrackControl(pw->pp_cat, lp, NULL);
	if (part == 0) return;
	cur = GetControlValue(pw->pp_cat);
	GetControlBounds(pw->pp_cat, &cr);
	gpt.h = cr.left;
	gpt.v = cr.top;
	LocalToGlobal(&gpt);
	chosen = PopUpMenuSelect(pw->m_cat, gpt.v, gpt.h, cur);
	if (chosen != 0) {
		SetControlValue(pw->pp_cat, (short)(chosen & 0xFFFF));
		prefs_set_cat(pw, (int)(chosen & 0xFFFF) - 1);
	}
}

static void prefs_check_toggle(ControlRef c, Point lp)
{
	short part;
	if (c == NULL) return;
	part = TrackControl(c, lp, NULL);
	if (part != 0) Draw1Control(c);
}

static void prefs_te_focus(struct prefs_win *pw, TEHandle te, Point lp)
{
	if (te == NULL) return;
	if (pw->active_te != NULL && pw->active_te != te)
		TEDeactivate(pw->active_te);
	pw->active_te = te;
	TEActivate(te);
	TEClick(lp, false, te);
}

static void prefs_te_blur(struct prefs_win *pw)
{
	if (pw->active_te != NULL) TEDeactivate(pw->active_te);
	pw->active_te = NULL;
}

static void prefs_te_tab(struct prefs_win *pw)
{
	TEHandle next;
	if (pw->cat != PREFS_CAT_GENERAL) return;
	next = NULL;
	if (pw->active_te == pw->te_home) next = pw->te_ww;
	else if (pw->active_te == pw->te_ww) next = pw->te_wh;
	else next = pw->te_home;
	if (next == NULL) next = pw->te_ww;
	if (next == NULL) next = pw->te_wh;
	if (next == NULL) return;
	if (pw->active_te != NULL && pw->active_te != next)
		TEDeactivate(pw->active_te);
	pw->active_te = next;
	TEActivate(next);
	TESetSelect(0, 32767, next);
}

/* mouseDown on our content: returns 1 when the window should close. */
static int prefs_click(struct prefs_win *pw, Point lp)
{
	Rect r;
	short part;

	if (PtInRect(lp, &s_btn_ok_rect)) {
		part = TrackControl(pw->btn_ok, lp, NULL);
		if (part != 0) {
			prefs_apply_from_ui(pw);
			macos9_prefs_save();
			macos9_prefs_apply_live();
			return 1;
		}
		return 0;
	}
	if (PtInRect(lp, &s_btn_cancel_rect)) {
		part = TrackControl(pw->btn_cancel, lp, NULL);
		if (part != 0) return 1;
		return 0;
	}
	if (PtInRect(lp, &s_btn_defaults_rect)) {
		part = TrackControl(pw->btn_defaults, lp, NULL);
		if (part != 0) {
			/* factory settings: re-apply the boot baseline into
			 * the live table and re-read every control. NOT
			 * written to disk until OK. */
			macos9_prefs_apply_defaults();
			prefs_load_values(pw);
			SetRect(&r, 0, 0, PREFS_W_W, PREFS_W_H);
			InvalWindowRect(pw->win, &r);
		}
		return 0;
	}
	if (PtInRect(lp, &s_pp_cat_rect)) {
		prefs_do_popup_cat(pw, lp);
		return 0;
	}

	switch (pw->cat) {
	case PREFS_CAT_GENERAL:
		if (PtInRect(lp, &s_te_home_rect)) {
			prefs_te_focus(pw, pw->te_home, lp); return 0;
		}
		if (PtInRect(lp, &s_te_ww_rect)) {
			prefs_te_focus(pw, pw->te_ww, lp); return 0;
		}
		if (PtInRect(lp, &s_te_wh_rect)) {
			prefs_te_focus(pw, pw->te_wh, lp); return 0;
		}
		break;
	case PREFS_CAT_APPEAR:
		if (PtInRect(lp, &s_ck_fg_rect)) {
			prefs_check_toggle(pw->ck_fg, lp); return 0;
		}
		if (PtInRect(lp, &s_ck_bg_rect)) {
			prefs_check_toggle(pw->ck_bg, lp); return 0;
		}
		if (PtInRect(lp, &s_ck_anim_rect)) {
			prefs_check_toggle(pw->ck_anim, lp); return 0;
		}
		if (PtInRect(lp, &s_pp_font_rect)) {
			prefs_do_popup(pw, pw->pp_font, pw->m_font, lp); return 0;
		}
		if (PtInRect(lp, &s_pp_minfont_rect)) {
			prefs_do_popup(pw, pw->pp_minfont, pw->m_minfont, lp); return 0;
		}
		break;
	case PREFS_CAT_CONTENT:
		if (PtInRect(lp, &s_ck_js_rect)) {
			prefs_check_toggle(pw->ck_js, lp); return 0;
		}
		if (PtInRect(lp, &s_ck_css_rect)) {
			prefs_check_toggle(pw->ck_css, lp); return 0;
		}
		if (PtInRect(lp, &s_ck_ads_rect)) {
			prefs_check_toggle(pw->ck_ads, lp); return 0;
		}
		if (PtInRect(lp, &s_ck_popups_rect)) {
			prefs_check_toggle(pw->ck_popups, lp); return 0;
		}
		break;
	case PREFS_CAT_PRIVACY:
		if (PtInRect(lp, &s_ck_dnt_rect)) {
			prefs_check_toggle(pw->ck_dnt, lp); return 0;
		}
		if (PtInRect(lp, &s_ck_ref_rect)) {
			prefs_check_toggle(pw->ck_ref, lp); return 0;
		}
		if (PtInRect(lp, &s_ck_cookies_rect)) {
			prefs_check_toggle(pw->ck_cookies, lp); return 0;
		}
		if (PtInRect(lp, &s_btn_cache_rect)) {
			part = TrackControl(pw->btn_cache, lp, NULL);
			if (part != 0) macos9_cache_clear_ui();
			return 0;
		}
		if (PtInRect(lp, &s_btn_hist_rect)) {
			part = TrackControl(pw->btn_hist, lp, NULL);
			if (part != 0) macos9_history_clear();
			return 0;
		}
		break;
	case PREFS_CAT_NETWORK:
		if (PtInRect(lp, &s_pp_fetch_rect)) {
			prefs_do_popup(pw, pw->pp_fetch, pw->m_fetch, lp); return 0;
		}
		if (PtInRect(lp, &s_pp_perhost_rect)) {
			prefs_do_popup(pw, pw->pp_perhost, pw->m_perhost, lp); return 0;
		}
		break;
	default:
		break;
	}

	/* clicked nothing: drop TE focus */
	prefs_te_blur(pw);
	return 0;
}

/* keyDown/autoKey: returns 1 when the window should close. */
static int prefs_key(struct prefs_win *pw, const EventRecord *ev)
{
	char ch = (char)(ev->message & charCodeMask);
	if (ev->modifiers & cmdKey) {
		if (ch == '.' || ch == 'w' || ch == 'W') return 1;
		return 0;  /* other command keys: ignore in this loop */
	}
	if (ch == 0x1B) return 1;  /* Esc = cancel */
	if (ch == '\r' || ch == 0x03) {
		prefs_apply_from_ui(pw);
		macos9_prefs_save();
		macos9_prefs_apply_live();
		return 1;
	}
	if (ch == 0x09) {  /* Tab cycles the three text fields */
		prefs_te_tab(pw);
		return 0;
	}
	if (pw->active_te != NULL) TEKey(ch, pw->active_te);
	return 0;
}

void macos9_prefs_show(void)
{
	struct prefs_win pw;
	GrafPtr saved_port;
	EventRecord ev;
	Rect wb;
	Str255 pt;
	int done = 0;

	memset(&pw, 0, sizeof pw);
	pw.cat = PREFS_CAT_GENERAL;

	/* one prefs window at a time */
	if (g_prefs_open_win != NULL) {
		SelectWindow(g_prefs_open_win);
		return;
	}

	SetRect(&wb, 120, 90, (short)(120 + PREFS_W_W), (short)(90 + PREFS_W_H));
	if (CreateNewWindow(kDocumentWindowClass, kWindowCloseBoxAttribute,
			&wb, &pw.win) != noErr || pw.win == NULL) {
		return;
	}
	g_prefs_open_win = pw.win;
	c_to_pstring("Preferences", pt);
	SetWTitle(pw.win, pt);
	SetWRefCon(pw.win, 0);  /* TENew dsMemWZErr safety (CLAUDE.md) */

	GetPort(&saved_port);
	SetPortWindowPort(pw.win);
	TextFont(1);
	TextSize(12);

	/* bottom row */
	PS_CTRL(pw.btn_defaults, pw.win, &s_btn_defaults_rect,
		"Defaults", kControlPushButtonProc);
	PS_CTRL(pw.btn_cancel, pw.win, &s_btn_cancel_rect,
		"Cancel", kControlPushButtonProc);
	PS_CTRL(pw.btn_ok, pw.win, &s_btn_ok_rect,
		"OK", kControlPushButtonProc);
	/* category picker */
	pw.m_cat = prefs_cat_menu();
	PS_CTRL(pw.pp_cat, pw.win, &s_pp_cat_rect,
		"", kControlPopupButtonProc);
	prefs_popup_attach(pw.pp_cat, pw.m_cat);
	/* Appearance */
	pw.m_font = prefs_popup_menu(&s_popup_font, PREFS_MENU_ID_FONT);
	PS_CTRL(pw.pp_font, pw.win, &s_pp_font_rect,
		"", kControlPopupButtonProc);
	prefs_popup_attach(pw.pp_font, pw.m_font);
	pw.m_minfont = prefs_popup_menu(&s_popup_minfont, PREFS_MENU_ID_MINFONT);
	PS_CTRL(pw.pp_minfont, pw.win, &s_pp_minfont_rect,
		"", kControlPopupButtonProc);
	prefs_popup_attach(pw.pp_minfont, pw.m_minfont);
	PS_CTRL(pw.ck_fg, pw.win, &s_ck_fg_rect,
		"Fetch foreground images", kControlCheckBoxProc);
	PS_CTRL(pw.ck_bg, pw.win, &s_ck_bg_rect,
		"Fetch background images", kControlCheckBoxProc);
	PS_CTRL(pw.ck_anim, pw.win, &s_ck_anim_rect,
		"Animate images", kControlCheckBoxProc);
	/* Content */
	PS_CTRL(pw.ck_js, pw.win, &s_ck_js_rect,
		"Enable JavaScript", kControlCheckBoxProc);
	PS_CTRL(pw.ck_css, pw.win, &s_ck_css_rect,
		"Apply author CSS", kControlCheckBoxProc);
	PS_CTRL(pw.ck_ads, pw.win, &s_ck_ads_rect,
		"Block advertisements", kControlCheckBoxProc);
	PS_CTRL(pw.ck_popups, pw.win, &s_ck_popups_rect,
		"Block pop-up windows", kControlCheckBoxProc);
	/* Privacy */
	PS_CTRL(pw.ck_dnt, pw.win, &s_ck_dnt_rect,
		"Send Do Not Track request", kControlCheckBoxProc);
	PS_CTRL(pw.ck_ref, pw.win, &s_ck_ref_rect,
		"Send Referer header", kControlCheckBoxProc);
	PS_CTRL(pw.ck_cookies, pw.win, &s_ck_cookies_rect,
		"Store and send cookies", kControlCheckBoxProc);
	PS_CTRL(pw.btn_cache, pw.win, &s_btn_cache_rect,
		"Clear Cache...", kControlPushButtonProc);
	PS_CTRL(pw.btn_hist, pw.win, &s_btn_hist_rect,
		"Clear History...", kControlPushButtonProc);
	/* Network */
	pw.m_fetch = prefs_popup_menu(&s_popup_fetch, PREFS_MENU_ID_FETCH);
	PS_CTRL(pw.pp_fetch, pw.win, &s_pp_fetch_rect,
		"", kControlPopupButtonProc);
	prefs_popup_attach(pw.pp_fetch, pw.m_fetch);
	pw.m_perhost = prefs_popup_menu(&s_popup_perhost, PREFS_MENU_ID_PERHOST);
	PS_CTRL(pw.pp_perhost, pw.win, &s_pp_perhost_rect,
		"", kControlPopupButtonProc);
	prefs_popup_attach(pw.pp_perhost, pw.m_perhost);
	/* General text fields (TENew uses the current port) */
	{
		Rect r = s_te_home_rect;
		pw.te_home = TENew(&r, &r);
	}
	{
		Rect r = s_te_ww_rect;
		pw.te_ww = TENew(&r, &r);
	}
	{
		Rect r = s_te_wh_rect;
		pw.te_wh = TENew(&r, &r);
	}

	prefs_load_values(&pw);
	prefs_panel_vis(&pw);
	ShowWindow(pw.win);
	SelectWindow(pw.win);
	if (pw.te_home != NULL) {
		pw.active_te = pw.te_home;
		TEActivate(pw.te_home);
	}

	while (!done) {
		WaitNextEvent(everyEvent, &ev, 30, NULL);
		switch (ev.what) {
		case mouseDown: {
			WindowRef which;
			short part;
			Point lp;
			part = FindWindow(ev.where, &which);
			if (which != pw.win) break;
			if (part == inDrag) {
				Rect db;
				BitMap sb;
				GetQDGlobalsScreenBits(&sb);
				db = sb.bounds;
				DragWindow(pw.win, ev.where, &db);
			} else if (part == inGoAway) {
				if (TrackGoAway(pw.win, ev.where)) done = 1;
			} else if (part == inContent) {
				lp = ev.where;
				GlobalToLocal(&lp);
				if (prefs_click(&pw, lp)) done = 1;
			}
			break;
		}
		case keyDown:
		case autoKey:
			if (prefs_key(&pw, &ev)) done = 1;
			break;
		case updateEvt:
			if ((WindowRef)ev.message == pw.win) {
				BeginUpdate(pw.win);
				prefs_paint(&pw);
				EndUpdate(pw.win);
			} else {
				/* fixes709 pattern - keep uncovered
				 * browser windows repainted. */
				extern void macos9_handle_update(const EventRecord *event);
				macos9_handle_update(&ev);
				SetPortWindowPort(pw.win);
			}
			break;
		case nullEvent:
			if (pw.active_te != NULL) TEIdle(pw.active_te);
			break;
		default:
			break;
		}
	}

	prefs_te_blur(&pw);
	prefs_disp_ctrl(pw.btn_defaults);
	prefs_disp_ctrl(pw.btn_cancel);
	prefs_disp_ctrl(pw.btn_ok);
	prefs_disp_ctrl(pw.pp_cat);
	prefs_disp_ctrl(pw.pp_font);
	prefs_disp_ctrl(pw.pp_minfont);
	prefs_disp_ctrl(pw.pp_fetch);
	prefs_disp_ctrl(pw.pp_perhost);
	prefs_disp_ctrl(pw.ck_fg);
	prefs_disp_ctrl(pw.ck_bg);
	prefs_disp_ctrl(pw.ck_anim);
	prefs_disp_ctrl(pw.ck_js);
	prefs_disp_ctrl(pw.ck_css);
	prefs_disp_ctrl(pw.ck_ads);
	prefs_disp_ctrl(pw.ck_popups);
	prefs_disp_ctrl(pw.ck_dnt);
	prefs_disp_ctrl(pw.ck_ref);
	prefs_disp_ctrl(pw.ck_cookies);
	prefs_disp_ctrl(pw.btn_cache);
	prefs_disp_ctrl(pw.btn_hist);
	prefs_disp_menu(pw.m_cat);
	prefs_disp_menu(pw.m_font);
	prefs_disp_menu(pw.m_minfont);
	prefs_disp_menu(pw.m_fetch);
	prefs_disp_menu(pw.m_perhost);
	if (pw.te_home != NULL) TEDispose(pw.te_home);
	if (pw.te_ww != NULL) TEDispose(pw.te_ww);
	if (pw.te_wh != NULL) TEDispose(pw.te_wh);
	DisposeWindow(pw.win);
	g_prefs_open_win = NULL;
	SetPort(saved_port);
}

#else /* !__MACOS9__ */

void macos9_prefs_show(void)
{
	/* Linux syntax-check / harness builds: nothing to show. */
}

#endif /* __MACOS9__ */
