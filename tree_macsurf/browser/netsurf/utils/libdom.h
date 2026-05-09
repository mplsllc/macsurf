/*
 * Copyright 2011 John-Mark Bell <jmb@netsurf-browser.org>
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
 * libdom utilities (implementation).
 */

#ifndef NETSURF_UTILS_LIBDOM_H_
#define NETSURF_UTILS_LIBDOM_H_

#include <stdbool.h>
#include <stdio.h>

#include <dom/dom.h>

#include <dom/bindings/hubbub/parser.h>
#include <dom/bindings/hubbub/errors.h>

/**
 * Search children of a node for first named element 
 * \param  parent dom_node to search children of, or NULL
 * \param  element_name	name of element to find
 * \return  first child of node which is an element and matches name, or
 *          NULL if not found or parameter node is NULL
 */
dom_node *libdom_find_first_element(dom_node *parent, lwc_string *element_name);

typedef nserror (*libdom_iterate_cb)(dom_node *node, void *ctx);

nserror libdom_iterate_child_elements(dom_node *parent,
		libdom_iterate_cb cb, void *ctx);

nserror libdom_parse_file(const char *filename, const char *encoding,
		dom_document **doc);

/**
 * Convert libdom hubbub binding errors to nserrors.
 *
 * \param error The hubbub binding error to convert
 * \return The appropriate nserror
 */
nserror libdom_hubbub_error_to_nserror(dom_hubbub_error error);

/**
 * Walk though a DOM (sub)tree, in depth first order, printing DOM structure.
 *
 * \param node The root node to start from.
 * \param f The file to write output into.
 * \param depth The depth of 'node' in the (sub)tree.
 */
nserror libdom_dump_structure(dom_node *node, FILE *f, int depth);

#endif
