#ifndef MACOS9_MACOS9_H
#define MACOS9_MACOS9_H

struct plot_font_style;
struct gui_window;
struct rect;

/* 1. Mandatory C89 Shims for CodeWarrior */
/* fixes526: guard so we don't REDEFINE inline.  macsurf_prefix.h (the global
 * prefix) already defines `inline` for the QuickJS build (__inline__); an
 * unconditional `#define inline` here redefined it to empty -> CW8 "macro
 * 'inline' redefined" error.  Defer to whatever the prefix set. */
#ifndef inline
#define inline
#endif
#ifndef restrict
#define restrict
#endif

/* 2. Absolute Foundation - MUST BE FIRST 
 * We define standard types and Carbon before anything else.
 */
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>

#ifdef __MACOS__
	#ifndef TARGET_API_MAC_CARBON
		#define TARGET_API_MAC_CARBON 1
	#endif
	/* Map NetSurf bool to Apple Boolean to prevent collisions.
	 * Guard against prefix.h already having defined bool via stdbool.h. */
	#ifndef bool
	typedef unsigned char bool;
	#define __bool_true_false_are_defined 1
	#endif
	
	/* Carbon.h's chain processes InternetConfig.h (uses AliasRecord by
	 * value) and MacWindows.h via LowMem.h (uses AliasHandle) before
	 * Aliases.h, so both fail when AliasRecord/AliasHandle are undefined.
	 * Include Files.h first, then pre-declare AliasHandle via pointer to
	 * incomplete struct, then include Aliases.h to complete AliasRecord
	 * before Carbon.h processes any header that needs these types.
	 * Mirrors the pattern in macsurf_debug.c. */
	/* Suppress InternetConfig.h before Carbon.h includes it (line 130).
	 * InternetConfig.h:271 uses AliasRecord by value, but its own header
	 * never arrives before Carbon.h chains to it.  MacSurf does not use
	 * Internet Config Manager, so skipping the header is safe. */
	#ifndef __INTERNETCONFIG__
	#define __INTERNETCONFIG__
	#endif

	/* fixes59: don't suppress Aliases.h - let Apple's own header provide the
	 * full AliasRecord/AliasPtr/AliasHandle definitions before Carbon.h chains
	 * into MacWindows.h. Previous fixes (263-266, 58) pre-declared AliasHandle
	 * to avoid Aliases.h, but every variant tripped CW8 inside MacWindows.h on
	 * SetWindowProxyAlias / GetWindowProxyAlias prototypes. Including Aliases.h
	 * explicitly now resolves AliasRecord cleanly. With __INTERNETCONFIG__ still
	 * suppressed (fixes265), the original "AliasRecord incomplete in
	 * InternetConfig.h" cascade can't re-emerge. */
	#include <Aliases.h>
	/* MacSurf does not use the Keychain - suppress KeychainCore.h AND
	 * KeychainHI.h (Carbon.h:210 chain) to avoid their C89-incompatible
	 * function prototypes (KCRef by value, etc.). */
	#ifndef __KEYCHAINCORE__
	#define __KEYCHAINCORE__
	#endif
	#ifndef __KEYCHAINHI__
	#define __KEYCHAINHI__
	#endif
	/* ATSLayoutTypes.h uses C11 anonymous struct/union members which CW8 rejects.
	 * MacSurf uses QuickDraw, not ATS text layout. */
	#ifndef __ATSLayoutTypes__
	#define __ATSLayoutTypes__
	#endif
	/* Power.h (via CoreServices.h:247) reports as corrupted on at least one
	 * CW8 install - TWOWORDINLINE lines and TARGET_CPU_68K conditionals show
	 * up with garbage bytes / unterminated comments. MacSurf doesn't touch
	 * the Power Manager, so suppress the whole header. */
	#ifndef __POWER__
	#define __POWER__
	#endif

	#include <Carbon.h>
	#include <Quickdraw.h>
	#include <QDOffscreen.h>
	#include <TextEdit.h>
	#include <Controls.h>
	#include <Appearance.h>
#else
	#include <stdbool.h>
#endif

/* 3. Core NetSurf Types */
#include "utils/ns_errors.h"
#include "netsurf/types.h"
#include "netsurf/window.h"

