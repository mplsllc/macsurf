/*
 * MacSurf - Preferences window + persistence (macos9_prefs.c)
 *
 * Provides user configuration and persistent options storage:
 *
 *   1. macos9_prefs_apply_defaults() / macos9_prefs_set_defaults()
 *      Establishes factory default baseline into the nsoptions_default
 *      table. Persistence stores only user deltas vs defaults.
 *
 *   2. macos9_prefs_load() / macos9_prefs_save()
 *      Reads/writes "MacSurf Preferences" in MacSurfData via nsoption_read
 *      and nsoption_write.
 *
 *   3. macos9_prefs_show()
 *      Programmatic native Carbon / Appearance Manager Preferences window.
 *      Organized into General, Web Content, Appearance, Privacy, and
 *      Advanced categories with descriptive explanations, reliable
 *      checkbox and popup tracking, draft/cancel semantics, and homepage
 *      helpers.
 *
 * C89 / CW8-clean: no inline, no // comments, declarations at top of block.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

#include "utils/ns_errors.h"
#include "utils/nsoption.h"
#include "utils/log.h"
#include "utils/nsurl.h"
#include "netsurf/browser_window.h"
#include "macos9.h"
#include "macsurf_config.h"	/* MACSURF_HOME_URL */
#include "macsurf_debug.h"

#ifdef __MACOS9__
#include <Carbon.h>
#endif

/* ====================================================================
 * Boot baseline - factory defaults table.
 * All user-adjustable options are reset to their intended compiled
 * baseline here so that Restore Defaults + OK cleanly clears any stale
 * deltas from the preferences file.
 */

void macos9_prefs_apply_defaults(void)
{
	/* Web Content defaults */
	nsoption_set_bool(foreground_images, true);
	nsoption_set_bool(background_images, true);
	nsoption_set_bool(animate_images, true);
	nsoption_set_bool(author_level_css, true);
	nsoption_set_bool(enable_javascript, true);
	nsoption_set_bool(block_advertisements, false);
	nsoption_set_bool(disable_popups, false);

	/* Appearance defaults (120 = 12pt = 16 CSS px; min = 85 = 8.5pt) */
	nsoption_set_int(font_size, 120);
	nsoption_set_int(font_min_size, 85);

	/* Privacy defaults */
	nsoption_set_bool(accept_cookies, true);
	nsoption_set_bool(send_referer, true);
	nsoption_set_bool(do_not_track, false);

	/* Network / Advanced defaults */
	nsoption_set_int(max_fetchers, 128);
	nsoption_set_int(max_fetchers_per_host, 16);
	nsoption_set_int(memory_cache_size, 32 * 1024 * 1024);

	/* Core form-controls */
	nsoption_set_bool(core_select_menu, true);

	/* General window geometry (0 = automatic default) */
	nsoption_set_int(window_width, 0);
	nsoption_set_int(window_height, 0);
}

/* nsoption_init callback: mutates the DEFAULT table (nsoptions is
 * redirected onto defs for the duration of the call). Must return NSERROR_OK. */
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

/* Build the absolute HFS path to the prefs file. 0 on success. */
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
	if (err != noErr && err != fnfErr) return -1;
	if (macos9_fsspec_to_path(&spec, out, cap) != 0) return -1;
	return 0;
#else
	(void)cap;
	strcpy(out, "macsurf_prefs.txt");
	return 0;
#endif
}

/* Load persisted preferences file over the boot defaults. */
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

/* Log every option that differs from the compiled-in default,
 * followed by a single definitive LIFE PREF render line. */
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
			break;
		}
	}

	macsurf_debug_log_writef("LIFE prefsdelta summary n=%d", n);
	macsurf_debug_log_writef(
		"LIFE PREF render css_author=%d fg_images=%d bg_images=%d js=%d home=%s",
		(int)nsoption_bool(author_level_css),
		(int)nsoption_bool(foreground_images),
		(int)nsoption_bool(background_images),
		(int)nsoption_bool(enable_javascript),
		macos9_home_url());
}

/* Persist only user deltas vs default table. */
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
 * Home page - authoritative accessor.
 */

const char *macos9_home_url(void)
{
	if (nsoptions != NULL) {
		const char *h = nsoption_charp(homepage_url);
		if (h != NULL && h[0] != '\0') return h;
	}
	return MACSURF_HOME_URL;
}

/* Reflow open browser windows for live visual settings. */
void macos9_prefs_apply_live(void)
{
	struct gui_window *g;
	for (g = macos9_window_list_head(); g != NULL; g = g->next_global) {
		macos9_window_request_reformat(g);
		macos9_window_invalidate_all(g);
	}
}

#ifdef __MACOS9__

/* ====================================================================
 * Preferences window - native Carbon / Appearance Manager UI.
 */

#define PREFS_W_W 480
#define PREFS_W_H 420
#define PREFS_BANNER_H 40
#define PREFS_PANEL_TOP 48
#define PREFS_PANEL_BOT 368

enum {
	PREFS_CAT_GENERAL = 0,
	PREFS_CAT_CONTENT,
	PREFS_CAT_APPEAR,
	PREFS_CAT_PRIVACY,
	PREFS_CAT_NETWORK,
	PREFS_CAT_COUNT
};

struct prefs_popup_def {
	const char **labels;
	const int *values;
	int count;
};

