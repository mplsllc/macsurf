/*
 * Copyright 2010 Vincent Sanders <vince@netsurf-browser.org>
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
 *
 * file scheme URL handling. Based on the data fetcher by Rob Kendrick
 *
 * output dates and directory ordering are affected by the current locale
 */

#include "utils/config.h"

#include <stdlib.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <stdio.h>
#include <stdarg.h>
#ifdef HAVE_MMAP
#include <sys/mman.h>
#endif
#include <libwapcaplet/libwapcaplet.h>

#include "netsurf/inttypes.h"
#include "utils/nsurl.h"
#include "utils/dirent.h"
#include "utils/corestrings.h"
#include "utils/messages.h"
#include "utils/utils.h"
#include "utils/log.h"
#include "utils/time.h"
#include "utils/ring.h"
#include "utils/file.h"
#include "netsurf/fetch.h"
#include "desktop/gui_internal.h"

#include "content/fetch.h"
#include "content/fetchers.h"
#include "dirlist.h"
#include "file.h"

/* Maximum size of read buffer */
#define FETCH_FILE_MAX_BUF_SIZE (1024 * 1024)

/** Context for a fetch */
struct fetch_file_context {
	struct fetch_file_context *r_next, *r_prev;

	struct fetch *fetchh; /**< Handle for this fetch */

	bool aborted; /**< Flag indicating fetch has been aborted */
	bool locked; /**< Flag indicating entry is already entered */

	nsurl *url; /**< The full url the fetch refers to */
	char *path; /**< The actual path to be used with open() */

	time_t file_etag; /**< Request etag for file (previous st.m_time) */
};

static struct fetch_file_context *ring = NULL;

/** issue fetch callbacks with locking */
static inline bool fetch_file_send_callback(const fetch_msg *msg,
		struct fetch_file_context *ctx)
{
	ctx->locked = true;
	fetch_send_callback(msg, ctx->fetchh);
	ctx->locked = false;

	return ctx->aborted;
}

static bool fetch_file_send_header(struct fetch_file_context *ctx,
		const char *fmt, ...)
{
	fetch_msg msg;
	char header[64];
	va_list ap;
	int len;

	va_start(ap, fmt);
	len = vsnprintf(header, sizeof header, fmt, ap);
	va_end(ap);

	if (len >= (int)sizeof(header) || len < 0) {
		return false;
	}

	msg.type = FETCH_HEADER;
	msg.data.header_or_data.buf = (const uint8_t *) header;
	msg.data.header_or_data.len = len;

	return fetch_file_send_callback(&msg, ctx);
}

/** callback to initialise the file fetcher. */
static bool fetch_file_initialise(lwc_string *scheme)
{
	return true;
}

/** callback to initialise the file fetcher. */
static void fetch_file_finalise(lwc_string *scheme)
{
}

static bool fetch_file_can_fetch(const nsurl *url)
{
	return true;
}

/** callback to set up a file fetch context. */
static void *
fetch_file_setup(struct fetch *fetchh,
		 nsurl *url,
		 bool only_2xx,
		 bool downgrade_tls,
		 const char *post_urlenc,
		 const struct fetch_multipart_data *post_multipart,
		 const char **headers)
{
	struct fetch_file_context *ctx;
	int i;
	nserror ret;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL)
		return NULL;

	ret = guit->file->nsurl_to_path(url, &ctx->path);
	if (ret != NSERROR_OK) {
		free(ctx);
		return NULL;
	}

	ctx->url = nsurl_ref(url);

	/* Scan request headers looking for If-None-Match */
	for (i = 0; headers[i] != NULL; i++) {
		if (strncasecmp(headers[i], "If-None-Match:",
				SLEN("If-None-Match:")) != 0) {
			continue;
		}

		/* If-None-Match: "12345678" */
		const char *d; d = headers[i] + SLEN(\"If-None-Match:\");

		/* Scan to first digit, if any */
		while (*d != '\0' && (*d < '0' || '9' < *d))
			d++;

		/* Convert to time_t */
		if (*d != '\0') {
			ret = nsc_snptimet(d, strlen(d), &ctx->file_etag);
			if (ret != NSERROR_OK) {
				NSLOG(fetch, WARNING,
						"Bad If-None-Match value");
			}
		}
	}

	ctx->fetchh = fetchh;

	RING_INSERT(ring, ctx);

	return ctx;
}