/* 4. Implementation Structs */
struct gui_window {
	WindowRef window;
	ControlRef back_btn;
	ControlRef forward_btn;
	ControlRef stop_btn;      /* fixes724 - Stop (X) */
	ControlRef reload_btn;
	ControlRef home_btn;
	ControlRef vscroll;
	ControlRef hscroll;
	struct browser_window *bw;
	TEHandle url_te;
	bool url_field_active;
	int scroll_x;
	int scroll_y;
	int content_width;
	int content_height;
	Rect toolbar_rect;
	Rect url_rect;
	Rect loader_rect;         /* fixes726 - animated loading spinner slot */
	Rect content_rect;
	Rect status_rect;
	bool needs_reformat;
	bool reformat_in_progress;
	char status[128];
	/* fixes77c -- offscreen GWorld used as a back buffer for the
	 * content paint. NULL before first allocation. content_gworld_rect
	 * tracks the window-space bounds the GWorld was sized for so the
	 * update handler can detect resize and reallocate. */
	GWorldPtr content_gworld;
	Rect content_gworld_rect;
	struct gui_window *next;
	/* fixes451: set after first profile emit per page, reset on new URL */
	int profile_emitted;
	/* fixes640: PERFACC (phase-accumulator) summary is emitted once at the
	 * real load-complete edge (browser_window_stop_available true->false),
	 * NOT at first-paint. perf_load_active latches that a load was seen in
	 * flight; perf_summary_emitted prevents a second emit. Both reset on new
	 * URL in macos9_gw_set_url. */
	int perf_load_active;
	int perf_summary_emitted;
	/* fixes645 (#188): self-tracked zoom/maximize state. CreateNewWindow
	 * never populates the classic WStateData standard state, so we don't
	 * rely on ZoomWindow/ZoomWindowIdeal - on first zoom we save the
	 * user-state content bounds here and fill the screen; on the next
	 * zoom we restore them. `zoomed` toggles which way the box goes. */
	Rect zoom_saved_bounds;
	int zoomed;
	/* fixes663 (#191): in-page text caret. The place_caret gui callback
	 * hands us document-relative coords + height; the update handler draws
	 * the caret on top of the composited content and macos9_caret_blink_tick
	 * toggles caret_on. caret_active is set while a field owns the caret and
	 * cleared on GW_EVENT_REMOVE_CARET (blur). Distinct from the URL bar,
	 * which is a Carbon TextEdit field with its own TEIdle blink. */
	int caret_active;
	int caret_on;
	int caret_x;
	int caret_y;
	int caret_h;
};

/* fixes645 - download manager V2. Downloads now auto-save to a "MacSurf
 * Downloads" folder (no modal Nav save dialog - that was the source of
 * the kNavInvalidSystemConfigErr / -5699 failure that made HTTPS
 * downloads silently do nothing), so several can run concurrently. Each
 * download is one node in a list the modeless manager window draws.
 * refnum < 0 means the file could not be created - data callbacks then
 * short-circuit so partial writes don't crash. fsspec is kept so
 * error/abort can FSpDelete the partial file. dl_state: 0=active,
 * 1=done, 2=failed. */
struct gui_download_window {
	struct gui_window *parent;
	FSSpec             fsspec;
	short              refnum;
	unsigned long      bytes_written;
	unsigned long      total_length;   /* 0 if unknown */
	char               filename[64];
	int                aborted;
	int                dl_state;       /* fixes645: 0 active,1 done,2 fail/cancel */
	struct gui_download_window *dl_next; /* fixes645: manager list link */
	struct download_context *dl_ctx;   /* fixes646: for Cancel (abort) */
};

/* 5. External Declarations */
extern struct gui_window_table *macos9_window_table;
extern struct gui_layout_table *macos9_layout_table;
extern struct gui_utf8_table *macos9_utf8_table;
extern struct gui_bitmap_table *macos9_bitmap_table;
extern struct gui_misc_table macos9_misc_table;
extern struct gui_download_table *macos9_download_table;
extern struct gui_clipboard_table *macos9_clipboard_table;

extern bool macos9_done;
extern bool macos9_quitting;

/* Menu IDs and item numbers - restored fixes307. */
#define MENU_APPLE  128
#define MENU_FILE   129
#define MENU_EDIT   130
#define MENU_GO     131
#define MENU_VIEW   132
#define MENU_BOOKMARK 133  /* fixes351 (#48) */
#define MENU_HISTORY  134  /* fixes694 (#47) */

