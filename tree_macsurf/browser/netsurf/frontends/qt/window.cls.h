/*
 * Copyright 2023 Vincent Sanders <vince@netsurf-browser.org>
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
 * Window class for QT frontend.
 */

#include <QWidget>
#include <QScrollBar>
#include <QLabel>
#include <QWheelEvent>

extern "C" {
#include "utils/errors.h"

#include "netsurf/mouse.h"
#include "netsurf/window.h"
}

#include "qt/widget.cls.h"
#include "qt/urlbar.cls.h"

/**
 * Class for netsurf window
 */
class NS_Window : public QWidget
{
	Q_OBJECT

public:
	NS_Window(QWidget* parent, struct browser_window *bw);
	~NS_Window();

	void destroy(void);

	nserror advance_throbber(bool cont);

	/* static wrappers to be able to call instance methods */
	static nserror static_set_scroll(struct gui_window *gw, const struct rect *rect);
	static void static_set_status(struct gui_window *gw, const char *text);
	static void static_set_title(struct gui_window *gw, const char *title);
	static void static_set_icon(struct gui_window *gw, struct hlcache_handle *icon);
	static bool static_get_scroll(struct gui_window *gw, int *sx, int *sy);
	static nserror static_set_url(struct gui_window *gw, struct nsurl *url);

	static nserror static_event(struct gui_window *gw, enum gui_window_event event);
	static nserror static_invalidate(struct gui_window *gw, const struct rect *rect);
	static nserror static_get_dimensions(struct gui_window *gw, int *width, int *height);
	static void static_set_pointer(struct gui_window *gw, enum gui_pointer_shape shape);
	static void static_place_caret(struct gui_window *gw, int x, int y, int height, const struct rect *clip);
	static struct gui_window *static_create(struct browser_window *bw, struct gui_window *existing, gui_window_create_flags flags);
	static void static_destroy(struct gui_window *gw);

signals:
	void titleChanged(const char *title);
	void iconChanged(const QIcon &icon);

protected:
	void closeEvent(QCloseEvent *event);
	void wheelEvent(QWheelEvent *event);
	void keyPressEvent(QKeyEvent *event);

private:
	struct browser_window *m_bw;

	NS_Actions *m_actions;
	NS_URLBar *m_nsurlbar;
	NS_Widget *m_nswidget;
	QScrollBar *m_vscrollbar;
	QScrollBar *m_hscrollbar;
	/** status text label */
	QLabel *m_status;
	/** current throbber frame or 0 to use favicon */
	unsigned int m_throbber_frame;
	/** current favicon */
	QIcon *m_favicon;

	/**
	 * set the current position of the scroll bars
	 */
	nserror set_scroll(const struct rect *rect);

	/**
	 * set the status text
	 *
	 * @param text Status text to display
	 */
	void set_status(const char *text);

	/**
	 * Update the extent of the underlying canvas
	 */
	nserror update_extent();

	/**
	 * Set the favicoin
	 */
	void set_favicon(QIcon *icon);
};