/** callback to free a file fetch */
static void fetch_file_free(void *ctx)
{
	struct fetch_file_context *c = ctx;
	nsurl_unref(c->url);
	free(c->path);
	free(ctx);
}

/** callback to start a file fetch */
static bool fetch_file_start(void *ctx)
{
	return true;
}

/** callback to abort a file fetch */
static void fetch_file_abort(void *ctx)
{
	struct fetch_file_context *c = ctx;

	/* To avoid the poll loop having to deal with the fetch context
	 * disappearing from under it, we simply flag the abort here.
	 * The poll loop itself will perform the appropriate cleanup.
	 */
	c->aborted = true;
}

static int fetch_file_errno_to_http_code(int error_no)
{
	switch (error_no) {
	case ENAMETOOLONG:
		return 400;
	case EACCES:
		return 403;
	case ENOENT:
		return 404;
	default:
		break;
	}

	return 500;
}

static void fetch_file_process_error(struct fetch_file_context *ctx, int code)
{
	fetch_msg msg;
	char buffer[1024];
	const char *title;
	char key[8];

	/* content is going to return error code */
	fetch_set_http_code(ctx->fetchh, code);

	/* content type */
	if (fetch_file_send_header(ctx, "Content-Type: text/html; charset=utf-8"))
		goto fetch_file_process_error_aborted;

	snprintf(key, sizeof key, "HTTP%03d", code);
	title = messages_get(key);

	snprintf(buffer, sizeof buffer,
		 "<html><head>"
		 "<title>%s</title>"
		 "<link rel=\"stylesheet\" type=\"text/css\" "
		 "href=\"resource:internal.css\">\n"
		 "</head>"
		 "<body class=\"ns-even-bg ns-even-fg ns-border\" "
		 "id =\"fetcherror\">\n"
		 "<h1 class=\"ns-border ns-odd-fg-bad\">%s</h1>\n"
		 "<p>%s %d %s %s</p>\n"
		 "</body>\n</html>\n",
		 title, title,
		 messages_get("FetchErrorCode"), code,
		 messages_get("FetchFile"), nsurl_access(ctx->url));

	msg.type = FETCH_DATA;
	msg.data.header_or_data.buf = (const uint8_t *) buffer;
	msg.data.header_or_data.len = strlen(buffer);
	if (fetch_file_send_callback(&msg, ctx))
		goto fetch_file_process_error_aborted;

	msg.type = FETCH_FINISHED;
	fetch_file_send_callback(&msg, ctx);

fetch_file_process_error_aborted:
	return;
}


/** Process object as a regular file */
static void fetch_file_process_plain(struct fetch_file_context *ctx,
				     struct stat *fdstat)
{
#ifdef HAVE_MMAP
	fetch_msg msg;
	char *buf = NULL;
	size_t buf_size;

	int fd; /**< The file descriptor of the object */

	/* Check if we can just return not modified */
	if (ctx->file_etag != 0 && ctx->file_etag == fdstat->st_mtime) {
		fetch_set_http_code(ctx->fetchh, 304);
		msg.type = FETCH_NOTMODIFIED;
		fetch_file_send_callback(&msg, ctx);
		return;
	}

	fd = open(ctx->path, O_RDONLY);
	if (fd < 0) {
		/* process errors as appropriate */
		fetch_file_process_error(ctx,
				fetch_file_errno_to_http_code(errno));
		return;
	}

	/* set buffer size */
	buf_size = fdstat->st_size;

	/* allocate the buffer storage */
	if (buf_size > 0) {
		buf = mmap(NULL, buf_size, PROT_READ, MAP_SHARED, fd, 0);
		if (buf == MAP_FAILED) {
			msg.type = FETCH_ERROR;
			msg.data.error = "Unable to map memory for file data buffer";
			fetch_file_send_callback(&msg, ctx);
			close(fd);
			return;
		}
	}

