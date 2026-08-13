/*
 * Copyright 2009 John-Mark Bell <jmb@netsurf-browser.org>
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

#ifndef NETSURF_CSS_INTERNAL_H_
#define NETSURF_CSS_INTERNAL_H_

/**
 * URL resolution callback for libcss
 *
 * \param pw    Resolution context
 * \param base  Base URI
 * \param rel   Relative URL
 * \param abs   Pointer to location to receive resolved URL
 * \return CSS_OK       on success,
 *         CSS_NOMEM    on memory exhaustion,
 *         CSS_INVALID  if resolution failed.
 */
css_error nscss_resolve_url(void *pw, const char *base, lwc_string *rel, lwc_string **abs);

/* fixes202: run the inline-style-relevant rewrite passes against a buffer.
 * Returns a freshly-allocated rewritten buffer (caller frees) and writes
 * the new size to *out_size_p; returns NULL when no rewrite applies (the
 * caller should use the original buffer). Defined in cssh_css.c. */
#include <stddef.h>
char *macsurf__rewrite_inline_style(const char *data, size_t in_size,
		size_t *out_size_p);

/* fixes1161c: grid-template-columns/-rows rewrite, exposed (was static)
 * so the Linux harness can run the real preprocessing in --layout mode.
 * Same contract as macsurf__rewrite_inline_style: fresh malloc'd buffer,
 * caller frees, NULL if no rewrite applied. Defined in cssh_css.c. */
char *macsurf__rewrite_grid_template_columns(const char *data, size_t size);
char *macsurf__rewrite_grid_template_rows(const char *data, size_t size);

#endif
