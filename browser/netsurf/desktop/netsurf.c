/*
 * Copyright 2003 Phil Mellor <monkeyson@users.sourceforge.net>
 * Copyright 2007 James Bursa <bursa@users.sourceforge.net>
 * Copyright 2004 Andrew Timmins <atimmins@blueyonder.co.uk>
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

#include <locale.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <libwapcaplet/libwapcaplet.h>
#include <dom/dom.h>

#include "netsurf/inttypes.h"
#include "utils/config.h"
#include "utils/errors.h"
#include "utils/nscolour.h"
#include "utils/nsoption.h"
#include "utils/corestrings.h"
#include "utils/log.h"
#include "utils/string.h"
#include "utils/utf8.h"
#include "utils/messages.h"
#include "utils/useragent.h"
#include "content/content_factory.h"
#include "content/fetchers.h"
#include "content/hlcache.h"
#include "content/mimesniff.h"
#include "content/urldb.h"
#include "css/css.h"
#include "image/image.h"
#include "image/image_cache.h"
#include "javascript/js.h"
#include "html/html.h"
#include "text/textplain.h"

#include "netsurf/browser_window.h"
#include "macsurf_debug.h"
#include "desktop/system_colour.h"
#include "desktop/page-info.h"
#include "desktop/searchweb.h"
#include "netsurf/misc.h"
#include "desktop/gui_internal.h"
#include "netsurf/netsurf.h"


/** \todo QUERY - Remove this import later */
#include "desktop/browser_private.h"

/** speculative pre-conversion small image size
 *
 * Experimenting by visiting every page from default page in order and
 * then netsurf homepage
 *
 * 0    : Cache hit/miss/speculative miss/fail 604/147/  0/0 (80%/19%/ 0%/ 0%)
 * 2048 : Cache hit/miss/speculative miss/fail 622/119/ 17/0 (82%/15%/ 2%/ 0%)
 * 4096 : Cache hit/miss/speculative miss/fail 656/109/ 25/0 (83%/13%/ 3%/ 0%)
 * 8192 : Cache hit/miss/speculative miss/fail 648/104/ 40/0 (81%/13%/ 5%/ 0%)
 * ALL  : Cache hit/miss/speculative miss/fail 775/  0/161/0 (82%/ 0%/17%/ 0%)
*/
#define SPECULATE_SMALL 4096

/** the time between image cache clean runs in ms. */
#define IMAGE_CACHE_CLEAN_TIME (10 * 1000)

/** default time between content cache cleans. */
#define HL_CACHE_CLEAN_TIME (2 * IMAGE_CACHE_CLEAN_TIME)

/** ensure there is a minimal amount of memory for source objetcs and
 * decoded bitmaps.
 */
#define MINIMUM_MEMORY_CACHE_SIZE (2 * 1024 * 1024)

/** default minimum object time before object is pushed to backing store. (s) */
#define LLCACHE_STORE_MIN_LIFETIME (60 * 30)

/** default minimum bandwidth for backing store writeout. (byte/s) */
#define LLCACHE_STORE_MIN_BANDWIDTH (128 * 1024)

/** default maximum bandwidth for backing store writeout. (byte/s) */
#define LLCACHE_STORE_MAX_BANDWIDTH (1024 * 1024)

/** default time quantum with which to calculate bandwidth (ms) */
#define LLCACHE_STORE_TIME_QUANTUM (100)

static void netsurf_lwc_iterator(lwc_string *str, void *pw)
{
	nslog_log(__FILE__, "", __LINE__,
		      "[%3"PRIu32"] %.*s",
		      str->refcnt,
		      (int)lwc_string_length(str),
		      lwc_string_data(str));
}