	/* fetch is going to be successful */
	fetch_set_http_code(ctx->fetchh, 200);

	/* Any callback can result in the fetch being aborted.
	 * Therefore, we _must_ check for this after _every_ call to
	 * fetch_file_send_callback().
	 */

	/* content type */
	if (fetch_file_send_header(ctx, "Content-Type: %s", 
				   guit->fetch->filetype(ctx->path))) {
		goto fetch_file_process_aborted;
	}

	/* content length */
	if (fetch_file_send_header(ctx, "Content-Length: %" PRIsizet,
				   fdstat->st_size)) {
		goto fetch_file_process_aborted;
	}

	/* create etag */
	if (fetch_file_send_header(ctx, "ETag: \"%10" PRId64 "\"",
				   (int64_t) fdstat->st_mtime)) {
		goto fetch_file_process_aborted;
	}

	msg.type = FETCH_DATA;
	msg.data.header_or_data.buf = (const uint8_t *) buf;
	msg.data.header_or_data.len = buf_size;
	fetch_file_send_callback(&msg, ctx);

	if (ctx->aborted == false) {
		msg.type = FETCH_FINISHED;
		fetch_file_send_callback(&msg, ctx);
	}

fetch_file_process_aborted:

	if (buf != NULL)
		munmap(buf, buf_size);
	close(fd);
#else
	fetch_msg msg;
	char *buf;
	size_t buf_size;

	ssize_t tot_read = 0;
	ssize_t res;

	FILE *infile;

	/* Check if we can just return not modified */
	if (ctx->file_etag != 0 && ctx->file_etag == fdstat->st_mtime) {
		fetch_set_http_code(ctx->fetchh, 304);
		msg.type = FETCH_NOTMODIFIED;
		fetch_file_send_callback(&msg, ctx);
		return;
	}

	infile = fopen(ctx->path, "rb");
	if (infile == NULL) {
		/* process errors as appropriate */
		fetch_file_process_error(ctx,
				fetch_file_errno_to_http_code(errno));
		return;
	}

	/* set buffer size */
	buf_size = fdstat->st_size;
	if (buf_size > FETCH_FILE_MAX_BUF_SIZE)
		buf_size = FETCH_FILE_MAX_BUF_SIZE;

	/* allocate the buffer storage */
	buf = malloc(buf_size);
	if (buf == NULL) {
		msg.type = FETCH_ERROR;
		msg.data.error =
			"Unable to allocate memory for file data buffer";
		fetch_file_send_callback(&msg, ctx);
		fclose(infile);
		return;
	}

	/* fetch is going to be successful */
	fetch_set_http_code(ctx->fetchh, 200);

	/* Any callback can result in the fetch being aborted.
	 * Therefore, we _must_ check for this after _every_ call to
	 * fetch_file_send_callback().
	 */

	/* content type */
	if (fetch_file_send_header(ctx, "Content-Type: %s", 
				   guit->fetch->filetype(ctx->path))) {
		goto fetch_file_process_aborted;
	}

	/* content length */
	if (fetch_file_send_header(ctx, "Content-Length: %" PRIsizet,
				   fdstat->st_size)) {
		goto fetch_file_process_aborted;
	}

	/* create etag */
	if (fetch_file_send_header(ctx, "ETag: \"%10" PRId64 "\"", 
				   (int64_t) fdstat->st_mtime)) {
		goto fetch_file_process_aborted;
	}

	/* main data loop */
	while (tot_read < fdstat->st_size) {
		res = fread(buf, 1, buf_size, infile);
		if (res == 0) {
			if (feof(infile)) {
				msg.type = FETCH_ERROR;
				msg.data.error = "Unexpected EOF reading file";
				fetch_file_send_callback(&msg, ctx);
				goto fetch_file_process_aborted;
			} else {
				msg.type = FETCH_ERROR;
				msg.data.error = "Error reading file";
				fetch_file_send_callback(&msg, ctx);
				goto fetch_file_process_aborted;
			}
		}
		tot_read += res;

		msg.type = FETCH_DATA;
		msg.data.header_or_data.buf = (const uint8_t *) buf;
		msg.data.header_or_data.len = res;
		if (fetch_file_send_callback(&msg, ctx))
			break;
	}

