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
 * Implementation of netsurf widget for qt.
 */

#include <QWidget>
#include <QMenu>

extern "C" {
#include "netsurf/types.h"
#include "netsurf/content_type.h"
#include "netsurf/browser_window.h"
}

#include "qt/actions.cls.h"

class NS_Widget : public QWidget
{
	Q_OBJECT

public:
	NS_Widget(QWidget *parent, NS_Actions *actions, struct browser_window *bw);

	QSize sizeHint() const override;
	bool get_scroll(int *sx, int *sy);
	nserror get_dimensions(int *width, int *height);
	nserror invalidate(const struct rect *rect);
	void set_pointer(enum gui_pointer_shape shape);
	void setCaret(bool visible, int x, int y, int height);

public slots:
	void setHorizontalScroll(int value);
	void setVerticalScroll(int value);

protected:
	void contextMenuEvent(QContextMenuEvent *event);
	void focusOutEvent(QFocusEvent *event);
	void keyPressEvent(QKeyEvent *event);
	void mouseMoveEvent(QMouseEvent *event);
	void mousePressEvent(QMouseEvent *event);
	void mouseReleaseEvent(QMouseEvent *event);
	void paintEvent(QPaintEvent *event);
	void resizeEvent(QResizeEvent *event);

private:
	/**
	 * map qt event modifiers to browser mouse state keyboard modifiers
	 */
	static void event_to_bms_modifiers(QMouseEvent *event, unsigned int &bms);

	/**
	 * draw caret
	 */
	void redraw_caret(struct rect *clip, struct redraw_context *ctx);

	/**
	 * advance caret animation to next frame
	 */
	static void next_caret_frame(void *p);

	struct browser_window *m_bw;
	NS_Actions *m_actions;
	int m_xoffset;
	int m_yoffset;
	enum gui_pointer_shape m_pointer_shape; /**< current pointer shape */
	QMenu *m_contextmenu;

	QPointF m_press_pos; /**< position of last mouse press event */
	unsigned int m_drag_state; /**< current drag state */

	int m_caret_frame; /**< current caret frame 0 is not shown */
	int m_caret_x; /**< x position of the caret */
	int m_caret_y; /**< y position of the caret */
	int m_caret_h; /**< height of the caret */
};
