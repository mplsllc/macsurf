#ifndef MACOS9_MACOS9_H
#define MACOS9_MACOS9_H

struct plot_font_style;
struct gui_window;
struct rect;

/* 1. Mandatory C89 Shims for CodeWarrior */
#define inline
#define restrict

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

	/* fixes59: don't suppress Aliases.h — let Apple's own header provide the
	 * full AliasRecord/AliasPtr/AliasHandle definitions before Carbon.h chains
	 * into MacWindows.h. Previous fixes (263-266, 58) pre-declared AliasHandle
	 * to avoid Aliases.h, but every variant tripped CW8 inside MacWindows.h on
	 * SetWindowProxyAlias / GetWindowProxyAlias prototypes. Including Aliases.h
	 * explicitly now resolves AliasRecord cleanly. With __INTERNETCONFIG__ still
	 * suppressed (fixes265), the original "AliasRecord incomplete in
	 * InternetConfig.h" cascade can't re-emerge. */
	#include <Aliases.h>
	/* MacSurf does not use the Keychain — suppress KeychainCore.h AND
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
	 * CW8 install — TWOWORDINLINE lines and TARGET_CPU_68K conditionals show
	 * up with garbage bytes / unterminated comments. MacSurf doesn't touch
	 * the Power Manager, so suppress the whole header. */
	#ifndef __POWER__
	#define __POWER__
	#endif

	#include <Carbon.h>
	#include <Quickdraw.h>
	#include <TextEdit.h>
	#include <Controls.h>
	#include <Appearance.h>
#else
	#include <stdbool.h>
#endif

/* 3. Core NetSurf Types */
#include "utils/errors.h"
#include "netsurf/types.h"
#include "netsurf/window.h"

/* 4. Implementation Structs */
struct gui_window {
	WindowRef window;
	ControlRef back_btn;
	ControlRef forward_btn;
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
	Rect content_rect;
	Rect status_rect;
	bool needs_reformat;
	bool reformat_in_progress;
	char status[128];
	struct gui_window *next;
};

struct gui_download_window { struct gui_window *parent; };

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

/* Menu IDs and item numbers — restored fixes307. */
#define MENU_APPLE  128
#define MENU_FILE   129
#define MENU_EDIT   130
#define MENU_GO     131

#define ITEM_FILE_NEW       1
#define ITEM_FILE_LOCATION  2
#define ITEM_FILE_CLOSE     3
#define ITEM_FILE_QUIT      5

#define ITEM_GO_BACK        1
#define ITEM_GO_FORWARD     2
#define ITEM_GO_STOP        3
#define ITEM_GO_RELOAD      4
#define ITEM_GO_HOME        6

struct gui_window *macos9_find_window(WindowRef w);
void macos9_window_layout(struct gui_window *g);
void macos9_window_invalidate_all(struct gui_window *g);
void macos9_window_invalidate_content(struct gui_window *g);
void macos9_window_invalidate_status(struct gui_window *g);
void macos9_window_invalidate_url(struct gui_window *g);
void macos9_window_update_scrollbars(struct gui_window *g);
void macos9_window_scroll_to(struct gui_window *g, int nx, int ny);
void macos9_window_scroll_by(struct gui_window *g, int dx, int dy);
void macos9_window_handle_scrollbar_click(struct gui_window *g, ControlRef c, short p, void *lp);
void macos9_window_te_activate_url(struct gui_window *g);
void macos9_window_te_deactivate_url(struct gui_window *g);
void macos9_window_navigate(struct gui_window *g, const char *u);
void macos9_window_address_bar_submit(struct gui_window *g);
void macos9_window_back(struct gui_window *g);
void macos9_window_forward(struct gui_window *g);
void macos9_window_reload(struct gui_window *g);
void macos9_window_home(struct gui_window *g);
void macos9_window_update_button_states(struct gui_window *g);
void macos9_window_resize(struct gui_window *g);
void macos9_windows_te_idle(void);
void macos9_windows_process_deferred(void);
struct gui_window *macos9_create_initial_window(void);
extern struct gui_window *initial_win;
void macos9_handle_mouse_down(const EventRecord *event);
void macos9_handle_key_down(const EventRecord *event);
void macos9_poll_mouse_hover(void);
void macos9_poll(void);
extern nserror macos9_schedule(int t, void (*callback)(void *p), void *p);
short macos9_font_id_from_style(const struct plot_font_style *fstyle);
short macos9_face_from_style(const struct plot_font_style *fstyle);
size_t macos9_utf8_to_macroman(const char *u, size_t l, char *m, size_t mx);

/* MACSURF_HOME_URL canonical definition is in macsurf_config.h.
 * Old frogfind default removed per fixes301. */
#define MACSURF_URL_MAX 1024
#define MACSURF_CONTENT_MAX (256 * 1024)

#endif