static const char *s_lbl_cat[] = {
	"General", "Web Content", "Appearance", "Privacy", "Advanced"
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
	"8 pt", "9 pt", "10 pt", "11 pt", "12 pt", "14 pt", "16 pt", "20 pt"
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

static WindowRef g_prefs_open_win = NULL;

struct prefs_win {
	WindowRef win;
	/* category selector tabs */
	ControlRef tabs;
	int cat;
	/* General */
	TEHandle te_home;
	ControlRef btn_home_current;
	ControlRef btn_home_default;
	TEHandle te_ww;
	TEHandle te_wh;
	TEHandle active_te;
	/* Web Content */
	ControlRef ck_images;
	ControlRef ck_anim;
	ControlRef ck_css;
	ControlRef ck_js;
	ControlRef ck_popups;
	ControlRef ck_ads;
	/* Appearance */
	ControlRef pp_font;
	MenuHandle m_font;
	ControlRef pp_minfont;
	MenuHandle m_minfont;
	/* Privacy */
	ControlRef ck_cookies;
	ControlRef ck_ref;
	ControlRef ck_dnt;
	ControlRef btn_cache;
	ControlRef btn_hist;
	/* Advanced */
	ControlRef pp_fetch;
	MenuHandle m_fetch;
	ControlRef pp_perhost;
	MenuHandle m_perhost;
	/* Bottom buttons */
	ControlRef btn_defaults;
	ControlRef btn_cancel;
	ControlRef btn_ok;
};

/* Layout bounds. Order: {top, left, bottom, right}. */
static const Rect s_btn_defaults_rect     = { 358,  20, 382, 144 };
static const Rect s_btn_cancel_rect       = { 358, 276, 382, 356 };
static const Rect s_btn_ok_rect           = { 358, 372, 382, 460 };
static const Rect s_tabs_rect             = {  42,  12, 348, 468 };

/* General panel */
static const Rect s_te_home_rect          = {  94,  24, 116, 456 };
static const Rect s_btn_home_current_rect = { 124,  24, 146, 154 };
static const Rect s_btn_home_default_rect = { 124, 164, 146, 284 };
static const Rect s_te_ww_rect            = { 192,  76, 214, 140 };
static const Rect s_te_wh_rect            = { 192, 216, 214, 280 };

/* Web Content panel native checkbox glyph bounds (20x20 px at left edge) */
static const Rect s_ck_images_ctrl_rect   = {  80,  24, 100,  44 };
static const Rect s_ck_anim_ctrl_rect     = { 124,  24, 144,  44 };
static const Rect s_ck_css_ctrl_rect      = { 168,  24, 188,  44 };
static const Rect s_ck_js_ctrl_rect       = { 212,  24, 232,  44 };
static const Rect s_ck_popups_ctrl_rect   = { 256,  24, 276,  44 };
static const Rect s_ck_ads_ctrl_rect      = { 300,  24, 320,  44 };

/* Web Content logical hit-test row bounds (full clickable width: 24..456) */
static const Rect s_ck_images_row_rect    = {  80,  24, 100, 456 };
static const Rect s_ck_anim_row_rect      = { 124,  24, 144, 456 };
static const Rect s_ck_css_row_rect       = { 168,  24, 188, 456 };
static const Rect s_ck_js_row_rect        = { 212,  24, 232, 456 };
static const Rect s_ck_popups_row_rect    = { 256,  24, 276, 456 };
static const Rect s_ck_ads_row_rect       = { 300,  24, 320, 456 };

/* Appearance panel */
static const Rect s_pp_font_rect          = {  88, 160, 110, 280 };
static const Rect s_pp_minfont_rect       = { 152, 160, 174, 280 };

/* Privacy panel native checkbox glyph bounds (20x20 px at left edge) */
static const Rect s_ck_cookies_ctrl_rect  = {  74,  24,  94,  44 };
static const Rect s_ck_ref_ctrl_rect      = { 122,  24, 142,  44 };
static const Rect s_ck_dnt_ctrl_rect      = { 170,  24, 190,  44 };

/* Privacy panel logical hit-test row bounds (full clickable width: 24..456) */
static const Rect s_ck_cookies_row_rect   = {  74,  24,  94, 456 };
static const Rect s_ck_ref_row_rect       = { 122,  24, 142, 456 };
static const Rect s_ck_dnt_row_rect       = { 170,  24, 190, 456 };
static const Rect s_btn_cache_rect        = { 230,  24, 254, 160 };
static const Rect s_btn_hist_rect         = { 230, 175, 254, 310 };

/* Advanced panel */
static const Rect s_pp_fetch_rect         = {  88, 280, 110, 380 };
static const Rect s_pp_perhost_rect       = { 152, 280, 174, 380 };

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

/* Gradient banner for the Preferences header. */
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
	TextFont(1); TextFace(bold); TextSize(14);
	MoveTo(50, (short)(band.top + 22));
	DrawText(title, 0, (short)strlen(title));
	TextFace(normal);
	RGBForeColor(&saved_fg);
}

/* Safe control creation helpers with valid min/max bounds */
static ControlRef prefs_create_checkbox(WindowRef win, const Rect *r,
		const char *title, int initial)
{
	/* Create checkbox WITHOUT title - we'll draw label manually to avoid
	 * white background cutout on platinum window background. */
	ControlRef c = NewControl(win, r, "\p", 1, (short)(initial ? 1 : 0), 0, 1,
		kControlCheckBoxProc, 0);
	if (c != NULL) AutoEmbedControl(c, win);
	(void)title; /* Label drawn in prefs_paint */
	return c;
}

static ControlRef prefs_create_button(WindowRef win, const Rect *r,
		const char *title)
{
	unsigned char pstr[256];
	ControlRef c;
	c_to_pstring(title, pstr);
	c = NewControl(win, r, pstr, 1, 0, 0, 0,
		kControlPushButtonProc, 0);
	if (c != NULL) AutoEmbedControl(c, win);
	return c;
}

static void prefs_popup_attach(ControlRef c, MenuHandle m)
{
	(void)SetControlData(c, kControlEntireControl,
		kControlPopupButtonMenuHandleTag, sizeof(m), &m);
}

static ControlRef prefs_create_tabs(WindowRef win, const Rect *r,
		const char **labels, int count, int initial_tab)
{
	ControlRef c;
	int i;
	if (initial_tab < 1) initial_tab = 1;
	if (initial_tab > count) initial_tab = count;
	c = NewControl(win, r, "\p", true, 0, 1, (short)count,
		kControlTabLargeProc, 0);
	if (c == NULL) return NULL;
	for (i = 1; i <= count; i++) {
		ControlTabInfoRec info;
		info.version = kControlTabInfoVersionZero;
		info.iconSuiteID = 0;
		c_to_pstring(labels[i - 1], info.name);
		SetControlData(c, (ControlPartCode)i, kControlTabInfoTag,
			sizeof(info), (Ptr)&info);
	}
	SetControlValue(c, (short)initial_tab);
	AutoEmbedControl(c, win);
	return c;
}

