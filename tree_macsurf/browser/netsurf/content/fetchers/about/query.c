/*
 * Copyright 2020 Vincent Sanders <vince@netsurf-browser.org>
 *
 * This file is part of NetSurf.
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
 * content generator for the about scheme query privacy page
 */

#include <stdlib.h>

#include "utils/errors.h"
#include "utils/nsurl.h"
#include "utils/messages.h"

#include "query.h"

/* exported interface documented in about/query.h */
nserror
get_query_description(struct nsurl *url,
		      const char *key,
		      char **out_str)
{
	nserror res;
	char *url_s;
	size_t url_l;
	char *str = NULL;

	/* get the host in question */
	res = nsurl_get(url, NSURL_HOST, &url_s, &url_l);
	if (res != NSERROR_OK) {
		return res;
	}

	/* obtain the description with the url substituted */
	str = messages_get_buff(key, url_s);
	if (str == NULL) {
		res = NSERROR_NOMEM;
	} else {
		*out_str = str;
	}

	free(url_s);

	return res;
}
