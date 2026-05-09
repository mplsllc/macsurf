/*
 * Copyright 2006 James Bursa <bursa@users.sourceforge.net>
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
 * Interface to browser history private operations
 */

#ifndef NETSURF_DESKTOP_BROWSER_HISTORY_PRIVATE_H
#define NETSURF_DESKTOP_BROWSER_HISTORY_PRIVATE_H

#include "content/handlers/css/utils.h"

#define LOCAL_HISTORY_WIDTH \
		(FIXTOINT(css_unit_css2device_px(INTTOFIX(116), nscss_screen_dpi)))
#define LOCAL_HISTORY_HEIGHT \
		(FIXTOINT(css_unit_css2device_px(INTTOFIX(100), nscss_screen_dpi)))
#define LOCAL_HISTORY_RIGHT_MARGIN \
		(FIXTOINT(css_unit_css2device_px(INTTOFIX( 50), nscss_screen_dpi)))
#define LOCAL_HISTORY_BOTTOM_MARGIN \
		(FIXTOINT(css_unit_css2device_px(INTTOFIX( 30), nscss_screen_dpi)))

#endif