/* fixes707 - hierarchical bookmark folder submenus get IDs
 * MENU_BMK_SUB_BASE .. MENU_BMK_SUB_BASE+MENU_BMK_SUB_MAX-1. Kept clear of
 * the 128-134 app menus and the toolbox's own hierarchical range. */
#define MENU_BMK_SUB_BASE 200
#define MENU_BMK_SUB_MAX  32

#define ITEM_FILE_NEW       1
#define ITEM_FILE_LOCATION  2
#define ITEM_FILE_CLOSE     3
#define ITEM_FILE_SENDLOG   4   /* fixes720: Send Debug Log */
#define ITEM_FILE_QUIT      6

#define ITEM_GO_BACK        1
#define ITEM_GO_FORWARD     2
#define ITEM_GO_STOP        3
#define ITEM_GO_RELOAD      4
#define ITEM_GO_HOME        6

#define ITEM_VIEW_SOURCE    1
#define ITEM_VIEW_FIND      3
/* fixes883 - Zoom and Downloads were REAL and completely undiscoverable:
 * page zoom worked only via unlisted Cmd -/+/0 keystrokes, and the download
 * manager window could only be reached by starting a download. Both are pure
 * exposure -- no new plumbing. */
#define ITEM_VIEW_ZOOM_IN   5
#define ITEM_VIEW_ZOOM_OUT  6
#define ITEM_VIEW_ZOOM_100  7
#define ITEM_VIEW_DOWNLOADS 9

/* Bookmarks menu items. fixes700 (#50): 1=Add, 2=Manage Bookmarks (opens
 * the manager window), 3=separator, 4+=bookmarks. */
#define ITEM_BMK_ADD        1
#define ITEM_BMK_MANAGE     2
#define ITEM_BMK_FIRST      4   /* first dynamic entry */

/* History menu items. fixes706 (#47): 1 = Show All History (opens the
 * manager window), 2 = Clear History, 3 = Clear Cache, 4 = separator, 5+ =
 * recent visits (most-recent first), rebuilt from the persistent history
 * store on every menu-bar click. */
#define ITEM_HIST_SHOW_ALL    1
#define ITEM_HIST_CLEAR       2
#define ITEM_HIST_CLEAR_CACHE 3
#define ITEM_HIST_FIRST       5

/* fixes645 (#48) - bookmarks menu + persistence (macos9_chrome_extras.c
 * + macos9_disk_cache.c). */
void macos9_bookmarks_init(void);
void macos9_bookmark_menu_rebuild(void);
void macos9_bookmark_navigate(struct gui_window *g, int menu_item);
void macos9_bookmark_submenu_navigate(struct gui_window *g, int menu_id, int item);
long macos9_bookmarks_load(char *out_buf, long buf_cap);
void macos9_bookmarks_save(const char *buf, long len);
/* fixes693 - bookmark folders + rename (used by the management UI). */
int  macos9_bookmark_rename(int id, const char *new_label);
int  macos9_bookmark_set_url(int id, const char *new_url);
int  macos9_bookmark_delete(int id);
int  macos9_bookmark_new_folder(const char *name, int parent_id);
int  macos9_bookmark_set_parent(int id, int parent_id);
/* fixes1162b - bookmarks Import (Netscape HTML or the native tab grammar)
 * and Export (native tab grammar). */
int  macos9_bookmarks_import_buffer(const char *buf, long len);
/* fixes700 (#50) - modal Bookmark manager window (folders, rename, delete,
 * move, go). */
void macos9_bookmark_window_show(struct gui_window *g);
/* fixes694/698 (#47) - History menu + persistent, clearable history store
 * (macos9_chrome_extras.c + macos9_disk_cache.c). */
void macos9_history_init(void);
void macos9_history_menu_rebuild(void);
void macos9_history_navigate(struct gui_window *g, int menu_item);
void macos9_history_record(struct gui_window *g, const char *title);
void macos9_history_clear(void);
void macos9_history_delete_entry(int index);
long macos9_history_load(char *out_buf, long buf_cap);
void macos9_history_save(const char *buf, long len);
/* fixes699 (#47) - modal History manager window (day-grouped, clearable). */
void macos9_history_window_show(struct gui_window *g);
/* fixes706 - Clear Cache menu handler (wipes disk cache + dead-host state). */
void macos9_cache_clear_ui(void);

/* fixes645 (#199) - modeless download-manager window (macos9_download.c),
 * routed from the main event loop. */