static ControlRef prefs_create_popup(WindowRef win, const Rect *r,
		MenuHandle m, int count, int initial_item)
{
	ControlRef c;
	short m_id = 0;
	if (initial_item < 1) initial_item = 1;
	if (initial_item > count) initial_item = count;
	if (m != NULL) {
		InsertMenu(m, hierMenu);
		m_id = GetMenuID(m);
		CheckMenuItem(m, (short)initial_item, true);
	}
	c = NewControl(win, r, "\p", 1, (short)initial_item, m_id, 0,
		popupMenuProc, 0);
	if (c != NULL) {
		if (m != NULL) {
			SetControlPopupMenuHandle(c, m);
			SetControlPopupMenuID(c, m_id);
			SetControlMinimum(c, 1);
			SetControlMaximum(c, (short)count);
		}
		AutoEmbedControl(c, win);
	}
	return c;
}

static void prefs_popup_set_item(ControlRef c, MenuHandle m, int item)
{
	short count, i;
	if (m != NULL) {
		count = CountMenuItems(m);
		for (i = 1; i <= count; i++) {
			CheckMenuItem(m, i, (i == item));
		}
	}
	if (c != NULL) {
		SetControlValue(c, (short)item);
	}
}

static MenuHandle prefs_popup_menu(const struct prefs_popup_def *def, short id)
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



static void prefs_set_val(ControlRef c, int v)
{
	if (c != NULL) SetControlValue(c, (short)v);
}

static int prefs_get_val(ControlRef c, int fallback)
{
	if (c == NULL) return fallback;
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
	if (m != NULL) {
		DeleteMenu(GetMenuID(m));
		DisposeMenu(m);
	}
}

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

static int prefs_popup_get(ControlRef c, const struct prefs_popup_def *def, int fallback)
{
	int idx;
	if (c == NULL) return fallback;
	idx = GetControlValue(c) - 1;
	if (idx < 0) idx = 0;
	if (idx >= def->count) idx = def->count - 1;
	return def->values[idx];
}

/* Category visibility management */
static void prefs_panel_vis(struct prefs_win *pw)
{
	/* General */
	prefs_set_vis(pw->btn_home_current, pw->cat == PREFS_CAT_GENERAL);
	prefs_set_vis(pw->btn_home_default, pw->cat == PREFS_CAT_GENERAL);
	/* Web Content */
	prefs_set_vis(pw->ck_images,        pw->cat == PREFS_CAT_CONTENT);
	prefs_set_vis(pw->ck_anim,          pw->cat == PREFS_CAT_CONTENT);
	prefs_set_vis(pw->ck_css,           pw->cat == PREFS_CAT_CONTENT);
	prefs_set_vis(pw->ck_js,            pw->cat == PREFS_CAT_CONTENT);
	prefs_set_vis(pw->ck_popups,        pw->cat == PREFS_CAT_CONTENT);
	prefs_set_vis(pw->ck_ads,           pw->cat == PREFS_CAT_CONTENT);
	/* Appearance */
	prefs_set_vis(pw->pp_font,          pw->cat == PREFS_CAT_APPEAR);
	prefs_set_vis(pw->pp_minfont,       pw->cat == PREFS_CAT_APPEAR);
	/* Privacy */
	prefs_set_vis(pw->ck_cookies,       pw->cat == PREFS_CAT_PRIVACY);
	prefs_set_vis(pw->ck_ref,           pw->cat == PREFS_CAT_PRIVACY);
	prefs_set_vis(pw->ck_dnt,           pw->cat == PREFS_CAT_PRIVACY);
	prefs_set_vis(pw->btn_cache,        pw->cat == PREFS_CAT_PRIVACY);
	prefs_set_vis(pw->btn_hist,         pw->cat == PREFS_CAT_PRIVACY);
	/* Advanced */
	prefs_set_vis(pw->pp_fetch,         pw->cat == PREFS_CAT_NETWORK);
	prefs_set_vis(pw->pp_perhost,       pw->cat == PREFS_CAT_NETWORK);
}

static void prefs_set_cat(struct prefs_win *pw, int cat)
{
	Rect r;
	if (cat < 0 || cat >= PREFS_CAT_COUNT) return;
	if (cat == pw->cat) return;
	pw->cat = cat;
	if (pw->tabs != NULL) {
		SetControlValue(pw->tabs, (short)(cat + 1));
	}
	SetPortWindowPort(pw->win);
	prefs_panel_vis(pw);
	SetRect(&r, 0, PREFS_PANEL_TOP, PREFS_W_W, PREFS_PANEL_BOT);
	InvalWindowRect(pw->win, &r);
}

