/*
 * Copyright 2014 vincent Sanders <vince@netsurf-browser.org>
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
 * Interface to system colour values.
 *
 * Netsurf has a list of user configurable colours with frontend
 * specific defaults. These colours are used for the css system
 * colours and to colour and style internally rendered widgets
 * (e.g. cookies treeview or local file directory views.
 */

#ifndef NETSURF_DESKTOP_SYSTEM_COLOUR_H
#define NETSURF_DESKTOP_SYSTEM_COLOUR_H

#include <libcss/libcss.h>

#include "utils/errors.h"
#include "netsurf/types.h"

/**
 * css callback to obtain named system colour.
 *
 * \param[in] pw context unused in implementation
 * \param[in] name The name of the colour being looked up
 * \param[out] color The system colour associated with the name.
 * \return CSS_OK and \a color updated on success else CSS_INVALID if
 *          the \a name is unrecognised
 */
css_error ns_system_colour(void *pw, lwc_string *name, css_color *color);


/**
 * Obtain a system colour from a name.
 *
 * \param[in] name The name of the colour being looked up
 * \param[out] color The system colour associated with the name in the
 *                     netsurf colour representation.
 * \return NSERROR_OK and \a color updated on success else appropriate
 *           error code.
 */
nserror ns_system_colour_char(const char *name, colour *color);


/**
 * Initialise the system colours
 *
 * \return NSERROR_OK on success else appropriate error code.
 */
nserror ns_system_colour_init(void);


/**
 * release any resources associated with the system colours.
 */
void ns_system_colour_finalize(void);

#endif
