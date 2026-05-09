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
 * Scaffolding class for QT frontend.
 */

#include <QWidget>
#include <QTabWidget>

/**
 * Class for netsurf scaffolding
 */
class NS_Scaffold : public QTabWidget
{
	Q_OBJECT

public:
	NS_Scaffold(QWidget *parent);

	/* static acessors */
	static NS_Scaffold* get_scaffold(QWidget *page, bool use_current);

public slots:
	/**
	 * scaffolding slot to change tab title
	 *
	 * iterates all the tabs to check if the signal came from one on this
	 * notebook and if so update the title text.
	 */
	void changeTabTitle(const char *title);

	/**
	 * scaffolding slot to change tab icon
	 *
	 * iterates all the tabs to check if the signal came from one on this
	 * notebook and if so update the icon.
	 */
	void changeTabIcon(const QIcon &icon);

protected:
	void closeEvent(QCloseEvent *event);

private slots:
	void destroyTab(int index);
	void changeTab(int index);
	void newtab_slot(bool checked);
private:
	QAction *m_newtab;
};