/* Commit UI state to the live nsoptions table */
static void prefs_apply_from_ui(struct prefs_win *pw)
{
	/* Home page */
	if (pw->te_home != NULL) {
		long n = pw->te_home[0]->teLength;
		char *buf;
		if (n < 0) n = 0;
		buf = (char *)malloc((size_t)n + 1);
		if (buf != NULL) {
			if (n > 0)
				memcpy(buf, *pw->te_home[0]->hText, (size_t)n);
			buf[n] = '\0';
			/* Trim whitespace */
			{
				char *start = buf;
				char *end;
				while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') start++;
				end = start + strlen(start);
				while (end > start && (*(end - 1) == ' ' || *(end - 1) == '\t' ||
				       *(end - 1) == '\r' || *(end - 1) == '\n')) {
					end--;
					*end = '\0';
				}
				if (start != buf) {
					memmove(buf, start, strlen(start) + 1);
				}
			}
			/* Clear if matches default so delta file is clean */
			if (buf[0] == '\0' || strcmp(buf, MACSURF_HOME_URL) == 0) {
				nsoption_set_charp(homepage_url, NULL);
			} else {
				nsoption_set_charp(homepage_url, buf);
			}
		}
	}

	/* Window dimensions */
	if (pw->te_ww != NULL) {
		long n = pw->te_ww[0]->teLength;
		char num[16];
		int v;
		if (n <= 0 || n >= (long)sizeof num) v = nsoption_int(window_width);
		else {
			memcpy(num, *pw->te_ww[0]->hText, (size_t)n);
			num[n] = '\0';
			v = atoi(num);
		}
		if (v < 0) v = 0;
		if (v > 4096) v = 4096;
		nsoption_set_int(window_width, v);
	}
	if (pw->te_wh != NULL) {
		long n = pw->te_wh[0]->teLength;
		char num[16];
		int v;
		if (n <= 0 || n >= (long)sizeof num) v = nsoption_int(window_height);
		else {
			memcpy(num, *pw->te_wh[0]->hText, (size_t)n);
			num[n] = '\0';
			v = atoi(num);
		}
		if (v < 0) v = 0;
		if (v > 4096) v = 4096;
		nsoption_set_int(window_height, v);
	}

	/* Web Content */
	if (pw->ck_images != NULL) {
		int img_val = (GetControlValue(pw->ck_images) != 0);
		nsoption_set_bool(foreground_images, img_val);
		nsoption_set_bool(background_images, img_val);
	}
	if (pw->ck_anim != NULL) {
		nsoption_set_bool(animate_images, GetControlValue(pw->ck_anim) != 0);
	}
	if (pw->ck_css != NULL) {
		nsoption_set_bool(author_level_css, GetControlValue(pw->ck_css) != 0);
	}
	if (pw->ck_js != NULL) {
		nsoption_set_bool(enable_javascript, GetControlValue(pw->ck_js) != 0);
	}
	if (pw->ck_popups != NULL) {
		nsoption_set_bool(disable_popups, GetControlValue(pw->ck_popups) != 0);
	}
	if (pw->ck_ads != NULL) {
		nsoption_set_bool(block_advertisements, GetControlValue(pw->ck_ads) != 0);
	}

	/* Appearance */
	if (pw->pp_font != NULL) {
		nsoption_set_int(font_size, prefs_popup_get(pw->pp_font, &s_popup_font, nsoption_int(font_size)));
	}
	if (pw->pp_minfont != NULL) {
		nsoption_set_int(font_min_size, prefs_popup_get(pw->pp_minfont, &s_popup_minfont, nsoption_int(font_min_size)));
	}

	/* Privacy */
	if (pw->ck_cookies != NULL) {
		nsoption_set_bool(accept_cookies, GetControlValue(pw->ck_cookies) != 0);
	}
	if (pw->ck_ref != NULL) {
		nsoption_set_bool(send_referer, GetControlValue(pw->ck_ref) != 0);
	}
	if (pw->ck_dnt != NULL) {
		nsoption_set_bool(do_not_track, GetControlValue(pw->ck_dnt) != 0);
	}

	/* Advanced */
	if (pw->pp_fetch != NULL) {
		nsoption_set_int(max_fetchers, prefs_popup_get(pw->pp_fetch, &s_popup_fetch, nsoption_int(max_fetchers)));
	}
	if (pw->pp_perhost != NULL) {
		nsoption_set_int(max_fetchers_per_host, prefs_popup_get(pw->pp_perhost, &s_popup_perhost, nsoption_int(max_fetchers_per_host)));
	}
}

/* Populate UI controls from live options */
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

	/* Web Content */
	prefs_set_val(pw->ck_images, (nsoption_bool(foreground_images) || nsoption_bool(background_images)) ? 1 : 0);
	prefs_set_val(pw->ck_anim,   nsoption_bool(animate_images) ? 1 : 0);
	prefs_set_val(pw->ck_css,    nsoption_bool(author_level_css) ? 1 : 0);
	prefs_set_val(pw->ck_js,     nsoption_bool(enable_javascript) ? 1 : 0);
	prefs_set_val(pw->ck_popups, nsoption_bool(disable_popups) ? 1 : 0);
	prefs_set_val(pw->ck_ads,    nsoption_bool(block_advertisements) ? 1 : 0);

	/* Appearance */
	prefs_popup_set_item(pw->pp_font,    pw->m_font,    prefs_popup_item(&s_popup_font, nsoption_int(font_size)));
	prefs_popup_set_item(pw->pp_minfont, pw->m_minfont, prefs_popup_item(&s_popup_minfont, nsoption_int(font_min_size)));

	/* Privacy */
	prefs_set_val(pw->ck_cookies, nsoption_bool(accept_cookies) ? 1 : 0);
	prefs_set_val(pw->ck_ref,     nsoption_bool(send_referer) ? 1 : 0);
	prefs_set_val(pw->ck_dnt,     nsoption_bool(do_not_track) ? 1 : 0);

	/* Advanced */
	prefs_popup_set_item(pw->pp_fetch,   pw->m_fetch,   prefs_popup_item(&s_popup_fetch, nsoption_int(max_fetchers)));
	prefs_popup_set_item(pw->pp_perhost, pw->m_perhost, prefs_popup_item(&s_popup_perhost, nsoption_int(max_fetchers_per_host)));

	if (pw->tabs != NULL)
		SetControlValue(pw->tabs, (short)(pw->cat + 1));
}

/* Reset UI controls to factory defaults without modifying live nsoptions */
static void prefs_load_defaults_into_ui(struct prefs_win *pw)
{
	const char *home = MACSURF_HOME_URL;

	if (pw->te_home != NULL) {
		TESetText(home, (long)strlen(home), pw->te_home);
		TESetSelect(0, 32767, pw->te_home);
	}
	if (pw->te_ww != NULL) {
		TESetText("0", 1, pw->te_ww);
		TESetSelect(0, 32767, pw->te_ww);
	}
	if (pw->te_wh != NULL) {
		TESetText("0", 1, pw->te_wh);
		TESetSelect(0, 32767, pw->te_wh);
	}

	prefs_set_val(pw->ck_images, 1);
	prefs_set_val(pw->ck_anim,   1);
	prefs_set_val(pw->ck_css,    1);
	prefs_set_val(pw->ck_js,     1);
	prefs_set_val(pw->ck_popups, 0);
	prefs_set_val(pw->ck_ads,    0);

	prefs_popup_set_item(pw->pp_font,    pw->m_font,    prefs_popup_item(&s_popup_font, 120));
	prefs_popup_set_item(pw->pp_minfont, pw->m_minfont, prefs_popup_item(&s_popup_minfont, 85));

	prefs_set_val(pw->ck_cookies, 1);
	prefs_set_val(pw->ck_ref,     1);
	prefs_set_val(pw->ck_dnt,     0);

	prefs_popup_set_item(pw->pp_fetch,   pw->m_fetch,   prefs_popup_item(&s_popup_fetch, 128));
	prefs_popup_set_item(pw->pp_perhost, pw->m_perhost, prefs_popup_item(&s_popup_perhost, 16));
}

