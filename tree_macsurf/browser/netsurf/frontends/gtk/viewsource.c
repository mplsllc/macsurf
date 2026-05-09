/*
 * Copyright 2009 Mark Benjamin <MarkBenjamin@dfgh.net>
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

#include <stdlib.h>
#include <string.h>
#include <gtk/gtk.h>

#include "utils/utils.h"
#include "utils/utf8.h"
#include "utils/nsurl.h"
#include "utils/messages.h"
#include "netsurf/browser_window.h"
#include "netsurf/content.h"

#include "gtk/viewdata.h"
#include "gtk/viewsource.h"

nserror nsgtk_viewsource(GtkWindow *parent, struct browser_window *bw)
{
	nserror ret;
	struct hlcache_handle *hlcontent;
	const uint8_t *source_data;
	size_t source_size;
	char *ndata = NULL;
	size_t ndata_len;
	char *filename;
	char *title;
	size_t titlesize;

	hlcontent = browser_window_get_content(bw);
	if (hlcontent == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	if (content_get_type(hlcontent) != CONTENT_HTML) {
		return NSERROR_BAD_CONTENT;
	}

	source_data = content_get_source_data(hlcontent, &source_size);

	ret = nsurl_nice(browser_window_access_url(bw), &filename, false);
	if (ret != NSERROR_OK) {
		filename = strdup(messages_get("SaveSource"));
		if (filename == NULL) {
			return NSERROR_NOMEM;
		}
	}

	titlesize = strlen(nsurl_access(browser_window_access_url(bw))) + SLEN("Source of  - NetSurf") + 1;
	title = malloc(titlesize);
	if (title == NULL) {
		free(filename);
		return NSERROR_NOMEM;
	}
	snprintf(title, titlesize, "Source of %s - NetSurf",
		 nsurl_access(browser_window_access_url(bw)));

	ret = utf8_from_enc((const char *)source_data,
			    content_get_encoding(hlcontent,
						 CONTENT_ENCODING_NORMAL),
			    source_size,
			    &ndata,
			    &ndata_len);
	if (ret == NSERROR_OK) {
		ret = nsgtk_viewdata(title, filename, ndata, ndata_len);
	}

	free(filename);
	free(title);

	return ret;
}
