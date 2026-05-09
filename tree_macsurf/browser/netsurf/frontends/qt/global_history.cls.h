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
 * Implementation of global history widget for qt.
 */

#include "qt/corewindow.cls.h"

class NS_Global_history : public NS_Corewindow
{
	Q_OBJECT

public:
	NS_Global_history(QWidget *parent);
	~NS_Global_history();

private:
	void draw(struct rect *clip, struct redraw_context *ctx);
	bool key_press(uint32_t nskey);
	void mouse_action(browser_mouse_state mouse_state, int x, int y);
};
