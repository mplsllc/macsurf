/*
 * Copyright 2009 Mark Benjamin <netsurf-browser.org.MarkBenjamin@dfgh.net>
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
 * Interface to HTML searching.
 */

#ifndef NETSURF_CONTENT_SEARCH_H
#define NETSURF_CONTENT_SEARCH_H

#include "desktop/search.h"

struct textsearch_context;
struct content;
struct hlcache_handle;
struct box;

/**
 * Free text search a content
 *
 * \param[in] h Handle to content to search.
 * \param[in] context The context passed to gui table search handlers
 * \param[in] flags The flags that control the search
 * \param[in] The string being searched for.
 * \retun NSERROR_OK on success else error code on faliure
 */
nserror content_textsearch(struct hlcache_handle *h, void *context, search_flags_t flags, const char *string);

/**
 * Clear a search
 *
 * \param[in] h Handle to content to clear search from.
 */
nserror content_textsearch_clear(struct hlcache_handle *h);

/**
 * Ends the search process, invalidating all state freeing the list of
 * found boxes.
 */
nserror content_textsearch_destroy(struct textsearch_context *textsearch);

/**
 * Determines whether any portion of the given text box should be
 * selected because it matches the current search string.
 *
 * \param textsearch The search context to hilight entries from.
 * \param c The content to highlight within.
 * \param start_offset byte offset within text of string to be checked
 * \param end_offset   byte offset within text
 * \param start_idx    byte offset within string of highlight start
 * \param end_idx      byte offset of highlight end
 * \return true iff part of the box should be highlighted
 */
bool content_textsearch_ishighlighted(struct textsearch_context *textsearch,
				      unsigned start_offset,
				      unsigned end_offset,
				      unsigned *start_idx,
				      unsigned *end_idx);

/**
 * Find the first occurrence of 'match' in 'string' and return its index
 *
 * \param  string     the string to be searched (unterminated)
 * \param  s_len      length of the string to be searched
 * \param  pattern    the pattern for which we are searching (unterminated)
 * \param  p_len      length of pattern
 * \param  case_sens  true iff case sensitive match required
 * \param  m_len      accepts length of match in bytes
 * \return pointer to first match, NULL if none
 */
const char *content_textsearch_find_pattern(const char *string, int s_len, const char *pattern, int p_len, bool case_sens, unsigned int *m_len);

/**
 * Add a new entry to the list of matches
 *
 * \param context The search context to add the entry to.
 * \param start_idx Offset of match start within textual representation
 * \param end_idx Offset of match end
 * \param start A pointer for the start
 * \param end A pointer for the end
 * \return NSERROR_OK on sucess else error code on faliure
 */
nserror content_textsearch_add_match(struct textsearch_context *context, unsigned start_idx, unsigned end_idx, struct box *start_ptr, struct box *end_ptr);

#endif
