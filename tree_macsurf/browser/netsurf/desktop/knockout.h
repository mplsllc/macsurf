/*
 * Copyright 2006 Richard Wilson <info@tinct.net>
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

/** \file
 * Knockout rendering (interface).
 */

#ifndef _NETSURF_DESKTOP_KNOCKOUT_H_
#define _NETSURF_DESKTOP_KNOCKOUT_H_

#include "netsurf/plotters.h"


/**
 * Start a knockout plotting session
 *
 * \param ctx the redraw context with real plotter table
 * \param knk_ctx updated to copy of ctx, with plotter table replaced
 * \return true on success, false otherwise
 */
bool knockout_plot_start(const struct redraw_context *ctx,
		struct redraw_context *knk_ctx);
/**
 * End a knockout plotting session
 *
 * \return true on success, false otherwise
 */
bool knockout_plot_end(const struct redraw_context *ctx);

extern const struct plotter_table knockout_plotters;

#endif
