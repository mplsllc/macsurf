/*
 * MacSurf — Mac OS 9 frontend for NetSurf
 * macos9.h — Shared declarations for the Mac OS 9 frontend
 *
 * This file is part of MacSurf, built on the NetSurf engine.
 * Licensed under GPL v2.
 */

#ifndef MACOS9_MACOS9_H
#define MACOS9_MACOS9_H

#ifdef __MACOS9__
/* Mac Toolbox headers first — MacTypes.h enum { false, true }
 * must be parsed before bool/true/false are #define'd. */
#include <MacWindows.h>
#include <Controls.h>
#include "shims/mac_types.h"	/* bool block comes after Mac headers */
#else
#include <stdbool.h>
/* Linux build: stub types for struct gui_window */
typedef void *WindowRef;
typedef void *ControlRef;
#endif

extern bool macos9_done;

struct gui_window {
	WindowRef window;		/* Carbon window */
	ControlRef scroll_h;		/* horizontal scroll bar */
	ControlRef scroll_v;		/* vertical scroll bar */
	ControlRef url_field;		/* URL text field */
	struct browser_window *bw;
	int scroll_x;
	int scroll_y;
	int content_width;
	int content_height;
	struct gui_window *next;	/* linked list */
};

struct gui_download_window {
	struct gui_window *parent;
};

/* Fetch activity flag — when true, event loop uses 1-tick sleep */
extern bool macos9_fetching;

/* Menu IDs */
#define MENU_APPLE  128
#define MENU_FILE   129
#define MENU_EDIT   130
#define MENU_GO     131
#define MENU_HELP   132

/* File menu items */
#define ITEM_FILE_NEW      1
#define ITEM_FILE_LOCATION 2
#define ITEM_FILE_CLOSE    3
#define ITEM_FILE_QUIT     5

/* Go menu items */
#define ITEM_GO_BACK       1
#define ITEM_GO_FORWARD    2
#define ITEM_GO_STOP       3
#define ITEM_GO_RELOAD     4
#define ITEM_GO_HOME       6

/* Window layout constants */
#define MACOS9_SCROLLBAR_WIDTH  15
#define MACOS9_URLBAR_HEIGHT    22

/* schedule.c — scheduler state (read-only outside schedule.c) */
extern bool macos9_sched_active;
extern unsigned long macos9_sched_time;

nserror macos9_schedule(int t, void (*callback)(void *p), void *p);
bool macos9_schedule_run(void);
int macos9_get_next_delay(void);

/* window.c — public window functions */
struct gui_window *macos9_create_initial_window(void);
void macos9_window_destroy(struct gui_window *gw);
struct gui_window *macos9_find_window(WindowRef w);

#endif /* MACOS9_MACOS9_H */
