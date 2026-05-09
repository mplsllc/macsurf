/*
 * Copyright 2014 Vincent Sanders <vince@netsurf-browser.org>
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
 * \file desktop/searchweb.h
 * \brief core web search facilities interface.
 */

#ifndef _NETSURF_DESKTOP_SEARCH_WEB_H_
#define _NETSURF_DESKTOP_SEARCH_WEB_H_

struct bitmap;
struct nsurl;

/**
 * Graphical user interface browser web search function table.
 *
 */
struct gui_search_web_table {
	/**
	 * called when the search provider details are updated.
	 *
	 * \param provider_name The name of the provider.
	 * \param ico_bitmap The bitmap of the search icon may be NULL
	 * if no icon is yet available.
	 */
	nserror (*provider_update)(const char *provider_name, struct bitmap *ico_bitmap);
};

/**
 * Flags which alter the behaviour of the omin search.
 */
enum search_web_omni_flags {
	/** no changes to default operation */
	SEARCH_WEB_OMNI_NONE = 0,

	/** The search does not attempt to interpret the url as a url
	 * before using it as a search term.
	 */
	SEARCH_WEB_OMNI_SEARCHONLY = 1,
};

/**
 * Generate a nsurl from a search term.
 *
 * This interface obtains a url appropriate for the given search
 * term. The flags allow control over the operation. By default the
 * operations are:
 *  - interpret the \a term as a url
 *  - if missing a scheme as a https: url
 *  - combined with the search providers url into a url for that provider.
 *
 * \param term The search term.
 * \param flags Flags to control operation.
 * \param url_out The ourput url on success.
 * \return NSERROR_OK on success or appropriate error code.
 */
nserror search_web_omni(const char *term, enum search_web_omni_flags flags, struct nsurl **url_out);


/**
 * obtain the current providers bitmap
 *
 * obtain the icon representing the current web search provider
 *
 * \param bitmap_out recives the resulting bitmap which may be NULL
 * \return NSERROR_OK on success or NSERROR_INIT_FAILED if not initialised
 */
nserror search_web_get_provider_bitmap(struct bitmap **bitmap_out);

/**
 * Change the currently selected web search provider.
 *
 * \param selection Name of the search provider to select or NULL to
 *                  reselect the current provider
 * \return NSERROR_OK on success or appropriate error code.
 */
nserror search_web_select_provider(const char *selection);


/**
 * Iterate the search providers, returning their names.
 *
 * \param from Index to start iteration from.  Use -1 to begin iteration.
 *             Use the value returned from search_web_iterate_providers to
 *             continue an iteration.
 * \param name Pointer to fill in with the search provider name requested.
 * \return -1 if there are no more, otherwise the iterator for the next item.
 *
 * \verbatim
 *     ssize_t iter = -1;
 *     const char *name;
 *     ...
 *     iter = search_web_iterate_providers(iter, &name);
 *     while(iter !=-1) {
 *         do_something_with(name);
 *         iter = search_web_iterate_providers(iter, &name);
 *     }
 * \endverbatim
 */
ssize_t search_web_iterate_providers(ssize_t iter, const char **name);


/**
 * Initialise the web search operations.
 *
 * \param provider_fname Path to web search providers file.
 * \return NSERROR_OK on successful initialisation or appropriate error code.
 */
nserror search_web_init(const char *provider_fname);

/**
 * Finalise the web search operations freeing all resources.
 *
 * \return NSERROR_OK on success or appropriate error code.
 */
nserror search_web_finalise(void);

#endif