	if (ctx->aborted == false) {
		msg.type = FETCH_FINISHED;
		fetch_file_send_callback(&msg, ctx);
	}

fetch_file_process_aborted:

	fclose(infile);
	free(buf);
#endif
	return;
}

static char *gen_nice_title(char *path)
{
	char *nice_path, *cnv, *tmp;
	char *title;
	int title_length;

	/* Convert path for display */
	nice_path = malloc(strlen(path) * SLEN("&amp;") + 1);
	if (nice_path == NULL) {
		return NULL;
	}

	/* Escape special HTML characters */
	for (cnv = nice_path, tmp = path; *tmp != '\0'; tmp++) {
		if (*tmp == '<') {
			*cnv++ = '&';
			*cnv++ = 'l';
			*cnv++ = 't';
			*cnv++ = ';';
		} else if (*tmp == '>') {
			*cnv++ = '&';
			*cnv++ = 'g';
			*cnv++ = 't';
			*cnv++ = ';';
		} else if (*tmp == '&') {
			*cnv++ = '&';
			*cnv++ = 'a';
			*cnv++ = 'm';
			*cnv++ = 'p';
			*cnv++ = ';';
		} else {
			*cnv++ = *tmp;
		}
	}
	*cnv = '\0';

	/* Construct a localised title string */
	title_length = (cnv - nice_path) + strlen(messages_get("FileIndex"));
	title = malloc(title_length + 1);

	if (title == NULL) {
		free(nice_path);
		return NULL;
	}

	/* Set title to localised "Index of <nice_path>" */
	snprintf(title, title_length, messages_get("FileIndex"), nice_path);

	free(nice_path);

	return title;
}

/**
 * Generate an output row of the directory listing.
 *
 * \param ctx The file fetching context.
 * \param ent current directory entry.
 * \param even is the row an even row.
 * \param buffer The output buffer.
 * \param buffer_len The space available in the output buffer.
 * \return NSERROR_OK or error code on faliure.
 */
static nserror
process_dir_ent(struct fetch_file_context *ctx,
		 struct dirent *ent,
		 bool even,
		 char *buffer,
		 size_t buffer_len)
{
	nserror ret;
	char *urlpath = NULL; /* buffer for leaf entry path */
	struct stat ent_stat; /* stat result of leaf entry */
	char datebuf[64]; /* buffer for date text */
	char timebuf[64]; /* buffer for time text */
	nsurl *url;

	/* skip hidden files */
	if (ent->d_name[0] == '.') {
		return NSERROR_BAD_PARAMETER;
	}

	ret = netsurf_mkpath(&urlpath, NULL, 2, ctx->path, ent->d_name);
	if (ret != NSERROR_OK) {
		return ret;
	}

	if (stat(urlpath, &ent_stat) != 0) {
		ent_stat.st_mode = 0;
		datebuf[0] = 0;
		timebuf[0] = 0;
	} else {
		/* Get date in output format. a (day of week) and b
		 * (month) are both affected by the locale
		 */
		if (strftime((char *)&datebuf, sizeof datebuf, "%a %d %b %Y",
			     localtime(&ent_stat.st_mtime)) == 0) {
			datebuf[0] = '-';
			datebuf[1] = 0;
		}

		/* Get time in output format */
		if (strftime((char *)&timebuf, sizeof timebuf, "%H:%M",
			     localtime(&ent_stat.st_mtime)) == 0) {
			timebuf[0] = '-';
			timebuf[1] = 0;
		}
	}

	ret = guit->file->path_to_nsurl(urlpath, &url);
	if (ret != NSERROR_OK) {
		free(urlpath);
		return ret;
	}

	if (S_ISREG(ent_stat.st_mode)) {
		/* regular file */
		dirlist_generate_row(even,
				     false,
				     url,
				     ent->d_name,
				     guit->fetch->filetype(urlpath),
				     ent_stat.st_size,
				     datebuf, timebuf,
				     buffer, buffer_len);
	} else if (S_ISDIR(ent_stat.st_mode)) {
		/* directory */
		dirlist_generate_row(even,
				     true,
				     url,
				     ent->d_name,
				     messages_get("FileDirectory"),
				     -1,
				     datebuf, timebuf,
				     buffer, buffer_len);
	} else {
		/* something else */
		dirlist_generate_row(even,
				     false,
				     url,
				     ent->d_name,
				     "",
				     -1,
				     datebuf, timebuf,
				     buffer, buffer_len);
	}

	nsurl_unref(url);
	free(urlpath);

	return NSERROR_OK;
}