/* exported interface documented in netsurf/netsurf.h */
nserror netsurf_init(const char *store_path)
{
	nserror ret;
	struct hlcache_parameters hlcache_parameters;
	struct image_cache_parameters image_cache_parameters;

	memset(&hlcache_parameters, 0, sizeof(hlcache_parameters));
	hlcache_parameters.bg_clean_time = HL_CACHE_CLEAN_TIME;
	hlcache_parameters.llcache.minimum_lifetime = LLCACHE_STORE_MIN_LIFETIME;
	hlcache_parameters.llcache.minimum_bandwidth = LLCACHE_STORE_MIN_BANDWIDTH;
	hlcache_parameters.llcache.maximum_bandwidth = LLCACHE_STORE_MAX_BANDWIDTH;
	hlcache_parameters.llcache.time_quantum = LLCACHE_STORE_TIME_QUANTUM;

	memset(&image_cache_parameters, 0, sizeof(image_cache_parameters));
	image_cache_parameters.bg_clean_time = IMAGE_CACHE_CLEAN_TIME;
	image_cache_parameters.speculative_small = SPECULATE_SMALL;
	
#ifdef HAVE_SIGPIPE
	/* Ignore SIGPIPE - this is necessary as OpenSSL can generate these
	 * and the default action is to terminate the app. There's no easy
	 * way of determining the cause of the SIGPIPE (other than using
	 * sigaction() and some mechanism for getting the file descriptor
	 * out of libcurl). However, we expect nothing else to generate a
	 * SIGPIPE, anyway, so may as well just ignore them all.
	 */
	signal(SIGPIPE, SIG_IGN);
#endif

	/* corestrings init */
	ret = corestrings_init();
	if (ret != NSERROR_OK)
		return ret;

	ret = nscolour_update();
	if (ret != NSERROR_OK)
		return ret;

	/* set up cache limits based on the memory cache size option */
	hlcache_parameters.llcache.limit = nsoption_int(memory_cache_size);

	if (hlcache_parameters.llcache.limit < MINIMUM_MEMORY_CACHE_SIZE) {
		hlcache_parameters.llcache.limit = MINIMUM_MEMORY_CACHE_SIZE;
		nslog_log(__FILE__, "", __LINE__,
			      "Setting minimum memory cache size %"PRIsizet,
			      hlcache_parameters.llcache.limit);
	} 

	/* Set up the max attempts made to fetch a timing out resource */
	hlcache_parameters.llcache.fetch_attempts = nsoption_uint(max_retried_fetches);

	/* image cache is 25% of total memory cache size */
	image_cache_parameters.limit = hlcache_parameters.llcache.limit / 4;

	/* image cache hysteresis is 20% of the image cache size */
	image_cache_parameters.hysteresis = image_cache_parameters.limit / 5;

	/* account for image cache use from total */
	hlcache_parameters.llcache.limit -= image_cache_parameters.limit;

	/* set backing store target limit */
	hlcache_parameters.llcache.store.limit = nsoption_uint(disc_cache_size);

	/* set backing store hysterissi to 20% */
	hlcache_parameters.llcache.store.hysteresis = hlcache_parameters.llcache.store.limit / 5;

	/* set the path to the backing store */
	hlcache_parameters.llcache.store.path =
		nsoption_charp(disc_cache_path) ?
		nsoption_charp(disc_cache_path) :
		store_path;

	/* image handler bitmap cache */
	ret = image_cache_init(&image_cache_parameters);
	if (ret != NSERROR_OK)
		return ret;

	/* content handler initialisation */
	{
		nserror css_init_err = nscss_init();
		if (css_init_err != NSERROR_OK) {
			MS_LOG("FATAL: nscss_init failed");
		} else {
			MS_LOG("nscss_init OK");
		}
		ret = css_init_err;
	}
	if (ret != NSERROR_OK)
		return ret;

	ret = html_init();
	if (ret != NSERROR_OK)
		return ret;

	ret = image_init();
	if (ret != NSERROR_OK)
		return ret;

	ret = textplain_init();
	if (ret != NSERROR_OK)
		return ret;

	setlocale(LC_ALL, "");

	/* initialise the fetchers */
	ret = fetcher_init();
	if (ret != NSERROR_OK)
		return ret;
	
	/* Initialise the hlcache and allow it to init the llcache for us */
	ret = hlcache_initialise(&hlcache_parameters);
	if (ret != NSERROR_OK)
		return ret;

	/* Initialize system colours */
	ret = ns_system_colour_init();
	if (ret != NSERROR_OK)
		return ret;

	js_initialise();

	ret = page_info_init();
	if (ret != NSERROR_OK) {
		return ret;
	}

	return NSERROR_OK;
}


/**
 * Clean up components used by gui NetSurf.
 */

void netsurf_exit(void)
{
	hlcache_stop();
	
	NSLOG(netsurf, INFO, "Closing GUI");
	guit->misc->quit();

	NSLOG(netsurf, INFO, "Finalising page-info module");
	page_info_fini();

	NSLOG(netsurf, INFO, "Finalising JavaScript");
	js_finalise();

	NSLOG(netsurf, INFO, "Finalising Web Search");
	search_web_finalise();

	NSLOG(netsurf, INFO, "Finalising high-level cache");
	hlcache_finalise();

	NSLOG(netsurf, INFO, "Closing fetches");
	fetcher_quit();
	/* Now the fetchers are done, our user-agent string can go */
	free_user_agent_string();

	/* dump any remaining cache entries */
	image_cache_fini();

	/* Clean up after content handlers */
	content_factory_fini();

	NSLOG(netsurf, INFO, "Closing utf8");
	utf8_finalise();

	NSLOG(netsurf, INFO, "Destroying URLdb");
	urldb_destroy();

	NSLOG(netsurf, INFO, "Destroying System colours");
	ns_system_colour_finalize();

	NSLOG(netsurf, INFO, "Destroying Messages");
	messages_destroy();

	corestrings_fini();
	if (dom_namespace_finalise() != DOM_NO_ERR) {
		NSLOG(netsurf, WARNING, "Unable to finalise DOM namespace strings");
	}
	NSLOG(netsurf, INFO, "Remaining lwc strings:");
	lwc_iterate_strings(netsurf_lwc_iterator, NULL);

	NSLOG(netsurf, INFO, "Exited successfully");
}
