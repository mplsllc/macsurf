/*
 * Copyright 2008 Rob Kendrick <rjek@netsurf-browser.org>
 *
 * This file is part of NetSurf.
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
 * data scheme fetch handler interface.
 */

#ifndef NETSURF_CONTENT_FETCHERS_FETCH_DATA_H
#define NETSURF_CONTENT_FETCHERS_FETCH_DATA_H

/**
 * Register data scheme handler.
 *
 * \return NSERROR_OK on successful registration or error code on failure.
 */
nserror fetch_data_register(void);

#endif