static void prefs_set_home_from_current(struct prefs_win *pw)
{
	struct gui_window *gw = macos9_window_list_head();
	if (gw != NULL && gw->bw != NULL && pw->te_home != NULL) {
		struct nsurl *u = NULL;
		if (browser_window_get_url(gw->bw, true, &u) == NSERROR_OK && u != NULL) {
			const char *url_str = nsurl_access(u);
			if (url_str != NULL && url_str[0] != '\0') {
				TESetText(url_str, (long)strlen(url_str), pw->te_home);
				TESetSelect(0, 32767, pw->te_home);
				InvalWindowRect(pw->win, &s_te_home_rect);
			}
		}
	}
}

static void prefs_set_home_default(struct prefs_win *pw)
{
	if (pw->te_home != NULL) {
		TESetText(MACSURF_HOME_URL, (long)strlen(MACSURF_HOME_URL), pw->te_home);
		TESetSelect(0, 32767, pw->te_home);
		InvalWindowRect(pw->win, &s_te_home_rect);
	}
}

static void prefs_paint(struct prefs_win *pw)
{
	Rect content;
	Rect panel;
	Rect r;
	RGBColor saved;
	RGBColor black_c;
	RGBColor gray_c;
	RGBColor sep_c;

	black_c.red = 0; black_c.green = 0; black_c.blue = 0;
	gray_c.red = 0x5555; gray_c.green = 0x5555; gray_c.blue = 0x5555;
	sep_c.red = 0xBBBB; sep_c.green = 0xBBBB; sep_c.blue = 0xBBBB;

	SetRect(&content, 0, 0, PREFS_W_W, PREFS_W_H);
	SetRect(&panel, 0, PREFS_PANEL_TOP, PREFS_W_W, PREFS_PANEL_BOT);
	EraseRect(&panel);

	prefs_banner(&content, "Preferences");

	GetForeColor(&saved);

	/* Bottom separator line */
	RGBForeColor(&sep_c);
	MoveTo(0, PREFS_PANEL_BOT + 2);
	LineTo(PREFS_W_W, PREFS_PANEL_BOT + 2);

	/* Default button ring around OK */
	{
		Rect ok_ring = s_btn_ok_rect;
		InsetRect(&ok_ring, -4, -4);
		PenSize(3, 3);
		RGBForeColor(&black_c);
		FrameRoundRect(&ok_ring, 16, 16);
		PenSize(1, 1);
	}

	/* Draw controls: category tabs frame & visible panel controls */
	RGBForeColor(&saved);
	DrawControls(pw->win);

	/* Category content labels and descriptions */
	switch (pw->cat) {
	case PREFS_CAT_GENERAL:
		RGBForeColor(&black_c);
		TextFont(1); TextFace(bold); TextSize(12);
		MoveTo(24, 90);
		DrawString("\pHome page:");

		MoveTo(24, 182);
		DrawString("\pNew window size:");

		TextFace(normal);
		MoveTo(28, 212);
		DrawString("\pWidth:");
		MoveTo(168, 212);
		DrawString("\pHeight:");

		RGBForeColor(&gray_c);
		TextFont(3); TextSize(9);
		MoveTo(288, 212);
		DrawString("\p(0 = automatic default)");
		break;

	case PREFS_CAT_CONTENT:
		RGBForeColor(&black_c);
		TextFont(1); TextFace(normal); TextSize(12);
		/* Checkbox labels - drawn manually to avoid white cutout.
		 * Checkbox is 20px tall (top to bottom). Center vertically:
		 * baseline at rect_top + 14 (8px checkbox + 2px gap + 4px baseline offset for Geneva 12). */
		#define CK_LABEL_V(top) ((short)((top) + 14))
		MoveTo((short)(s_ck_images_ctrl_rect.left + 22), CK_LABEL_V(s_ck_images_ctrl_rect.top));
		DrawString("\pLoad images");
		MoveTo((short)(s_ck_anim_ctrl_rect.left + 22), CK_LABEL_V(s_ck_anim_ctrl_rect.top));
		DrawString("\pAnimate images");
		MoveTo((short)(s_ck_css_ctrl_rect.left + 22), CK_LABEL_V(s_ck_css_ctrl_rect.top));
		DrawString("\pUse website styles (CSS)");
		MoveTo((short)(s_ck_js_ctrl_rect.left + 22), CK_LABEL_V(s_ck_js_ctrl_rect.top));
		DrawString("\pEnable JavaScript");
		MoveTo((short)(s_ck_popups_ctrl_rect.left + 22), CK_LABEL_V(s_ck_popups_ctrl_rect.top));
		DrawString("\pBlock pop-up windows");
		MoveTo((short)(s_ck_ads_ctrl_rect.left + 22), CK_LABEL_V(s_ck_ads_ctrl_rect.top));
		DrawString("\pBlock advertisements");
		#undef CK_LABEL_V

		RGBForeColor(&gray_c);
		TextFont(3); TextFace(normal); TextSize(9);
		MoveTo(44, 108);
		DrawString("\pShows pictures and image-based page backgrounds.");
		MoveTo(64, 152);
		DrawString("\pPlays animated GIF images.");
		MoveTo(44, 196);
		DrawString("\pDisplays pages using formatting and styles provided by websites.");
		MoveTo(44, 240);
		DrawString("\pRequired for menus, comments, and interactive features on websites.");
		MoveTo(44, 284);
		DrawString("\pPrevents websites from opening unrequested new windows.");
		break;

	case PREFS_CAT_APPEAR:
		RGBForeColor(&black_c);
		TextFont(1); TextFace(normal); TextSize(12);
		MoveTo(24, 104);
		DrawString("\pDefault font size:");
		MoveTo(24, 168);
		DrawString("\pMinimum font size:");

		RGBForeColor(&gray_c);
		TextFont(3); TextSize(9);
		MoveTo(24, 126);
		DrawString("\pStandard text size for web pages (MacSurf default is 12 pt / 16 px).");
		MoveTo(24, 190);
		DrawString("\pSmallest size text will be allowed to shrink to on detailed pages.");
		break;

	case PREFS_CAT_PRIVACY:
		RGBForeColor(&black_c);
		TextFont(1); TextFace(normal); TextSize(12);
		/* Checkbox labels - drawn manually to avoid white cutout.
		 * Center vertically: baseline at rect_top + 14. */
		#define CK_LABEL_V(top) ((short)((top) + 14))
		MoveTo((short)(s_ck_cookies_ctrl_rect.left + 22), CK_LABEL_V(s_ck_cookies_ctrl_rect.top));
		DrawString("\pStore and send cookies");
		MoveTo((short)(s_ck_ref_ctrl_rect.left + 22), CK_LABEL_V(s_ck_ref_ctrl_rect.top));
		DrawString("\pSend Referer header");
		MoveTo((short)(s_ck_dnt_ctrl_rect.left + 22), CK_LABEL_V(s_ck_dnt_ctrl_rect.top));
		DrawString("\pSend Do Not Track request");
		#undef CK_LABEL_V

		RGBForeColor(&gray_c);
		TextFont(3); TextFace(normal); TextSize(9);
		MoveTo(44, 108);
		DrawString("\pAllows websites to remember your logins, sessions, and preferences.");
		MoveTo(44, 156);
		DrawString("\pInforms websites which web page linked you to them.");
		MoveTo(44, 204);
		DrawString("\pAsks websites and advertisers not to track your browsing habits.");
		MoveTo(24, 272);
		DrawString("\pRemoves temporarily cached files and recorded page visit history.");
		break;;

	case PREFS_CAT_NETWORK:
		RGBForeColor(&black_c);
		TextFont(1); TextFace(normal); TextSize(12);
		MoveTo(24, 104);
		DrawString("\pMaximum simultaneous connections:");
		MoveTo(24, 168);
		DrawString("\pMaximum connections per host:");
		MoveTo(24, 230);
		DrawString("\pMemory cache size:");

		RGBForeColor(&gray_c);
		TextFont(3); TextSize(9);
		MoveTo(24, 126);
		DrawString("\pTotal concurrent HTTP/HTTPS network connections (default is 128).");
		MoveTo(24, 190);
		DrawString("\pConcurrent connections to a single server domain (default is 16).");

		TextFont(1); TextSize(12);
		RGBForeColor(&black_c);
		MoveTo(160, 230);
		DrawString("\p32 MB (allocated from application partition)");
		break;

	default:
		break;
	}

	/* Draw popup button labels directly to guarantee crisp 12pt Geneva text */
	TextFont(1); TextFace(normal); TextSize(12);
	RGBForeColor(&black_c);

	if (pw->cat == PREFS_CAT_APPEAR) {
		int fi = prefs_popup_item(&s_popup_font, nsoption_int(font_size)) - 1;
		int mi = prefs_popup_item(&s_popup_minfont, nsoption_int(font_min_size)) - 1;
		if (fi >= 0 && fi < s_popup_font.count) {
			MoveTo((short)(s_pp_font_rect.left + 8), (short)(s_pp_font_rect.top + 15));
			DrawText(s_popup_font.labels[fi], 0, (short)strlen(s_popup_font.labels[fi]));
		}
		if (mi >= 0 && mi < s_popup_minfont.count) {
			MoveTo((short)(s_pp_minfont_rect.left + 8), (short)(s_pp_minfont_rect.top + 15));
			DrawText(s_popup_minfont.labels[mi], 0, (short)strlen(s_popup_minfont.labels[mi]));
		}
	} else if (pw->cat == PREFS_CAT_NETWORK) {
		int fi = prefs_popup_item(&s_popup_fetch, nsoption_int(max_fetchers)) - 1;
		int pi = prefs_popup_item(&s_popup_perhost, nsoption_int(max_fetchers_per_host)) - 1;
		if (fi >= 0 && fi < s_popup_fetch.count) {
			MoveTo((short)(s_pp_fetch_rect.left + 8), (short)(s_pp_fetch_rect.top + 15));
			DrawText(s_popup_fetch.labels[fi], 0, (short)strlen(s_popup_fetch.labels[fi]));
		}
		if (pi >= 0 && pi < s_popup_perhost.count) {
			MoveTo((short)(s_pp_perhost_rect.left + 8), (short)(s_pp_perhost_rect.top + 15));
			DrawText(s_popup_perhost.labels[pi], 0, (short)strlen(s_popup_perhost.labels[pi]));
		}
	}

	/* Framed TextEdit fields */
	if (pw->cat == PREFS_CAT_GENERAL) {
		RGBForeColor(&black_c);
		if (pw->te_home != NULL) {
			r = s_te_home_rect;
			FrameRect(&r);
			TEUpdate(&r, pw->te_home);
		}
		if (pw->te_ww != NULL) {
			r = s_te_ww_rect;
			FrameRect(&r);
			TEUpdate(&r, pw->te_ww);
		}
		if (pw->te_wh != NULL) {
			r = s_te_wh_rect;
			FrameRect(&r);
			TEUpdate(&r, pw->te_wh);
		}
		RGBForeColor(&saved);
	}
}