long macos9_download_mgr_is(WindowRef w);   /* 1 if w is the mgr window */
void macos9_download_mgr_draw(void);
void macos9_download_mgr_show(void);        /* fixes883: View > Downloads */
void macos9_download_mgr_click(short part, Point where);
#ifdef __MACOS9__
OSErr macos9_downloads_dir_get(short *vRef, long *dirID);
/* fixes647: shared <app>/MacSurfData[/subfolder] resolver (Cache, Downloads,
 * Bookmarks, log all nest under one MacSurfData folder). subfolder NULL =
 * the MacSurfData folder itself. */
OSErr macos9_data_dir_get(const char *subfolder, short *vRef, long *dirID);
#endif

struct gui_window *macos9_find_window(WindowRef w);
void macos9_window_layout(struct gui_window *g);
void macos9_window_invalidate_all(struct gui_window *g);
void macos9_window_invalidate_content(struct gui_window *g);
void macos9_window_request_reformat(struct gui_window *g);
void macos9_window_invalidate_rect(struct gui_window *g, int px, int py, int pw, int ph);
void macos9_window_invalidate_status(struct gui_window *g);
void macos9_window_invalidate_url(struct gui_window *g);
/* fixes294 - Phase 0 favicon plumbing.  load fn must be called once at
 * startup; draw fn is called from main.c's draw_url_bar after TEUpdate. */
void macos9_window_load_default_favicon(void);
void macos9_window_draw_favicon(struct gui_window *g);
/* fixes297 - toolbar button icon overlays */
void macos9_window_load_toolbar_icons(void);
void macos9_window_draw_toolbar_icons(struct gui_window *g);
/* fixes298 - Netscape-7-style gradient background for the toolbar area */
void macos9_window_draw_toolbar_bg(struct gui_window *g);
void macos9_window_update_scrollbars(struct gui_window *g);
void macos9_window_scroll_to(struct gui_window *g, int nx, int ny);
void macos9_window_scroll_by(struct gui_window *g, int dx, int dy);
void macos9_window_handle_scrollbar_click(struct gui_window *g, ControlRef c, short p, void *lp);
void macos9_window_te_activate_url(struct gui_window *g);
void macos9_window_te_deactivate_url(struct gui_window *g);
void macos9_window_set_url_display(struct gui_window *g, const char *u);
/* fixes763 - address-bar suggestion dropdown */
int  macos9_urlsug_active(struct gui_window *g);
int  macos9_urlsug_click(struct gui_window *g, Point p);
void macos9_urlsug_hide(struct gui_window *g);
void macos9_urlsug_draw(struct gui_window *g);
void macos9_window_navigate(struct gui_window *g, const char *u);
void macos9_window_address_bar_submit(struct gui_window *g);
void macos9_window_back(struct gui_window *g);
void macos9_window_forward(struct gui_window *g);
void macos9_window_stop(struct gui_window *g);   /* fixes724 */
void macos9_window_reload(struct gui_window *g);
void macos9_window_home(struct gui_window *g);
void macos9_window_update_button_states(struct gui_window *g);
void macos9_window_update_hover(struct gui_window *g);   /* fixes725 */
void macos9_window_resize(struct gui_window *g);
/* fixes641 - declared for the per-window close path in main.c (both defined
 * in window.c; previously only referenced internally). */
struct gui_window *macos9_window_list_head(void);
void macos9_window_destroy(struct gui_window *g);
void macos9_windows_te_idle(void);
void macos9_windows_process_deferred(void);
struct gui_window *macos9_create_initial_window(void);
extern struct gui_window *initial_win;
void macos9_handle_mouse_down(const EventRecord *event);
void macos9_handle_key_down(const EventRecord *event);
void macos9_handle_update(const EventRecord *event);
/* Throttled repaint during tight StillDown() drag loops (scrollbar + text
 * selection). Only paints if >= N ticks have elapsed since *last_tick;
 * updates *last_tick when it does.  gw may be NULL (no-op). */
void macos9_throttled_repaint(struct gui_window *gw, unsigned long *last_tick);
void macos9_drag_autoscroll(struct gui_window *gw, Point curp,
		unsigned long *last_scroll_tick);
