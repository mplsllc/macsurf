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
 * Declaration of netsurf list selection widget for qt.
 */

#include <QWidget>
#include <QListWidget>
#include <QToolButton>
#include <QScrollBar>

/**
 * qlist widget that sizes itself to first column
 */
class FirstListWidget:public QListWidget
{
public:
	QSize sizeHint() const final {
		int width = sizeHintForColumn(0) + frameWidth() * 2 + 5;
		width += verticalScrollBar()->sizeHint().width();
		return QSize(width, 100);
	}
};

/**
 * list selection allows selection of one or more items from one list in a second
 */
class NS_ListSelection: public QWidget
{
	Q_OBJECT

public:
	NS_ListSelection(QWidget *parent=nullptr);
	void addItem(const char *label, const char *data);
	void addItem(QByteArray &label, QByteArray &data);
	void selectItem(const char *data);
	void deselectAll();
	QList<QByteArray> selection();

public slots:
	void addtoselection(bool checked);
	void remfromselection(bool checked);
	void selectionup(bool checked);
	void selectiondown(bool checked);
private:
	enum ItemDataRole { DataRole = Qt::UserRole, SourcePosRole };
	void selectRow(int row); /**< move row from source to selected */
	void deselectRow(int row); /**< move row from selected to source */

	FirstListWidget *m_source; /**< source list view */
	FirstListWidget *m_selected; /**< selected list view */
	QToolButton *m_add; /**< button that moves item from source to selected */
	QToolButton *m_rem; /**< button that moves item from selected to source */
	QToolButton *m_sel_up;
	QToolButton *m_sel_down;
	int m_count; /**< number of items added */
};
