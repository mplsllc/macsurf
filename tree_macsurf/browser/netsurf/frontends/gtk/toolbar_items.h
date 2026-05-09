/*
 * Copyright 2012 Vincent Sanders <vince@netsurf-browser.org>
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

#ifndef NETSURF_GTK_TOOLBAR_ITEMS_H
#define NETSURF_GTK_TOOLBAR_ITEMS_H

typedef enum {
	BACK_BUTTON = 0,
	HISTORY_BUTTON,
	FORWARD_BUTTON,
	RELOADSTOP_BUTTON,
	URL_BAR_ITEM,
	WEBSEARCH_ITEM,
	OPENMENU_BUTTON,
	STOP_BUTTON,
	RELOAD_BUTTON,
	HOME_BUTTON,
	THROBBER_ITEM,
	NEWWINDOW_BUTTON,
	NEWTAB_BUTTON,
	OPENFILE_BUTTON,
	CLOSETAB_BUTTON,
	CLOSEWINDOW_BUTTON,
	SAVEPAGE_BUTTON,
	PDF_BUTTON,
	PLAINTEXT_BUTTON,
	DRAWFILE_BUTTON,
	POSTSCRIPT_BUTTON,
	PRINTPREVIEW_BUTTON,
	PRINT_BUTTON,
	QUIT_BUTTON,
	CUT_BUTTON,
	COPY_BUTTON,
	PASTE_BUTTON,
	DELETE_BUTTON,
	SELECTALL_BUTTON,
	FIND_BUTTON,
	PREFERENCES_BUTTON,
	ZOOMPLUS_BUTTON,
	ZOOMMINUS_BUTTON,
	ZOOMNORMAL_BUTTON,
	FULLSCREEN_BUTTON,
	VIEWSOURCE_BUTTON,
	DOWNLOADS_BUTTON,
	SAVEWINDOWSIZE_BUTTON,
	TOGGLEDEBUGGING_BUTTON,
	SAVEBOXTREE_BUTTON,
	SAVEDOMTREE_BUTTON,
	LOCALHISTORY_BUTTON,
	GLOBALHISTORY_BUTTON,
	ADDBOOKMARKS_BUTTON,
	SHOWBOOKMARKS_BUTTON,
	SHOWCOOKIES_BUTTON,
	OPENLOCATION_BUTTON,
	NEXTTAB_BUTTON,
	PREVTAB_BUTTON,
	CONTENTS_BUTTON,
	GUIDE_BUTTON,
	INFO_BUTTON,
	ABOUT_BUTTON,
	CUSTOMIZE_BUTTON,
	PLACEHOLDER_BUTTON /* size indicator; array maximum indices */
} nsgtk_toolbar_button;    /* PLACEHOLDER_BUTTON - 1 */

#endif

/*
 * Item fields are:
 *   - item identifier (enum value)
 *   - name (identifier)
 *   - initial sensitivity (true/false)
 *   - if there is a toolbar click signal handler (y/n) and it is available in
 *          the toolbar and toolbox as a button (b, implies y) if the item is
 *          available as a button but not placed in the toolbox (t, implies y)
 *   - if there is a menu activate signal handler (y/n) and it calls the
 *          toolbar click handler directly. (p, implies y)
 *   - item label as a netsurf message (identifier)
 *   - icon image name ("string")
 */

#ifndef TOOLBAR_ITEM
#define TOOLBAR_ITEM(a, b, c, d, e, f, g)
#define TOOLBAR_ITEM_SET
#endif

