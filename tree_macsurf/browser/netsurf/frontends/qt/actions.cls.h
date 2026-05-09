/*
 * Copyright 2024 Vincent Sanders <vince@netsurf-browser.org>
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
 * Interface of netsurf qt actions.
 *
 * this wraps all the interface actions in one place to be used in toolbars or
 * menus as needed.
 */

#pragma once

#include <QAction>
#include <QToolButton>
#include <QWidgetAction>

extern "C" {

#include "netsurf/types.h"
#include "netsurf/content_type.h"
#include "netsurf/browser_window.h"

}

class NS_Actions :public QObject
{
	Q_OBJECT

public:
	NS_Actions(QWidget* parent, struct browser_window *bw);
	~NS_Actions();

	/**
	 * Activity update states
	 */
	enum Update {
		UpdateInactive,
		UpdateActive,
		UpdateUnchanged,
		UpdatePageInfo,
		UpdateBookmarks,
		UpdatePageScale,
	};
	/**
	 * Change action state appropriate for flag
	 */
	void update(NS_Actions::Update update = UpdateUnchanged);
        /**
	 * change action states associated with menu context
	 */
	void update(struct nsurl *link, struct hlcache_handle *object, char *selection);

	/**
	 * create instance of widget action for use for a page scale menu entry
	 *
	 * \return instance of a new widget action
	 */
	QWidgetAction *page_scale_widget_action(QWidget* parent);

	QAction *m_back; /**< Navigate to previous page */
	QAction *m_forward; /**< Navigate to subsequent page */
	QAction *m_stop_reload; /**< Stop navigation or reload page */
	QAction *m_settings; /**< Open settings dialog */
	QAction *m_bookmarks; /**< Manage bookmarks */
	QAction *m_add_edit_bookmark; /**< Add or edit a bookmark */
	QAction *m_local_history; /**< show history for single context */
	QAction *m_global_history; /**< show global history */
	QAction *m_cookies; /**< manage cookies */
	QAction *m_page_info; /**< display page information */
	QAction *m_page_scale; /**< current page scaling */
	QAction *m_reset_page_scale; /**< reset current page scale to default */
	QAction *m_reduce_page_scale; /**< reduce page scale by a step */
	QAction *m_increase_page_scale; /**< increase page scale by a step */
	QAction *m_newtab; /**< create a new browsing context in a tab */
	QAction *m_newwindow; /**< create a new browsing window */
	QAction *m_quit; /**< quit the browser */
	QAction *m_page_save; /**< save a copy of the page */
	QAction *m_page_source; /**< display page source */
	QAction *m_debug_render; /**< toggle debug rendering */
	QAction *m_debug_box_tree; /**< debug the box tree */
	QAction *m_debug_dom_tree; /**< debug the DOM tree */
	QAction *m_about_netsurf; /**< show info about the browser */
	QAction *m_link_new_tab; /**< open link in new tab */
	QAction *m_link_new_window; /**< open link in new window */
	QAction *m_link_bookmark; /**<  bookmark link */
	QAction *m_link_save; /**< save link */
	QAction *m_link_copy; /**< copy link address */
	QAction *m_img_new_tab; /**< open image in new tab */
	QAction *m_img_save; /**< save image */
	QAction *m_img_copy; /**< copy image address */
	QAction *m_obj_save; /**< save object */
	QAction *m_obj_copy; /**< copy object address to clipboard */
	QAction *m_sel_copy; /**< copy selection to clipboard */
	QAction *m_sel_search; /**< search selection */

private slots:
	void back_slot(bool checked);
	void forward_slot(bool checked);
	void stop_reload_slot(bool checked);
	void settings_slot(bool checked);
	void bookmarks_slot(bool checked);
	void add_edit_bookmark_slot(bool checked);
	void local_history_slot(bool checked);
	void global_history_slot(bool checked);
	void cookies_slot(bool checked);
	void page_info_slot(bool checked);
	void reset_page_scale_slot(bool checked);
	void reduce_page_scale_slot(bool checked);
	void increase_page_scale_slot(bool checked);
	void newtab_slot(bool checked);
	void newwindow_slot(bool checked);
	void quit_slot(bool checked);
	void page_save_slot(bool checked);
	void page_source_slot(bool checked);
	void debug_render_slot(bool checked);
	void debug_box_tree_slot(bool checked);
	void debug_dom_tree_slot(bool checked);
	void about_netsurf_slot(bool checked);
	void link_new_tab_slot(bool checked);
	void link_new_window_slot(bool checked);
	void link_bookmark_slot(bool checked);
	void link_save_slot(bool checked);
	void link_copy_slot(bool checked);
	void img_new_tab_slot(bool checked);
	void img_save_slot(bool checked);
	void obj_save_slot(bool checked);
	void obj_copy_slot(bool checked);
	void sel_copy_slot(bool checked);
	void sel_search_slot(bool checked);

private:
	/**
	 * obtain the bottom left corner global location of an action attached
	 * widget.
	 */
	static QPoint actionGlobal(QAction *action);

	/**
	 * get toolbutton associated with action
	 */
	static QToolButton *QToolButtonFromQAction(QAction *action);

	/**
	 * generate a QIcon from a text string
	 */
	static QIcon QIconFromText(QString text);

	/**
	 * generate a QString from a url
	 */
	static QString QStringFromNsurl(struct nsurl *url);

	/**
	 * Change the current active navigation state for this browsing context.
	 */
	void update_navigation(NS_Actions::Update update);

	/**
	 * Change actions related to page information
	 */
	void update_page_info();

	/**
	 * Change actions related to page scaling (zoom)
	 */
	void update_page_scale();

	/**
	 * Change actions related to bookmarks
	 */
	void update_bookmarks();

	/**
	 * change page scale in discrete steps
	 */
	void change_page_scale(int step);

	/** Current browser window handle */
	struct browser_window *m_bw;

	/** Current navigation state */
	bool m_active;

	/** current page url is bookmarked */
	bool m_marked;

	/** Current page information state */
	browser_window_page_info_state m_pistate;

	struct nsurl *m_link;
	struct hlcache_handle *m_object;
	char *m_selection;
};
