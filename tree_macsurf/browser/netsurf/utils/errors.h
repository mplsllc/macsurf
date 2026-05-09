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

/** \file
 * Error codes
 */

#ifndef NETSURF_UTILS_ERRORS_H_
#define NETSURF_UTILS_ERRORS_H_

/**
 * Enumeration of error codes
 */
typedef enum {
	NSERROR_OK,                     /**< No error */
	NSERROR_UNKNOWN,                /**< Unknown error - DO *NOT* USE */
	NSERROR_NOMEM,                  /**< Memory exhaustion */
	NSERROR_NO_FETCH_HANDLER,       /**< No fetch handler for URL scheme */
	NSERROR_NOT_FOUND,              /**< Requested item not found */
	NSERROR_NOT_DIRECTORY,          /**< Missing directory */
	NSERROR_SAVE_FAILED,            /**< Failed to save data */
	NSERROR_CLONE_FAILED,           /**< Failed to clone handle */
	NSERROR_INIT_FAILED,            /**< Initialisation failed */
	NSERROR_BMP_ERROR,              /**< A BMP error occurred */
	NSERROR_GIF_ERROR,              /**< A GIF error occurred */
	NSERROR_ICO_ERROR,              /**< A ICO error occurred */
	NSERROR_PNG_ERROR,              /**< A PNG error occurred */
	NSERROR_SPRITE_ERROR,           /**< A RISC OS Sprite error occurred */
	NSERROR_SVG_ERROR,              /**< A SVG error occurred */
	NSERROR_BAD_ENCODING,           /**< The character set is unknown */
	NSERROR_NEED_DATA,              /**< More data needed */
	NSERROR_ENCODING_CHANGE,        /**< The character changed */
	NSERROR_BAD_PARAMETER,          /**< Bad Parameter */
	NSERROR_INVALID,                /**< Invalid data */
	NSERROR_BOX_CONVERT,            /**< Box conversion failed */
	NSERROR_STOPPED,                /**< Content conversion stopped */
	NSERROR_DOM,                    /**< DOM call returned error */
	NSERROR_CSS,                    /**< CSS call returned error */
	NSERROR_CSS_BASE,               /**< CSS base sheet failed */
	NSERROR_BAD_URL,                /**< Bad URL */
	NSERROR_BAD_CONTENT,            /**< Bad Content */
	NSERROR_FRAME_DEPTH,            /**< Exceeded frame depth */
	NSERROR_PERMISSION,             /**< Permission error */
	NSERROR_NOSPACE,                /**< Insufficient space */
	NSERROR_BAD_SIZE,               /**< Bad size */
	NSERROR_NOT_IMPLEMENTED,        /**< Functionality is not implemented */
	NSERROR_BAD_REDIRECT,           /**< Fetch encountered a bad redirect */
	NSERROR_CYCLIC_REDIRECT,        /**< Fetch encountered a cyclic redirect */
	NSERROR_UNSAFE_REDIRECT,        /**< Fetch encountered a unsafe redirect */
	NSERROR_BAD_AUTH,               /**< Fetch needs authentication data */
	NSERROR_BAD_CERTS,              /**< Fetch needs certificate chain check */
	NSERROR_TIMEOUT,                /**< Operation timed out */
} nserror;

#endif