void macos9_poll_mouse_hover(void);
void macos9_poll(void);
extern nserror macos9_schedule(int t, void (*callback)(void *p), void *p);
/* fixes517: cancel ALL scheduled callbacks owned by p (universal anti-UAF). */
extern void macos9_schedule_cancel_owner(void *p);
/* fixes1148: check whether (callback, p) is already queued, without
 * modifying the queue.  Used by the reconvert path to avoid
 * rescheduling on every DOM mutation. */
extern int macos9_sched_is_queued(void (*callback)(void *p), void *p);
short macos9_font_id_from_style(const struct plot_font_style *fstyle);
void  macos9_font_metric_probe_run(void); /* fixes144a -- diagnostic probe */
void  macos9_font_vmetric_probe_run(void); /* fixes153 -- FontInfo dump */
short macos9_face_from_style(const struct plot_font_style *fstyle);

/* fixes157: font-family alias retry post-fixes156 (defensive-clamp fix).
 * Hardware-accepted on G3 2026-05-20 - Times/Monaco/Helvetica dispatch
 * resolved per-segment with no width-vs-paint divergence and no fixes145
 * horizontal scrambling on mixed-family inline lines. MACSURF_FONT_ALIAS_DIAG
 * flipped to 0 (silent) post-acceptance per the user's "keep gated, don't
 * strip" directive: code block in macos9_font_measure + macos9_plot_text
 * stays in place as a dormant probe handle for any future scrambling
 * regression - flip to 1 here to re-enable without rebuilding the
 * diagnostic.
 *
 * MACSURF_FONT_ALIAS_DIAG_SMART = 1 filters the firehose: log only when
 * the computed style requests a non-SANS_SERIF family (SERIF / MONOSPACE /
 * CURSIVE / FANTASY). Set to 0 alongside _DIAG to log every call (fixes154
 * behaviour) if a Helvetica-path mismatch is suspected later. */
#define MACSURF_FONT_ALIAS_DIAG 0
#define MACSURF_FONT_ALIAS_DIAG_SMART 1

size_t macos9_utf8_to_macroman(const char *u, size_t l, char *m, size_t mx);
size_t macos9_macroman_to_utf8(const unsigned char *m, size_t l, char *u, size_t mx);

/* fixes609 - shared effective letter/word spacing, so macos9_font_measure
 * (macos9_font.c) and macos9_plot_text (plotters.c) fold in the bold-smear
 * and sub-12 bitmap-gap bumps identically and their advance widths agree. */
void macos9_run_spacing(const struct plot_font_style *fstyle,
			short font_id, short face, short size, size_t mac_len,
			int *out_ls, int *out_ws);

/* fixes376 - Edit menu item ordering. Must match the AppendMenu calls in
 * main.c: Undo(1), separator(2), Cut(3), Copy(4), Paste(5). Item 2 (the
 * separator) has no selector. SELECT_ALL is a synthetic selector (not a menu
 * item) used by macos9_url_te_edit. */
#define ITEM_EDIT_UNDO        1
#define ITEM_EDIT_CUT         3
#define ITEM_EDIT_COPY        4
#define ITEM_EDIT_PASTE       5
#define ITEM_EDIT_SELECT_ALL  7   /* fixes743 (#214): Undo,sep,Cut,Copy,Paste,sep,SelectAll */

/* fixes376 - Cut/Copy/Paste/Select-All on the URL TextEdit field, synced with
 * the Carbon desk scrap (MacRoman, no UTF-8 conversion on this path). */
void macos9_url_te_edit(struct gui_window *g, short edit_item);

/* Preferences window: Apple menu item 2 ("Preferences...", Cmd-,).
 * Must match the AppendMenu calls in main.c - About(1), Prefs(2),
 * separator(3). */
#define ITEM_APPLE_PREFS 2

struct nsoption_s;
/* Preferences (macos9_prefs.c): boot baseline + persistence + window. */
void macos9_prefs_show(void);
void macos9_prefs_apply_defaults(void);
nserror macos9_prefs_set_defaults(struct nsoption_s *defs);
void macos9_prefs_load(void);
void macos9_prefs_save(void);
void macos9_prefs_log_deltas(void);
const char *macos9_home_url(void);
void macos9_prefs_apply_live(void);

/* MACSURF_HOME_URL canonical definition is in macsurf_config.h.
 * Old frogfind default removed per fixes301. */
#define MACSURF_URL_MAX 1024
#define MACSURF_CONTENT_MAX (256 * 1024)

#endif
