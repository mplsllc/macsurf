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
 * Scaffold style class for QT frontend.
 */

#include <QWidget>
#include <QProxyStyle>

/**
 * Class for netsurf scaffold style
 */
class ScaffoldStyle: public QProxyStyle
{
	Q_OBJECT
public:
	explicit ScaffoldStyle(QObject *parent = nullptr);

	virtual QRect subElementRect(SubElement subElement, const QStyleOption *option, const QWidget *widget) const override;
};