/**
 * Comparison function for sorting directories.
 *
 * Correctly orders non zero-padded numerical parts.
 * ie. produces "file1, file2, file10" rather than "file1, file10, file2".
 *
 * \param d1 first directory entry
 * \param d2 second directory entry
 */
static int dir_sort_alpha(const struct dirent **d1, const struct dirent **d2)
{
	const char *s1 = (*d1)->d_name;
	const char *s2 = (*d2)->d_name;

	while (*s1 != '\0' && *s2 != '\0') {
		if ((*s1 >= '0' && *s1 <= '9') &&
				(*s2 >= '0' && *s2 <= '9')) {
			int n1 = 0,  n2 = 0;
			while (*s1 >= '0' && *s1 <= '9') {
				n1 = n1 * 10 + (*s1) - '0';
				s1++;
			}
			while (*s2 >= '0' && *s2 <= '9') {
				n2 = n2 * 10 + (*s2) - '0';
				s2++;
			}
			if (n1 != n2) {
				return n1 - n2;
			}
			if (*s1 == '\0' || *s2 == '\0')
				break;
		}
		if (tolower(*s1) != tolower(*s2))
			break;

		s1++;
		s2++;
	}

	return tolower(*s1) - tolower(*s2);
}

static void fetch_file_process_dir(struct fetch_file_context *ctx,
				   struct stat *fdstat)
{
	fetch_msg msg;
	char buffer[1024]; /* Output buffer */
	bool even = false; /* formatting flag */
	char *title; /* pretty printed title */
	nserror err; /* result from url routines */
	nsurl *up; /* url of parent */

	struct dirent **listing = NULL; /* directory entry listing */
	int i; /* directory entry index */
	int n; /* number of directory entries */

	n = scandir(ctx->path, &listing, 0, dir_sort_alpha);
	if (n < 0) {
		fetch_file_process_error(ctx,
			fetch_file_errno_to_http_code(errno));
		return;
	}

	/* fetch is going to be successful */
	fetch_set_http_code(ctx->fetchh, 200);

	/* force no-cache */
	if (fetch_file_send_header(ctx, "Cache-Control: no-cache"))
		goto fetch_file_process_dir_aborted;

	/* content type */
	if (fetch_file_send_header(ctx, "Content-Type: text/html; charset=utf-8"))
		goto fetch_file_process_dir_aborted;

	msg.type = FETCH_DATA;
	msg.data.header_or_data.buf = (const uint8_t *) buffer;

	/* directory listing top */
	dirlist_generate_top(buffer, sizeof buffer);
	msg.data.header_or_data.len = strlen(buffer);
	if (fetch_file_send_callback(&msg, ctx))
		goto fetch_file_process_dir_aborted;

	/* directory listing title */
	title = gen_nice_title(ctx->path);
	dirlist_generate_title(title, buffer, sizeof buffer);
	free(title);
	msg.data.header_or_data.len = strlen(buffer);
	if (fetch_file_send_callback(&msg, ctx))
		goto fetch_file_process_dir_aborted;

