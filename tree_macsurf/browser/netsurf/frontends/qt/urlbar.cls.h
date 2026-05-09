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
 * Ineterface of netsurf window for qt.
 */

#include <QToolBar>
#include <QLineEdit>
#include <QMenu>
#include <QToolButton>

extern "C" {

#include "netsurf/types.h"
#include "netsurf/content_type.h"
#include "netsurf/browser_window.h"

}

#include "qt/actions.cls.h"

/**
 * plot scale text into icon
 *
 * idealy the scale widget should be in the line edit but no text gets
 * plotted if it is. This is because QLineEditIconButton::paintEvent() assumes
 * an icon is a pixmap and has no provision for text.
 *
 * defining this switches to plotting text into a pixmap and uses the icon in
 *  the line edit.
 */
#undef USE_ICON_FOR_SCALE
//#define USE_ICON_FOR_SCALE

class NS_URLBar :public QToolBar
{
	Q_OBJECT

public:
	NS_URLBar(QWidget *parent, NS_Actions *actions, struct browser_window *bw);
	nserror set_url(struct nsurl *url);

public slots:
	void input_pressed();

private:
	struct browser_window *m_bw;
	QLineEdit *m_input;
	QMenu *m_burgermenu;
	QToolButton *m_burgerbutton;
};
