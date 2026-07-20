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
 * HTML content object interface
 */

#ifndef NETSURF_HTML_OBJECT_H
#define NETSURF_HTML_OBJECT_H

struct html_content;
struct browser_window;
struct box;
struct nsurl;

/**
 * Start a fetch for an object required by a page.
 *
 * The created content object is added to the HTML content which is
 *  updated as the fetch progresses. The box (if any) is updated when
 *  the object content becomes done.
 *
 * \param c content of type CONTENT_HTML
 * \param url URL of object to fetch
 * \param box box that will contain the object or NULL if none
 * \param permitted_types bitmap of acceptable types
 * \param background this is a background image
 * \return true on success, false on memory exhaustion
 */
bool html_fetch_object(struct html_content *c, struct nsurl *url, struct box *box, content_type permitted_types, bool background);

/**
 * release memory of content objects associated with a HTML content
 *
 * The content objects contents should have been previously closed
 *  with html_object_close_objects().
 *
 * \param html The html content to release the objects from.
 * \return NSERROR_OK on success else appropriate error code.
 */
nserror html_object_free_objects(struct html_content *html);

/**
 * close content of content objects associated with a HTML content
 *
 * \param html The html content to close the objects from.
 * \return NSERROR_OK on success else appropriate error code.
 */
nserror html_object_close_objects(struct html_content *html);


/**
 * open content of content objects associated with a HTML content
 *
 * \param html The html content to open the objects from.
 * \param bw Browser window handle to open contents with.
 * \return NSERROR_OK on success else appropriate error code.
 */
nserror html_object_open_objects(struct html_content *html, struct browser_window *bw);


/**
 * abort any content objects that have not completed fetching.
 *
 * \param html The html content to abort the objects from.
 * \return NSERROR_OK on success else appropriate error code.
 */
nserror html_object_abort_objects(struct html_content *html);

#endif
