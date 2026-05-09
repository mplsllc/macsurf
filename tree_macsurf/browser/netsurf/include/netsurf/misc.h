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
 * \file
 *
 * Interface to platform-specific miscellaneous browser operation table.
 */

#ifndef NETSURF_MISC_H_
#define NETSURF_MISC_H_

struct form_control;
struct gui_window;
struct cert_chain;
struct nsurl;

/**
 * Graphical user interface browser misc function table.
 *
 * function table implementing GUI interface to miscelaneous browser
 * functionality
 */
struct gui_misc_table {
	/* Mandantory entries */

	/**
	 * Schedule a callback.
	 *
	 * \param t interval before the callback should be made in ms or
	 *          negative value to remove any existing callback.
	 * \param callback callback function
	 * \param p user parameter passed to callback function
	 * \return NSERROR_OK on sucess or appropriate error on faliure
	 *
	 * The callback function will be called as soon as possible
	 * after the timeout has elapsed.
	 *
	 * Additional calls with the same callback and user parameter will
	 * reset the callback time to the newly specified value.
	 *
	 */
	nserror (*schedule)(int t, void (*callback)(void *p), void *p);


	/* Optional entries */

	/**
	 * called to allow the gui to cleanup.
	 */
	void (*quit)(void);

	/**
	 * core has no fetcher for url
	 */
	nserror (*launch_url)(struct nsurl *url);

	/**
	 * Retrieve username/password for a given url+realm if there is one
	 * stored in a frontend-specific way (e.g. gnome-keyring)
	 *
	 * To respond, call the callback with the url, realm, username,
	 * and password.  Pass "" if the empty string
	 * is required.
	 *
	 * To keep hold of the url, remember to nsurl_ref() it, and to keep
	 * the realm, you will need to strdup() it.
	 *
	 * If the front end returns NSERROR_OK for this function, they may,
	 * at some future time, call the `cb` with `cbpw` callback exactly once.
	 *
	 * If the front end returns other than NSERROR_OK, they should not
	 * call the `cb` callback.
	 *
	 * The callback should not be called immediately upon receipt of this
	 * call as the browser window may not be reentered.
	 *
	 * **NOTE** The lifetime of the cbpw is not well defined.  In general
	 * do not use the cb if *any* browser window has navigated or been
	 * destroyed.
	 *
	 * \param url       The URL being verified.
	 * \param realm     The authorization realm.
	 * \param username  Any current username (or empty string).
	 * \param password  Any current password (or empty string).
	 * \param cb        Callback upon user decision.
	 * \param cbpw      Context pointer passed to cb
	 * \return NSERROR_OK on sucess else error and cb never called
	 */
	nserror (*login)(struct nsurl *url, const char *realm,
			 const char *username, const char *password,
			 nserror (*cb)(struct nsurl *url,
				       const char *realm,
				       const char *username,
				       const char *password,
				       void *pw),
			 void *cbpw);

	/**
	 * Prompt the user for a password for a PDF.
	 */
	void (*pdf_password)(char **owner_pass, char **user_pass, char *path);

	/**
	 * Request that the cookie manager be displayed
	 *
	 * \param search_term The search term to be set (NULL if no search)
	 *
	 * \return NSERROR_OK on success
	 */
	nserror (*present_cookies)(const char *search_term);
};

#endif
