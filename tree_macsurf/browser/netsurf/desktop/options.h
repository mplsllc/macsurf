/*
 * Copyright 2012 Vincent Sanders <vince@netsurf-browser.org>
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
 * Option available on all platforms
 *
 * Non-platform specific options can be added by editing this file 
 *
 * Platform specific options should be added in the platform options.h.
 *
 * This header is specificaly intented to be included multiple times
 *   with different macro definitions so there is no guard
 */

#ifndef NETSURF_DESKTOP_OPTIONS_H_
#define NETSURF_DESKTOP_OPTIONS_H_

#include "netsurf/types.h"

/* defines for system colour table */
#define NSOPTION_SYS_COLOUR_START NSOPTION_sys_colour_AccentColor
#define NSOPTION_SYS_COLOUR_END NSOPTION_sys_colour_VisitedText

#endif

/** An HTTP proxy should be used. */
NSOPTION_BOOL(http_proxy, false) 

/** Hostname of proxy. */
NSOPTION_STRING(http_proxy_host, NULL) 

/** Proxy port. */
NSOPTION_INTEGER(http_proxy_port, 8080) 

/** Proxy authentication method. */
NSOPTION_INTEGER(http_proxy_auth, OPTION_HTTP_PROXY_AUTH_NONE) 

/** Proxy authentication user name */
NSOPTION_STRING(http_proxy_auth_user, NULL)

/** Proxy authentication password */
NSOPTION_STRING(http_proxy_auth_pass, NULL)

/** Proxy omission list */
NSOPTION_STRING(http_proxy_noproxy, "localhost")

/** Default treeview font size (unit: 0.1pt) */
NSOPTION_INTEGER(treeview_font_size, 110)

/** Default font size (unit: 0.1pt) */
NSOPTION_INTEGER(font_size, 128)

/** Minimum font size. */
NSOPTION_INTEGER(font_min_size, 85)

/** Default sans serif font */
NSOPTION_STRING(font_sans, NULL)
/** Default serif font */
NSOPTION_STRING(font_serif, NULL)

/** Default monospace font */
NSOPTION_STRING(font_mono, NULL)

/** Default cursive font */
NSOPTION_STRING(font_cursive, NULL)

/** Default fantasy font */
NSOPTION_STRING(font_fantasy, NULL)

/** Accept-Language header. */
NSOPTION_STRING(accept_language, NULL)

/** Accept-Charset header. */
NSOPTION_STRING(accept_charset, NULL)

/** Preferred maximum size of memory cache / bytes. */
NSOPTION_INTEGER(memory_cache_size, 12 * 1024 * 1024)

/** Preferred location of disc cache, or NULL for system provided location */
NSOPTION_STRING(disc_cache_path, NULL)

/** Preferred expiry size of disc cache / bytes. */
NSOPTION_UINT(disc_cache_size, 1024 * 1024 * 1024)

/** Preferred expiry age of disc cache / days. */
NSOPTION_INTEGER(disc_cache_age, 28)

/** Whether to block advertisements */
NSOPTION_BOOL(block_advertisements, false)

/** Control popups e.g. noniteractive opening windows with window.open */
NSOPTION_BOOL(disable_popups, false)

/** Disable website tracking, see	                
 * http://www.w3.org/Submission/2011/SUBM-web-tracking-protection-20110224/#dnt-uas */
NSOPTION_BOOL(do_not_track, false)

/** Whether to send the referer HTTP header */
NSOPTION_BOOL(send_referer, true)

/** Whether to fetch foreground images */
NSOPTION_BOOL(foreground_images, true)

/** Whether to fetch background images */
NSOPTION_BOOL(background_images, true)

/** Whether to animate images */
NSOPTION_BOOL(animate_images, true)

/** Whether to execute javascript */
NSOPTION_BOOL(enable_javascript, false)

/** Whether to allow Author level CSS. */
NSOPTION_BOOL(author_level_css, true)

/** Maximum time (in seconds) to wait for a script to run */
NSOPTION_INTEGER(script_timeout, 10)

/** How many days to retain URL data for */
NSOPTION_INTEGER(expire_url, 28)

/** Default font family */
NSOPTION_INTEGER(font_default, PLOT_FONT_FAMILY_SANS_SERIF)

/** ca-bundle location */
NSOPTION_STRING(ca_bundle, NULL)

/** ca-path location */
NSOPTION_STRING(ca_path, NULL)

/** Cookie file location */
NSOPTION_STRING(cookie_file, NULL)

/** Cookie jar location */
NSOPTION_STRING(cookie_jar, NULL)

/** Home page location */
NSOPTION_STRING(homepage_url, NULL)

/** search web from url bar */
NSOPTION_BOOL(search_url_bar, false)

/** default web search provider */
NSOPTION_STRING(search_web_provider, NULL)

/** URL completion in url bar */
NSOPTION_BOOL(url_suggestion, true)

/** default x position of new windows */
NSOPTION_INTEGER(window_x, 0)

/** default y position of new windows */
NSOPTION_INTEGER(window_y, 0)

/** default width of new windows */
NSOPTION_INTEGER(window_width, 0)

/** default height of new windows */
NSOPTION_INTEGER(window_height, 0)

/** default size of status bar vs. h scroll bar */
NSOPTION_INTEGER(toolbar_status_size, 6667)

/** default window scale */
NSOPTION_INTEGER(scale, 100)

