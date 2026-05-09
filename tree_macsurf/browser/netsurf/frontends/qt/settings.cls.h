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

#include <QWidget>
#include <QLabel>
#include <QListWidget>
#include <QStackedWidget>
#include <QSpinBox>
#include <QDialogButtonBox>
#include <QLineEdit>

class AbstractSettingsCategory: public QWidget
{
public:
	AbstractSettingsCategory(QWidget *parent):QWidget(parent) { }
	virtual const char *categoryName() = 0;
	virtual void categoryRealize() = 0;
	virtual void categoryApply() = 0;
};

/**
 * Class for netsurf settings window
 */
class NS_Settings : public QWidget
{
	Q_OBJECT

public:
	NS_Settings(QWidget *parent);

public slots:
	void categorychanged_slot(const QString &currentText);
	void clicked_slot(QAbstractButton *button);

protected:
	void showEvent(QShowEvent *event);

private:
	void addCategory(AbstractSettingsCategory *widget);

	QLabel *m_headerlabel;
	QListWidget *m_categorylist;
	QStackedWidget *m_categories;
	QDialogButtonBox *m_buttonbox;
};
