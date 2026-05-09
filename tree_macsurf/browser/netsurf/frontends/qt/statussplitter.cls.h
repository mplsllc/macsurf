/*
 * Copyright 2025 Vincent Sanders <vince@netsurf-browser.org>
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
 * Status splitter.
 */

#include <QSplitter>
#include <QLabel>
#include <QScrollBar>
#include <QResizeEvent>

/**
 * splitter widget for status line
 *
 * this ensures the split between ststus and scrollbar remains at the configured
 * percentage and updates the option when the handle is moved.
 */
class NS_StatusSplitter: public QSplitter
{
	Q_OBJECT

public:
	NS_StatusSplitter(QLabel *status, QScrollBar *scrollbar, QWidget *parent=nullptr);

private slots:
	void moved_slot(int pos, int index);

private:
	void resizeEvent(QResizeEvent *event);
	bool m_resize_move;
};