	/* Print parent directory link */
	err = nsurl_parent(ctx->url, &up);
	if (err == NSERROR_OK) {
		if (nsurl_compare(ctx->url, up, NSURL_COMPLETE) == false) {
			/* different URL; have parent */
			dirlist_generate_parent_link(nsurl_access(up),
					buffer, sizeof buffer);

			msg.data.header_or_data.len = strlen(buffer);
			fetch_file_send_callback(&msg, ctx);
		}
		nsurl_unref(up);

		if (ctx->aborted)
			goto fetch_file_process_dir_aborted;

	}

	/* directory list headings */
	dirlist_generate_headings(buffer, sizeof buffer);
	msg.data.header_or_data.len = strlen(buffer);
	if (fetch_file_send_callback(&msg, ctx))
		goto fetch_file_process_dir_aborted;

	for (i = 0; i < n; i++) {

		err = process_dir_ent(ctx, listing[i], even, buffer,
				       sizeof(buffer));

		if (err == NSERROR_OK) {
			msg.data.header_or_data.len = strlen(buffer);
			if (fetch_file_send_callback(&msg, ctx))
				goto fetch_file_process_dir_aborted;

			even = !even;
		}
	}

	/* directory listing bottom */
	dirlist_generate_bottom(buffer, sizeof buffer);
	msg.data.header_or_data.len = strlen(buffer);
	if (fetch_file_send_callback(&msg, ctx))
		goto fetch_file_process_dir_aborted;

	msg.type = FETCH_FINISHED;
	fetch_file_send_callback(&msg, ctx);

fetch_file_process_dir_aborted:

	if (listing != NULL) {
		for (i = 0; i < n; i++) {
			free(listing[i]);
		}
		free(listing);
	}
}


/* process a file fetch */
static void fetch_file_process(struct fetch_file_context *ctx)
{
	struct stat fdstat; /**< The objects stat */

	if (stat(ctx->path, &fdstat) != 0) {
		/* process errors as appropriate */
		fetch_file_process_error(ctx,
				fetch_file_errno_to_http_code(errno));
		return;
	}

	if (S_ISDIR(fdstat.st_mode)) {
		/* directory listing */
		fetch_file_process_dir(ctx, &fdstat);
		return;
	} else if (S_ISREG(fdstat.st_mode)) {
		/* regular file */
		fetch_file_process_plain(ctx, &fdstat);
		return;
	} else {
		/* unhandled type of file */
		fetch_file_process_error(ctx, 501);
	}

	return;
}

/** callback to poll for additional file fetch contents */
static void fetch_file_poll(lwc_string *scheme)
{
	struct fetch_file_context *c, *save_ring = NULL;

	while (ring != NULL) {
		/* Take the first entry from the ring */
		c = ring;
		RING_REMOVE(ring, c);

		/* Ignore fetches that have been flagged as locked.
		 * This allows safe re-entrant calls to this function.
		 * Re-entrancy can occur if, as a result of a callback,
		 * the interested party causes fetch_poll() to be called
		 * again.
		 */
		if (c->locked == true) {
			RING_INSERT(save_ring, c);
			continue;
		}

		/* Only process non-aborted fetches */
		if (c->aborted == false) {
			/* file fetches can be processed in one go */
			fetch_file_process(c);
		}

		/* And now finish */
		fetch_remove_from_queues(c->fetchh);
		fetch_free(c->fetchh);

	}

	/* Finally, if we saved any fetches which were locked, put them back
	 * into the ring for next time
	 */
	ring = save_ring;
}

nserror fetch_file_register(void)
{
	lwc_string *scheme = lwc_string_ref(corestring_lwc_file);
	const struct fetcher_operation_table fetcher_ops = {
		.initialise = fetch_file_initialise,
		.acceptable = fetch_file_can_fetch,
		.setup = fetch_file_setup,
		.start = fetch_file_start,
		.abort = fetch_file_abort,
		.free = fetch_file_free,
		.poll = fetch_file_poll,
		.finalise = fetch_file_finalise
	};

	return fetcher_add(scheme, &fetcher_ops);
}
