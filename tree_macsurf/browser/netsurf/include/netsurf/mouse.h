/*
 * Copyright 2003 Phil Mellor <monkeyson@users.sourceforge.net>
 * Copyright 2006 James Bursa <bursa@users.sourceforge.net>
 * Copyright 2010 Michael Drake <tlsa@netsurf-browser.org>
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 *
 * NetSurf is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * NetSurf is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * \file
 *
 * Core mouse and pointer states.
 */

#ifndef _NETSURF_MOUSE_H_
#define _NETSURF_MOUSE_H_

/**
 * Mouse state:
 *   1 is primary mouse button.
 *     Select on RISC OS, Left button on GTK/QT/win32
 *   2 is auxillary mouse button.
 *     Adjust on RISC OS, Middle button on GTK/QT/win32
 *   3 is secondary mouse button.
 *     Menu on RISC OS, right button on GTK/QT/win32
 *   4 is fourth button.
 *     Not present on RISC OS, back on GTK/QT/win32
 *   5 is fifth button.
 *     Not present on RISC OS, forwards on GTK/QT/win32
 *
 * \note click meaning is different for different front ends. On RISC
 *       OS, it is standard to act on press, so a click is fired at
 *       the same time as a mouse button is pressed. With GTK, it is
 *       standard to act on release, so a click is fired when the
 *       mouse button is released, if the operation wasn't a drag.
 *
 * \note double and triple clicks are fired alongside a
 *       BROWSER_MOUSE_CLICK_[1|2] to indicate which button is used.
 */
typedef enum browser_mouse_state {
	/** No mouse buttons pressed, May be used to indicate hover or
	 * end of drag.
	 */
	BROWSER_MOUSE_HOVER = 0,

	/** primary button pressed */
	BROWSER_MOUSE_PRESS_1 = (1 <<  0),
	/** auxillary button pressed */
	BROWSER_MOUSE_PRESS_2 = (1 <<  1),
	/** secondary button pressed */
	BROWSER_MOUSE_PRESS_3 = (1 <<  2),
	/** fourth button pressed */
	BROWSER_MOUSE_PRESS_4 = (1 <<  3),
	/** fifth button pressed */
	BROWSER_MOUSE_PRESS_5 = (1 <<  4),

	/** button 1 clicked. */
	BROWSER_MOUSE_CLICK_1 = (1 <<  5),
	/** button 2 clicked. */
	BROWSER_MOUSE_CLICK_2 = (1 <<  6),
	/** button 3 clicked. */
	BROWSER_MOUSE_CLICK_3 = (1 <<  7),
	/** button 4 clicked. */
	BROWSER_MOUSE_CLICK_4 = (1 <<  8),
	/** button 5 clicked. */
	BROWSER_MOUSE_CLICK_5 = (1 <<  9),

	/** button double clicked */
	BROWSER_MOUSE_DOUBLE_CLICK = (1 << 10),
	/** button triple clicked */
	BROWSER_MOUSE_TRIPLE_CLICK = (1 << 11),

	/** start of button 1 drag */
	BROWSER_MOUSE_DRAG_1 = (1 << 12),
	/** start of button 2 drag */
	BROWSER_MOUSE_DRAG_2 = (1 << 13),

	/** a drag operation was started and a mouse button is still pressed */
	BROWSER_MOUSE_DRAG_ON = (1 << 14),

	/** during button 1 drag */
	BROWSER_MOUSE_HOLDING_1 = (1 << 15),
	/** during button 2 drag */
	BROWSER_MOUSE_HOLDING_2 = (1 << 16),

	/** 1st modifier key pressed (eg. Shift) */
	BROWSER_MOUSE_MOD_1 = (1 << 17),
	/** 2nd modifier key pressed (eg. Ctrl) */
	BROWSER_MOUSE_MOD_2 = (1 << 18),
	/** 3rd modifier key pressed (eg. Alt) */
	BROWSER_MOUSE_MOD_3 = (1 << 19),
	/** 4th modifier key pressed (eg. Meta) */
	BROWSER_MOUSE_MOD_4 = (1 << 20),

	/** pointer leaving window */
	BROWSER_MOUSE_LEAVE = (1 << 21),
} browser_mouse_state;


typedef enum gui_pointer_shape {
	GUI_POINTER_DEFAULT,
	GUI_POINTER_POINT,
	GUI_POINTER_CARET,
	GUI_POINTER_MENU,
	GUI_POINTER_UP,
	GUI_POINTER_DOWN,
	GUI_POINTER_LEFT,
	GUI_POINTER_RIGHT,
	GUI_POINTER_RU,
	GUI_POINTER_LD,
	GUI_POINTER_LU,
	GUI_POINTER_RD,
	GUI_POINTER_CROSS,
	GUI_POINTER_MOVE,
	GUI_POINTER_WAIT,
	GUI_POINTER_HELP,
	GUI_POINTER_NO_DROP,
	GUI_POINTER_NOT_ALLOWED,
	GUI_POINTER_PROGRESS
} gui_pointer_shape;

/** Mouse pointer type */
typedef enum {
	BROWSER_POINTER_DEFAULT		= GUI_POINTER_DEFAULT,
	BROWSER_POINTER_POINT		= GUI_POINTER_POINT,
	BROWSER_POINTER_CARET		= GUI_POINTER_CARET,
	BROWSER_POINTER_MENU		= GUI_POINTER_MENU,
	BROWSER_POINTER_UP		= GUI_POINTER_UP,
	BROWSER_POINTER_DOWN		= GUI_POINTER_DOWN,
	BROWSER_POINTER_LEFT		= GUI_POINTER_LEFT,
	BROWSER_POINTER_RIGHT		= GUI_POINTER_RIGHT,
	BROWSER_POINTER_RU		= GUI_POINTER_RU,
	BROWSER_POINTER_LD		= GUI_POINTER_LD,
	BROWSER_POINTER_LU		= GUI_POINTER_LU,
	BROWSER_POINTER_RD		= GUI_POINTER_RD,
	BROWSER_POINTER_CROSS		= GUI_POINTER_CROSS,
	BROWSER_POINTER_MOVE		= GUI_POINTER_MOVE,
	BROWSER_POINTER_WAIT		= GUI_POINTER_WAIT,
	BROWSER_POINTER_HELP		= GUI_POINTER_HELP,
	BROWSER_POINTER_NO_DROP		= GUI_POINTER_NO_DROP,
	BROWSER_POINTER_NOT_ALLOWED	= GUI_POINTER_NOT_ALLOWED,
	BROWSER_POINTER_PROGRESS	= GUI_POINTER_PROGRESS,
	BROWSER_POINTER_AUTO
} browser_pointer_shape;


void browser_mouse_state_dump(browser_mouse_state mouse);

#endif
