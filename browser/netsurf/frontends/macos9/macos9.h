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
	#include <Files.h>
	struct AliasRecord;
	typedef struct AliasRecord **AliasHandle;
	#include <Aliases.h>

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
void macos9_handle_mouse_down(const EventRecord *event);
void macos9_handle_key_down(const EventRecord *event);
void macos9_poll(void);
extern nserror macos9_schedule(int t, void (*callback)(void *p), void *p);
short macos9_font_id_from_style(const struct plot_font_style *fstyle);
short macos9_face_from_style(const struct plot_font_style *fstyle);
size_t macos9_utf8_to_macroman(const char *u, size_t l, char *m, size_t mx);

#define MACSURF_HOME_URL "http://frogfind.com"
#define MACSURF_URL_MAX 1024
#define MACSURF_CONTENT_MAX (256 * 1024)

#endif