TOOLBAR_ITEM(BACK_BUTTON, back, false, b, p, gtkBack, "go-previous")
TOOLBAR_ITEM(HISTORY_BUTTON, history, true, y, n, , "local-history")
TOOLBAR_ITEM(FORWARD_BUTTON, forward, false, b, p, gtkForward, "go-next")
TOOLBAR_ITEM(STOP_BUTTON, stop, false, t, p, gtkStop, NSGTK_STOCK_STOP)
TOOLBAR_ITEM(RELOAD_BUTTON, reload, true, t, p, Reload, NSGTK_STOCK_REFRESH)
TOOLBAR_ITEM(HOME_BUTTON, home, true, b, p, gtkHome, NSGTK_STOCK_HOME)
TOOLBAR_ITEM(URL_BAR_ITEM, url_bar, true, n, n, , NULL)
TOOLBAR_ITEM(WEBSEARCH_ITEM, websearch, true, n, n, , NULL)
TOOLBAR_ITEM(THROBBER_ITEM, throbber, true, n, n, , NULL)
TOOLBAR_ITEM(NEWWINDOW_BUTTON, newwindow, true, b, p, gtkNewWindow, "document-new")
TOOLBAR_ITEM(NEWTAB_BUTTON, newtab, true, b, p, gtkNewTab, NSGTK_STOCK_ADD)
TOOLBAR_ITEM(OPENFILE_BUTTON, openfile, true, b, p, gtkOpenFile, "document-open")
TOOLBAR_ITEM(CLOSETAB_BUTTON, closetab, false, n, y, , "window-close")
TOOLBAR_ITEM(CLOSEWINDOW_BUTTON, closewindow, true, y, p, , "window-close")
TOOLBAR_ITEM(SAVEPAGE_BUTTON, savepage, true, b, p, gtkSavePage, "text-html")
TOOLBAR_ITEM(PDF_BUTTON, pdf, false, y, p, , "x-office-document")
TOOLBAR_ITEM(PLAINTEXT_BUTTON, plaintext, true, b, p, gtkPlainText, "text-x-generic")
TOOLBAR_ITEM(DRAWFILE_BUTTON, drawfile, false, n, n, , NULL)
TOOLBAR_ITEM(POSTSCRIPT_BUTTON, postscript, false, n, n, , NULL)
TOOLBAR_ITEM(PRINTPREVIEW_BUTTON, printpreview, false, n, p, gtkPrintPreview, "gtk-print-preview")
TOOLBAR_ITEM(PRINT_BUTTON, print, true, b, p, gtkPrint, "document-print")
TOOLBAR_ITEM(QUIT_BUTTON, quit, true, b, p, gtkQuitMenu, "application-exit")
TOOLBAR_ITEM(CUT_BUTTON, cut, true, b, p, gtkCut, "edit-cut")
TOOLBAR_ITEM(COPY_BUTTON, copy, true, b, p, gtkCopy, "edit-copy")
TOOLBAR_ITEM(PASTE_BUTTON, paste, true, b, p, gtkPaste, "edit-paste")
TOOLBAR_ITEM(DELETE_BUTTON, delete, false, b, p, gtkDelete, "edit-delete")
TOOLBAR_ITEM(SELECTALL_BUTTON, selectall, true, b, p, gtkSelectAll, "edit-select-all")
TOOLBAR_ITEM(FIND_BUTTON, find, true, n, y, gtkFind, "edit-find")
TOOLBAR_ITEM(PREFERENCES_BUTTON, preferences, true, b, p, gtkPreferences, "preferences-system")
TOOLBAR_ITEM(ZOOMPLUS_BUTTON, zoomplus, true, b, p, gtkZoomPlus, "gtk-zoom-in")
TOOLBAR_ITEM(ZOOMMINUS_BUTTON, zoomminus, true, b, p, gtkZoomMinus, "gtk-zoom-out")
TOOLBAR_ITEM(ZOOMNORMAL_BUTTON, zoomnormal, true, b, p, gtkZoomNormal, "gtk-zoom-100")
TOOLBAR_ITEM(FULLSCREEN_BUTTON, fullscreen, true, b, p, gtkFullScreen, "gtk-fullscreen")
TOOLBAR_ITEM(VIEWSOURCE_BUTTON, viewsource, true, b, p, gtkPageSource, "gtk-index")
TOOLBAR_ITEM(DOWNLOADS_BUTTON, downloads, true, b, p, gtkDownloads, NSGTK_STOCK_SAVE_AS)
TOOLBAR_ITEM(SAVEWINDOWSIZE_BUTTON, savewindowsize, true, y, p, gtkSaveWindowSize, NULL)
TOOLBAR_ITEM(TOGGLEDEBUGGING_BUTTON, toggledebugging, true, y, p, gtkToggleDebugging, NULL)
TOOLBAR_ITEM(SAVEBOXTREE_BUTTON, debugboxtree, true, y, p, gtkDebugBoxTree, NULL)
TOOLBAR_ITEM(SAVEDOMTREE_BUTTON, debugdomtree, true, y, p, gtkDebugDomTree, NULL)
TOOLBAR_ITEM(LOCALHISTORY_BUTTON, localhistory, true, y, p, , NULL)
TOOLBAR_ITEM(GLOBALHISTORY_BUTTON, globalhistory, true, y, p, gtkGlobalHistory, NULL)
TOOLBAR_ITEM(ADDBOOKMARKS_BUTTON, addbookmarks, true, y, p, gtkAddBookMarks, NULL)
TOOLBAR_ITEM(SHOWBOOKMARKS_BUTTON, showbookmarks, true, b, p, gtkShowBookMarks, "user-bookmarks")
TOOLBAR_ITEM(SHOWCOOKIES_BUTTON, showcookies, true, b, p, gtkShowCookies, "show-cookie")
TOOLBAR_ITEM(OPENLOCATION_BUTTON, openlocation, true, y, p, gtkOpenLocation, NULL)
TOOLBAR_ITEM(NEXTTAB_BUTTON, nexttab, false, n, y, gtkNextTab, "media-skip-forward")
TOOLBAR_ITEM(PREVTAB_BUTTON, prevtab, false, n, y, gtkPrevTab, "media-skip-backward")
TOOLBAR_ITEM(CONTENTS_BUTTON, contents, true, y, p, gtkContents, "gtk-help")
TOOLBAR_ITEM(GUIDE_BUTTON, guide, true, y, p, gtkGuide, "gtk-help")
TOOLBAR_ITEM(INFO_BUTTON, info, true, y, p, gtkUserInformation, "dialog-information")
TOOLBAR_ITEM(ABOUT_BUTTON, about, true, b, p, gtkAbout, "help-about")
TOOLBAR_ITEM(OPENMENU_BUTTON, openmenu, true, b, n, gtkOpenMenu, NSGTK_STOCK_OPEN_MENU)
TOOLBAR_ITEM(CUSTOMIZE_BUTTON, cutomize, true, y, p, , NULL)
TOOLBAR_ITEM(RELOADSTOP_BUTTON, reloadstop, true, b, n, Reload, NSGTK_STOCK_REFRESH)

#ifdef TOOLBAR_ITEM_SET
#undef TOOLBAR_ITEM
#undef TOOLBAR_ITEM_SET
#endif
