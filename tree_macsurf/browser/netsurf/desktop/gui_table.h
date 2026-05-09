/*
 * Copyright 2014 Vincent Sanders <vince@netsurf-browser.org>
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
 * top level interface table.
 *
 * \note This should probably not be included directly but rather
 * through netsurf.h or gui_internal.h
 */

#ifndef _NETSURF_DESKTOP_GUI_TABLE_H_
#define _NETSURF_DESKTOP_GUI_TABLE_H_

struct gui_misc_table;
struct gui_window_table;
struct gui_download_table;
struct gui_clipboard_table;
struct gui_fetch_table;
struct gui_file_table;
struct gui_utf8_table;
struct gui_search_table;
struct gui_search_web_table;
struct gui_llcache_table;
struct gui_bitmap_table;
struct gui_layout_table;

/**
 * NetSurf operation function table
 *
 * Function table implementing interface operations for the browser core.
 */
struct netsurf_table {

	/**
	 * Browser table.
	 *
	 * Provides miscellaneous browser functionality.
	 *
	 * The table is mandantory and must be provided.
	 */
	struct gui_misc_table *misc;

	/**
	 * Window table.
	 *
	 * Provides all operations which affect a frontends display window.
	 *
	 * The table is mandantory and must be provided.
	 */
	struct gui_window_table *window;

	/**
	 * Core window table.
	 *
	 * Provides all operations which affect core ui windows of a frontend.
	 *
	 * The table is optional and may be NULL which does nothing.
	 */
	struct core_window_table *corewindow;

	/**
	 * Download table.
	 *
	 * operations table for the download windows.
	 */
	struct gui_download_table *download;

	/**
	 * Clipboard table.
	 */
	struct gui_clipboard_table *clipboard;

	/**
	 * Fetcher table
	 *
	 * The table is mandantory and must be provided.
	 */
	struct gui_fetch_table *fetch;

	/**
	 * File table
	 *
	 * Provides file and filename operations to the core.
	 *
	 * The table is optional and may be NULL in which case the
	 * default posix compliant operations will be used.
	 */
	struct gui_file_table *file;

	/**
	 * UTF8 table.
	 *
	 * Provides for conversion between the gui local character
	 * encoding and utf8.
	 *
	 * The table optional and may be NULL which implies the local
	 * encoding is utf8.
	 */
	struct gui_utf8_table *utf8;

	/**
	 * Page search table.
	 *
	 * Provides routines for the interactive text search on a page.
	 */
	struct gui_search_table *search;

	/**
	 * Web search table.
	 *
	 * Used by the web search provider system.
	 *
	 * The table is optional and may be NULL which uses the
	 * default empty implementation.
	 */
	struct gui_search_web_table *search_web;

	/**
	 * Low level cache table.
	 *
	 * Used by the low level cache to push objects to persistant
	 * storage.
	 *
	 * The table is optional and may be NULL which uses the
	 * default implementation.
	 */
	struct gui_llcache_table *llcache;

	/**
	 * Bitmap table.
	 *
	 * Used by the image convertors as a generic interface to
	 * native platform-specific image formats.
	 *
	 * The table is mandantory and must be provided.
	 */
	struct gui_bitmap_table *bitmap;

	/**
	 * Layout table
	 *
	 * Used by the layout process to measure glyphs in a frontend
	 * specific manner.
	 *
	 * The table is mandantory and must be provided.
	 */
	struct gui_layout_table *layout;
};

#endif