static void prefs_do_popup(ControlRef c, MenuHandle m, Point lp)
{
	short cur;
	Rect cr;
	Point gpt;
	long chosen;
	(void)lp;
	if (c == NULL || m == NULL) return;
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



static void prefs_check_toggle(ControlRef c, Point lp, const Rect *ctrl_rect)
{
	short cur;
	if (c == NULL) return;
	if (ctrl_rect != NULL && PtInRect(lp, ctrl_rect)) {
		short part = TrackControl(c, lp, NULL);
		if (part == 0) return;
	}
	cur = GetControlValue(c);
	SetControlValue(c, cur ? 0 : 1);
	Draw1Control(c);
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

static int prefs_click(struct prefs_win *pw, Point lp)
{
	Rect r;
	short part;

	/* OK: commit changes, save deltas, apply live, exit */
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

	/* Cancel: discard draft changes, exit */
	if (PtInRect(lp, &s_btn_cancel_rect)) {
		part = TrackControl(pw->btn_cancel, lp, NULL);
		if (part != 0) return 1;
		return 0;
	}

	/* Restore Defaults: reset UI controls only (confirmed on OK) */
	if (PtInRect(lp, &s_btn_defaults_rect)) {
		part = TrackControl(pw->btn_defaults, lp, NULL);
		if (part != 0) {
			prefs_load_defaults_into_ui(pw);
			SetRect(&r, 0, PREFS_PANEL_TOP, PREFS_W_W, PREFS_PANEL_BOT);
			InvalWindowRect(pw->win, &r);
		}
		return 0;
	}

	/* Category tabs (header row y: 42..68) */
	if (PtInRect(lp, &s_tabs_rect) && lp.v < 70) {
		part = TrackControl(pw->tabs, lp, NULL);
		if (part != 0) {
			short new_cat = GetControlValue(pw->tabs);
			if (new_cat >= 1 && new_cat <= PREFS_CAT_COUNT) {
				prefs_set_cat(pw, new_cat - 1);
			}
		}
		return 0;
	}

	/* Per-category interactions */
	switch (pw->cat) {
	case PREFS_CAT_GENERAL:
		if (PtInRect(lp, &s_btn_home_current_rect)) {
			part = TrackControl(pw->btn_home_current, lp, NULL);
			if (part != 0) prefs_set_home_from_current(pw);
			return 0;
		}
		if (PtInRect(lp, &s_btn_home_default_rect)) {
			part = TrackControl(pw->btn_home_default, lp, NULL);
			if (part != 0) prefs_set_home_default(pw);
			return 0;
		}
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

	case PREFS_CAT_CONTENT:
		if (PtInRect(lp, &s_ck_images_row_rect)) {
			prefs_check_toggle(pw->ck_images, lp, &s_ck_images_ctrl_rect); return 0;
		}
		if (PtInRect(lp, &s_ck_anim_row_rect)) {
			prefs_check_toggle(pw->ck_anim, lp, &s_ck_anim_ctrl_rect); return 0;
		}
		if (PtInRect(lp, &s_ck_css_row_rect)) {
			prefs_check_toggle(pw->ck_css, lp, &s_ck_css_ctrl_rect); return 0;
		}
		if (PtInRect(lp, &s_ck_js_row_rect)) {
			prefs_check_toggle(pw->ck_js, lp, &s_ck_js_ctrl_rect); return 0;
		}
		if (PtInRect(lp, &s_ck_popups_row_rect)) {
			prefs_check_toggle(pw->ck_popups, lp, &s_ck_popups_ctrl_rect); return 0;
		}
		if (PtInRect(lp, &s_ck_ads_row_rect)) {
			prefs_check_toggle(pw->ck_ads, lp, &s_ck_ads_ctrl_rect); return 0;
		}
		break;

	case PREFS_CAT_APPEAR:
		if (PtInRect(lp, &s_pp_font_rect)) {
			prefs_do_popup(pw->pp_font, pw->m_font, lp); return 0;
		}
		if (PtInRect(lp, &s_pp_minfont_rect)) {
			prefs_do_popup(pw->pp_minfont, pw->m_minfont, lp); return 0;
		}
		break;

	case PREFS_CAT_PRIVACY:
		if (PtInRect(lp, &s_ck_cookies_row_rect)) {
			prefs_check_toggle(pw->ck_cookies, lp, &s_ck_cookies_ctrl_rect); return 0;
		}
		if (PtInRect(lp, &s_ck_ref_row_rect)) {
			prefs_check_toggle(pw->ck_ref, lp, &s_ck_ref_ctrl_rect); return 0;
		}
		if (PtInRect(lp, &s_ck_dnt_row_rect)) {
			prefs_check_toggle(pw->ck_dnt, lp, &s_ck_dnt_ctrl_rect); return 0;
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
			prefs_do_popup(pw->pp_fetch, pw->m_fetch, lp); return 0;
		}
		if (PtInRect(lp, &s_pp_perhost_rect)) {
			prefs_do_popup(pw->pp_perhost, pw->m_perhost, lp); return 0;
		}
		break;

	default:
		break;
	}

	prefs_te_blur(pw);
	return 0;
}

static int prefs_key(struct prefs_win *pw, const EventRecord *ev)
{
	char ch = (char)(ev->message & charCodeMask);
	if (ev->modifiers & cmdKey) {
		if (ch == '.' || ch == 'w' || ch == 'W') return 1;
		if (ch == 0x1C) {  /* Cmd-Left Arrow: previous tab */
			int c = pw->cat - 1;
			if (c < 0) c = PREFS_CAT_COUNT - 1;
			prefs_set_cat(pw, c);
			return 0;
		}
		if (ch == 0x1D) {  /* Cmd-Right Arrow: next tab */
			int c = pw->cat + 1;
			if (c >= PREFS_CAT_COUNT) c = 0;
			prefs_set_cat(pw, c);
			return 0;
		}
		return 0;
	}
	if (ch == 0x1B) return 1;  /* Esc = cancel */
	if (ch == '\r' || ch == 0x03) {  /* Return / Enter = OK */
		prefs_apply_from_ui(pw);
		macos9_prefs_save();
		macos9_prefs_apply_live();
		return 1;
	}
	if (ch == 0x09) {  /* Tab cycles text fields */
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
	ControlRef root = NULL;
	int done = 0;

	memset(&pw, 0, sizeof pw);
	pw.cat = PREFS_CAT_GENERAL;

	if (g_prefs_open_win != NULL) {
		SelectWindow(g_prefs_open_win);
		return;
	}

	SetRect(&wb, 110, 80, (short)(110 + PREFS_W_W), (short)(80 + PREFS_W_H));
	if (CreateNewWindow(kDocumentWindowClass, kWindowCloseBoxAttribute,
			&wb, &pw.win) != noErr || pw.win == NULL) {
		return;
	}
	g_prefs_open_win = pw.win;
	c_to_pstring("Preferences", pt);
	SetWTitle(pw.win, pt);
	SetWRefCon(pw.win, 0);  /* dsMemWZErr guard */

	CreateRootControl(pw.win, &root);

	GetPort(&saved_port);
	SetPortWindowPort(pw.win);
	TextFont(1);
	TextSize(12);

	/* Bottom button row */
	pw.btn_defaults = prefs_create_button(pw.win, &s_btn_defaults_rect, "Restore Defaults");
	pw.btn_cancel   = prefs_create_button(pw.win, &s_btn_cancel_rect, "Cancel");
	pw.btn_ok       = prefs_create_button(pw.win, &s_btn_ok_rect, "OK");

	/* Category tabs (created before panel controls to establish container frame) */
	pw.tabs = prefs_create_tabs(pw.win, &s_tabs_rect, s_lbl_cat,
		PREFS_CAT_COUNT, pw.cat + 1);

	/* General panel controls */
	pw.btn_home_current = prefs_create_button(pw.win, &s_btn_home_current_rect, "Use Current Page");
	pw.btn_home_default = prefs_create_button(pw.win, &s_btn_home_default_rect, "Restore Default");

	/* Web Content checkboxes */
	pw.ck_images = prefs_create_checkbox(pw.win, &s_ck_images_ctrl_rect, "Load images",
		nsoption_bool(foreground_images) || nsoption_bool(background_images));
	pw.ck_anim   = prefs_create_checkbox(pw.win, &s_ck_anim_ctrl_rect, "Animate images",
		nsoption_bool(animate_images));
	pw.ck_css    = prefs_create_checkbox(pw.win, &s_ck_css_ctrl_rect, "Use website styles (CSS)",
		nsoption_bool(author_level_css));
	pw.ck_js     = prefs_create_checkbox(pw.win, &s_ck_js_ctrl_rect, "Enable JavaScript",
		nsoption_bool(enable_javascript));
	pw.ck_popups = prefs_create_checkbox(pw.win, &s_ck_popups_ctrl_rect, "Block pop-up windows",
		nsoption_bool(disable_popups));
	pw.ck_ads    = prefs_create_checkbox(pw.win, &s_ck_ads_ctrl_rect, "Block advertisements",
		nsoption_bool(block_advertisements));

	/* Appearance popups */
	pw.m_font = prefs_popup_menu(&s_popup_font, PREFS_MENU_ID_FONT);
	pw.pp_font = prefs_create_popup(pw.win, &s_pp_font_rect, pw.m_font,
		s_popup_font.count, prefs_popup_item(&s_popup_font, nsoption_int(font_size)));

	pw.m_minfont = prefs_popup_menu(&s_popup_minfont, PREFS_MENU_ID_MINFONT);
	pw.pp_minfont = prefs_create_popup(pw.win, &s_pp_minfont_rect, pw.m_minfont,
		s_popup_minfont.count, prefs_popup_item(&s_popup_minfont, nsoption_int(font_min_size)));

	/* Privacy checkboxes & buttons */
	pw.ck_cookies = prefs_create_checkbox(pw.win, &s_ck_cookies_ctrl_rect, "Store and send cookies",
		nsoption_bool(accept_cookies));
	pw.ck_ref     = prefs_create_checkbox(pw.win, &s_ck_ref_ctrl_rect, "Send Referer header",
		nsoption_bool(send_referer));
	pw.ck_dnt     = prefs_create_checkbox(pw.win, &s_ck_dnt_ctrl_rect, "Send Do Not Track request",
		nsoption_bool(do_not_track));
	pw.btn_cache  = prefs_create_button(pw.win, &s_btn_cache_rect, "Clear Cache...");
	pw.btn_hist   = prefs_create_button(pw.win, &s_btn_hist_rect, "Clear History...");

	/* Advanced popups */
	pw.m_fetch = prefs_popup_menu(&s_popup_fetch, PREFS_MENU_ID_FETCH);
	pw.pp_fetch = prefs_create_popup(pw.win, &s_pp_fetch_rect, pw.m_fetch,
		s_popup_fetch.count, prefs_popup_item(&s_popup_fetch, nsoption_int(max_fetchers)));

	pw.m_perhost = prefs_popup_menu(&s_popup_perhost, PREFS_MENU_ID_PERHOST);
	pw.pp_perhost = prefs_create_popup(pw.win, &s_pp_perhost_rect, pw.m_perhost,
		s_popup_perhost.count, prefs_popup_item(&s_popup_perhost, nsoption_int(max_fetchers_per_host)));

	/* TextEdit fields for General panel */
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

	SetPortWindowPort(pw.win);
	{
		Rect port_r;
		GetWindowPortBounds(pw.win, &port_r);
		InvalWindowRect(pw.win, &port_r);
	}

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
				SetPortWindowPort(pw.win);
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
		case activateEvt: {
			WindowRef which = (WindowRef)(unsigned long)ev.message;
			Boolean becoming_active = (ev.modifiers & activeFlag) != 0;
			if (which == pw.win) {
				SetPortWindowPort(pw.win);
				if (becoming_active) {
					if (pw.active_te != NULL) TEActivate(pw.active_te);
				} else {
					if (pw.active_te != NULL) TEDeactivate(pw.active_te);
				}
				{
					Rect pb;
					GetWindowPortBounds(pw.win, &pb);
					InvalWindowRect(pw.win, &pb);
				}
			}
			break;
		}
		case updateEvt:
			if ((WindowRef)ev.message == pw.win) {
				BeginUpdate(pw.win);
				prefs_paint(&pw);
				EndUpdate(pw.win);
			} else {
				extern void macos9_handle_update(const EventRecord *event);
				macos9_handle_update(&ev);
				SetPortWindowPort(pw.win);
			}
			break;
		case nullEvent:
			if (pw.active_te != NULL) TEIdle(pw.active_te);
			break;
		case kHighLevelEvent:
			AEProcessAppleEvent(&ev);
			break;
		default:
			break;
		}
	}

	prefs_te_blur(&pw);

	/* Dispose controls */
	prefs_disp_ctrl(pw.btn_defaults);
	prefs_disp_ctrl(pw.btn_cancel);
	prefs_disp_ctrl(pw.btn_ok);
	prefs_disp_ctrl(pw.tabs);
	prefs_disp_ctrl(pw.btn_home_current);
	prefs_disp_ctrl(pw.btn_home_default);
	prefs_disp_ctrl(pw.ck_images);
	prefs_disp_ctrl(pw.ck_anim);
	prefs_disp_ctrl(pw.ck_css);
	prefs_disp_ctrl(pw.ck_js);
	prefs_disp_ctrl(pw.ck_popups);
	prefs_disp_ctrl(pw.ck_ads);
	prefs_disp_ctrl(pw.pp_font);
	prefs_disp_ctrl(pw.pp_minfont);
	prefs_disp_ctrl(pw.ck_cookies);
	prefs_disp_ctrl(pw.ck_ref);
	prefs_disp_ctrl(pw.ck_dnt);
	prefs_disp_ctrl(pw.btn_cache);
	prefs_disp_ctrl(pw.btn_hist);
	prefs_disp_ctrl(pw.pp_fetch);
	prefs_disp_ctrl(pw.pp_perhost);

	/* Dispose menus */
	prefs_disp_menu(pw.m_font);
	prefs_disp_menu(pw.m_minfont);
	prefs_disp_menu(pw.m_fetch);
	prefs_disp_menu(pw.m_perhost);

	/* Dispose TextEdit handles */
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
}

#endif /* __MACOS9__ */
