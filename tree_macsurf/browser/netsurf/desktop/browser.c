/*
 * Copyright 2019 Vincent Sanders <vince@netsurf-browser.org>
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
 *
 * Browser core functionality
 */

#include "utils/ns_errors.h"
#include "utils/log.h"
#include "utils/utils.h"
#include "netsurf/browser.h"
#include "css/utils.h"

/* exported interface documented in netsurf/browser.h */
nserror browser_set_dpi(int dpi)
{
	if (dpi < 72 || dpi > 250) {
		int bad = dpi;
		if (dpi < 72) dpi = 72;
		if (dpi > 250) dpi = 250;
		nslog_log(__FILE__, "", __LINE__,
			 "Clamping invalid DPI %d to %d", bad, dpi);
	}
	nscss_screen_dpi = INTTOFIX(dpi);

	return NSERROR_OK;
}

/* exported interface documented in netsurf/browser.h */
int browser_get_dpi(void)
{
	return FIXTOINT(nscss_screen_dpi);
}
