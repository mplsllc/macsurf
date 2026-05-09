/*
 * Copyright 2019 Daniel Silverstone <dsilvers@netsurf-browser.org>
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
 * Browser window console stuff
 */

#ifndef _NETSURF_CONSOLE_H_
#define _NETSURF_CONSOLE_H_

/**
 * Sources of messages which end up in the browser window console
 */
typedef enum {
	BW_CS_INPUT, /**< Input from the client */
	BW_CS_SCRIPT_ERROR, /**< Error from some running script */
	BW_CS_SCRIPT_CONSOLE, /**< Logging from some running script */
} browser_window_console_source;

/**
 * Flags for browser window console logging.
 *
 * It is valid to bitwise-or some of these flags together where indicated.
 */
typedef enum {
	/**
	 * The log entry is foldable.
	 *
	 * Set this to indicate that the text should be folded on the first
	 * newline on display.  If this is set but there are no newlines in
	 * the logged text, the core will unset it before passing on to
	 * callbacks or storing the log entry.
	 */
	BW_CS_FLAG_FOLDABLE = 1 << 0,

	/** Logged at the 'debug' level, please use only one of the LEVEL flags */
	BW_CS_FLAG_LEVEL_DEBUG = 0 << 1,
	/** Logged at the 'log' level, please only use one of the LEVEL flags */
	BW_CS_FLAG_LEVEL_LOG = 1 << 1,
	/** Logged at the 'info' level, please use only one of the LEVEL flags */
	BW_CS_FLAG_LEVEL_INFO = 2 << 1,
	/** Logged at the 'warn' level, please use only one of the LEVEL flags */
	BW_CS_FLAG_LEVEL_WARN = 3 << 1,
	/** Logged at the 'error' level, please use only one of the LEVEL flags */
	BW_CS_FLAG_LEVEL_ERROR = 4 << 1,
	/* Levels 5, 6, 7 unused as yet */
	/** Mask for the error level to allow easy comparison using the above */
	BW_CS_FLAG_LEVEL_MASK = 7 << 1,
} browser_window_console_flags;

#endif /* _NETSURF_CONSOLE_H_ */