/* Whether to reflow web pages while objects are fetching */
NSOPTION_BOOL(incremental_reflow, true)

/* Minimum time (in cs) between HTML reflows while objects are fetching */
NSOPTION_UINT(min_reflow_period, DEFAULT_REFLOW_PERIOD)

/* use core selection menu */
NSOPTION_BOOL(core_select_menu, false)

/* display decoded international domain names */
NSOPTION_BOOL(display_decoded_idn, false)

/******** Fetcher options ********/

/** Maximum simultaneous active fetchers */
NSOPTION_INTEGER(max_fetchers, 24)

/** Maximum simultaneous active fetchers per host.
 * (<=option_max_fetchers else it makes no sense) Note that rfc2616
 * section 8.1.4 says that there should be no more than two keepalive
 * connections per host. None of the main browsers follow this as it
 * slows page fetches down considerably.  See
 * https://bugzilla.mozilla.org/show_bug.cgi?id=423377#c4
 */
NSOPTION_INTEGER(max_fetchers_per_host, 5)

/** Maximum number of inactive fetchers cached.  The total number of
 * handles netsurf will therefore have open is this plus
 * option_max_fetchers.
 */
NSOPTION_INTEGER(max_cached_fetch_handles, 6)

/** Number of times to retry timed-out fetches before giving up. */
NSOPTION_UINT(max_retried_fetches, 1)

/** Number of seconds to allow for a DNS-resolution+connect() before timing out
 * the cURL socket.
 */
NSOPTION_UINT(curl_fetch_timeout, 30)

/** Maximum number of redirects before a fetch fails. */
NSOPTION_UINT(fetch_redirect_limit, 10)

/** Suppress debug output from cURL. */
NSOPTION_BOOL(suppress_curl_debug, true)

/******** appearnce of new browser views ********/

/** Whether to allow target="_blank" */
NSOPTION_BOOL(target_blank, true)

/** auxillary (second) mouse button opens in new tab */
NSOPTION_BOOL(button_2_tab, true)

/** control if new view (window or tab) opens in foreground */
NSOPTION_BOOL(foreground_new, false)

/******** PDF / Print options ********/

/** top margin of exported page */
NSOPTION_INTEGER(margin_top, DEFAULT_MARGIN_TOP_MM)

/** bottom margin of exported page */
NSOPTION_INTEGER(margin_bottom, DEFAULT_MARGIN_BOTTOM_MM)

/** left margin of exported page */
NSOPTION_INTEGER(margin_left, DEFAULT_MARGIN_LEFT_MM)

/** right margin of exported page */
NSOPTION_INTEGER(margin_right, DEFAULT_MARGIN_RIGHT_MM)

/** scale of exported content */
NSOPTION_INTEGER(export_scale, DEFAULT_EXPORT_SCALE * 100)

/** suppressing images in printed content */
NSOPTION_BOOL(suppress_images, false)

/** turning off all backgrounds for printed content */
NSOPTION_BOOL(remove_backgrounds, false)

/** turning on content loosening for printed content */
NSOPTION_BOOL(enable_loosening, true)

/** compression of PDF documents */
NSOPTION_BOOL(enable_PDF_compression, true)

/** setting a password and encoding PDF documents */
NSOPTION_BOOL(enable_PDF_password, false)

/** whether to prefer dark mode (light on dark) */
NSOPTION_BOOL(prefer_dark_mode, false)

/******** System colours ********/
NSOPTION_COLOUR(sys_colour_AccentColor, 0x00666666)
NSOPTION_COLOUR(sys_colour_AccentColorText, 0x00ffffff)
NSOPTION_COLOUR(sys_colour_ActiveText, 0x000000ee)
NSOPTION_COLOUR(sys_colour_ButtonBorder, 0x004e4e4e)
NSOPTION_COLOUR(sys_colour_ButtonFace, 0x00f9f9f9)
NSOPTION_COLOUR(sys_colour_ButtonText, 0x004c4c4c)
NSOPTION_COLOUR(sys_colour_Canvas, 0x00f1f1f1)
NSOPTION_COLOUR(sys_colour_CanvasText, 0x00000000)
NSOPTION_COLOUR(sys_colour_Field, 0x00f1f1f1)
NSOPTION_COLOUR(sys_colour_FieldText, 0x00000000)
NSOPTION_COLOUR(sys_colour_GrayText, 0x00a6a6a6)
NSOPTION_COLOUR(sys_colour_Highlight, 0x00c00800)
NSOPTION_COLOUR(sys_colour_HighlightText, 0x00ffffff)
NSOPTION_COLOUR(sys_colour_LinkText, 0x00ee0000)
NSOPTION_COLOUR(sys_colour_Mark, 0x0000ffff)
NSOPTION_COLOUR(sys_colour_MarkText, 0x00000000)
NSOPTION_COLOUR(sys_colour_SelectedItem, 0x00e48435)
NSOPTION_COLOUR(sys_colour_SelectedItemText, 0x00ffffff)
NSOPTION_COLOUR(sys_colour_VisitedText, 0x008b1a55)

/** Filter for non-verbose logging */
NSOPTION_STRING(log_filter, NETSURF_BUILTIN_LOG_FILTER)
/** Filter for verbose logging */
NSOPTION_STRING(verbose_filter, NETSURF_BUILTIN_VERBOSE_FILTER)
