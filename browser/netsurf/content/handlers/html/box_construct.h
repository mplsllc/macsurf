/*
 * Copyright 2020 Vincent Sanders <vince@netsurf-browser.org>
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
 * HTML Box tree construction interface.
 *
 * This stage of rendering converts a tree of dom_nodes (produced by libdom)
 * to a tree of struct box. The box tree represents the structure of the
 * document as given by the CSS display and float properties.
 *
 * For example, consider the following HTML:
 * \code
 *   <h1>Example Heading</h1>
 *   <p>Example paragraph <em>with emphasised text</em> etc.</p>       \endcode
 *
 * This would produce approximately the following box tree with default CSS
 * rules:
 * \code
 *   BOX_BLOCK (corresponds to h1)
 *     BOX_INLINE_CONTAINER
 *       BOX_INLINE "Example Heading"
 *   BOX_BLOCK (p)
 *     BOX_INLINE_CONTAINER
 *       BOX_INLINE "Example paragraph "
 *       BOX_INLINE "with emphasised text" (em)
 *       BOX_INLINE "etc."                                             \endcode
 *
 * Note that the em has been collapsed into the INLINE_CONTAINER.
 *
 * If these CSS rules were applied:
 * \code
 *   h1 { display: table-cell }
 *   p { display: table-cell }
 *   em { float: left; width: 5em }                                    \endcode
 *
 * then the box tree would instead look like this:
 * \code
 *   BOX_TABLE
 *     BOX_TABLE_ROW_GROUP
 *       BOX_TABLE_ROW
 *         BOX_TABLE_CELL (h1)
 *           BOX_INLINE_CONTAINER
 *             BOX_INLINE "Example Heading"
 *         BOX_TABLE_CELL (p)
 *           BOX_INLINE_CONTAINER
 *             BOX_INLINE "Example paragraph "
 *             BOX_FLOAT_LEFT (em)
 *               BOX_BLOCK
 *                 BOX_INLINE_CONTAINER
 *                   BOX_INLINE "with emphasised text"
 *             BOX_INLINE "etc."                                       \endcode
 *
 * Here implied boxes have been added and a float is present.
 */

#ifndef NETSURF_HTML_BOX_CONSTRUCT_H
#define NETSURF_HTML_BOX_CONSTRUCT_H

/**
 * Construct a box tree from a dom and html content
 *
 * \param n dom document
 * \param c content of type CONTENT_HTML to construct box tree in
 * \param cb callback to report conversion completion
 * \param box_conversion_context pointer that recives the conversion context
 * \return netsurf error code indicating status of call
 */
nserror dom_to_box(struct dom_node *n, struct html_content *c, box_construct_complete_cb cb, void **box_conversion_context);


/**
 * aborts any ongoing box construction
 */
nserror cancel_dom_to_box(void *box_conversion_context);


/**
 * fixes130b: re-cascade an existing box tree in place.
 *
 * Walks the existing box tree top-down. For every box that has a
 * DOM node and cached select results, calls box_get_style with the
 * current html_content state (which includes dynamic pseudo-class
 * pointers dyn_hover_node / dyn_active_node / dyn_focus_node) and
 * swaps in the freshly computed css_select_results. Used after a
 * hover/active/focus state change to make :hover etc. take effect
 * without doing a full DOM-to-box reconstruction.
 *
 * \param c html_content whose box tree to re-cascade
 * \return NSERROR_OK on success
 */
nserror html_recascade_tree(struct html_content *c);

/* Select CSS for an inline-SVG descendant. SVG shape nodes deliberately do
 * not have layout boxes, so the SVG painter uses this to run the same author
 * cascade and custom-property inheritance as ordinary HTML elements. */
css_select_results *html_svg_get_style(const struct html_content *c,
		struct dom_node *node,
		const css_computed_style *parent_style,
		css_custom_env *parent_custom_env,
		css_custom_env **out_custom_env);


/**
 * Retrieve the box for a dom node, if there is one
 *
 * \param node The DOM node
 * \return The box if there is one
 */
struct box *box_for_node(struct dom_node *node);

/**
 * Extract a URL from a relative link, handling junk like whitespace and
 * attempting to read a real URL from "javascript:" links.
 *
 * \param content html content
 * \param dsrel relative URL text taken from page
 * \param base base for relative URLs
 * \param result updated to target URL on heap, unchanged if extract failed
 * \return true on success, false on memory exhaustion
 */
bool box_extract_link(const struct html_content *content, const struct dom_string *dsrel, struct nsurl *base, struct nsurl **result);

#endif
