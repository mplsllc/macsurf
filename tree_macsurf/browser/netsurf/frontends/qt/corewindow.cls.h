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
 * Implementation of corewindow for qt.
 */

#pragma once

#include <QWidget>

extern "C" {
#include "utils/errors.h"
#include "netsurf/core_window.h"
#include "netsurf/mouse.h"
}

class NS_Corewindow : public QWidget
{
	Q_OBJECT

public:
	NS_Corewindow(QWidget *parent, Qt::WindowFlags f = Qt::Window);

	static nserror static_invalidate(struct core_window *cw, const struct rect *rect);
	static nserror static_set_extent(struct core_window *cw, int width, int height);
	static nserror static_set_scroll(struct core_window *cw, int x, int y);
	static nserror static_get_scroll(const struct core_window *cw, int *x, int *y);
	static nserror static_get_dimensions(const struct core_window *cw, int *width, int *height);
	static nserror static_drag_status(struct core_window *cw, core_window_drag_status ds);

protected:
	void paintEvent(QPaintEvent *event);
	void keyPressEvent(QKeyEvent *event);
	void mouseMoveEvent(QMouseEvent *event);
	void mousePressEvent(QMouseEvent *event);
	void mouseReleaseEvent(QMouseEvent *event);

	struct nsqt_core_window *m_core_window;
private:
	virtual void draw(struct rect *clip, struct redraw_context *ctx) = 0;
	virtual bool key_press(uint32_t nskey) = 0;
	virtual void mouse_action(browser_mouse_state mouse_state, int x, int y)=0;
	int m_xoffset;
	int m_yoffset;
};
