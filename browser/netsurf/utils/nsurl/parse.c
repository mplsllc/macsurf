/*
 * Copyright 2011 Michael Drake <tlsa@netsurf-browser.org>
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
 * NetSurf URL handling implementation.
 *
 * This is the common implementation of all URL handling within the
 * browser. This implementation is based upon RFC3986 although this has
 * been superceeded by https://url.spec.whatwg.org/ which is based on
 * actual contemporary implementations.
 *
 * Care must be taken with character encodings within this module as
 * the specifications work with specific ascii ranges and must not be
 * affected by locale. Hence the c library character type functions
 * are not used.
 */

#include <assert.h>
#include <ctype.h>
#include <libwapcaplet/libwapcaplet.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "netsurf/inttypes.h"

#include "utils/ascii.h"
#include "utils/corestrings.h"
#include "utils/ns_errors.h"
#include "utils/idna.h"
#include "utils/log.h"
#include "utils/nsurl.h"
#include "utils/nsurl/private.h"
#include "utils/utils.h"


/** Marker set, indicating positions of sections within a URL string */
struct url_markers {
	size_t start; /** start of URL */
	size_t scheme_end;
	size_t authority;

	size_t colon_first;
	size_t at;
	size_t colon_last;

	size_t path;
	size_t query;
	size_t fragment;

	size_t end; /** end of URL */

	enum nsurl_scheme_type scheme_type;
};


/** Sections of a URL */
enum url_sections {
	URL_SCHEME,
	URL_CREDENTIALS,
	URL_HOST,
	URL_PATH,
	URL_QUERY,
	URL_FRAGMENT
};


/**
 * Return a hex digit for the given numerical value.
 *
 * \param digit the value to get the hex digit for.
 * \return character in range 0-9A-F
 */
inline static char digit2uppercase_hex(unsigned char digit) {
	assert(digit < 16);
	return "0123456789ABCDEF"[digit];
}

/**
 * determine if a character is unreserved
 *
 * \param c character to classify.
 * \return true if the character is unreserved else false.
 */
static bool nsurl__is_unreserved(unsigned char c)
{
	/* From RFC3986 section 2.3 (unreserved characters)
	 *
	 *      unreserved  = ALPHA / DIGIT / "-" / "." / "_" / "~"
	 *
	 */
	static const bool unreserved[256] = {
		false, false, false, false, false, false, false, false, /* 00 */
		false, false, false, false, false, false, false, false, /* 08 */
		false, false, false, false, false, false, false, false, /* 10 */
		false, false, false, false, false, false, false, false, /* 18 */
		false, false, false, false, false, false, false, false, /* 20 */
		false, false, false, false, false, true,  true,  false, /* 28 */
		true,  true,  true,  true,  true,  true,  true,  true,  /* 30 */
		true,  true,  false, false, false, false, false, false, /* 38 */
		false, true,  true,  true,  true,  true,  true,  true,  /* 40 */
		true,  true,  true,  true,  true,  true,  true,  true,  /* 48 */
		true,  true,  true,  true,  true,  true,  true,  true,  /* 50 */
		true,  true,  true,  false, false, false, false, true,  /* 58 */
		false, true,  true,  true,  true,  true,  true,  true,  /* 60 */
		true,  true,  true,  true,  true,  true,  true,  true,  /* 68 */
		true,  true,  true,  true,  true,  true,  true,  true,  /* 70 */
		true,  true,  true,  false, false, false, true,  false, /* 78 */
		false, false, false, false, false, false, false, false, /* 80 */
		false, false, false, false, false, false, false, false, /* 88 */
		false, false, false, false, false, false, false, false, /* 90 */
		false, false, false, false, false, false, false, false, /* 98 */
		false, false, false, false, false, false, false, false, /* A0 */
		false, false, false, false, false, false, false, false, /* A8 */
		false, false, false, false, false, false, false, false, /* B0 */
		false, false, false, false, false, false, false, false, /* B8 */
		false, false, false, false, false, false, false, false, /* C0 */
		false, false, false, false, false, false, false, false, /* C8 */
		false, false, false, false, false, false, false, false, /* D0 */
		false, false, false, false, false, false, false, false, /* D8 */
		false, false, false, false, false, false, false, false, /* E0 */
		false, false, false, false, false, false, false, false, /* E8 */
		false, false, false, false, false, false, false, false, /* F0 */
		false, false, false, false, false, false, false, false  /* F8 */
	};
	return unreserved[c];
}

/**
 * determine if a character should be percent escaped.
 *
 * The ASCII codes which should not be percent escaped
 *
 * \param c character to classify.
 * \return true if the character should not be escaped else false.
 */
static bool nsurl__is_no_escape(unsigned char c)
{
	static const bool no_escape[256] = {
		false, false, false, false, false, false, false, false, /* 00 */
		false, false, false, false, false, false, false, false, /* 08 */
		false, false, false, false, false, false, false, false, /* 10 */
		false, false, false, false, false, false, false, false, /* 18 */
		false, true,  false, true,  true,  false, true,  true,  /* 20 */
		true,  true,  true,  true,  true,  true,  true,  true,  /* 28 */
		true,  true,  true,  true,  true,  true,  true,  true,  /* 30 */
		true,  true,  true,  true,  false, true,  false, true,  /* 38 */
		true,  true,  true,  true,  true,  true,  true,  true,  /* 40 */
		true,  true,  true,  true,  true,  true,  true,  true,  /* 48 */
		true,  true,  true,  true,  true,  true,  true,  true,  /* 50 */
		true,  true,  true,  true,  false, true,  false, true,  /* 58 */
		false, true,  true,  true,  true,  true,  true,  true,  /* 60 */
		true,  true,  true,  true,  true,  true,  true,  true,  /* 68 */
		true,  true,  true,  true,  true,  true,  true,  true,  /* 70 */
		true,  true,  true,  false, true,  false, true,  false, /* 78 */
		false, false, false, false, false, false, false, false, /* 80 */
		false, false, false, false, false, false, false, false, /* 88 */
		false, false, false, false, false, false, false, false, /* 90 */
		false, false, false, false, false, false, false, false, /* 98 */
		false, false, false, false, false, false, false, false, /* A0 */
		false, false, false, false, false, false, false, false, /* A8 */
		false, false, false, false, false, false, false, false, /* B0 */
		false, false, false, false, false, false, false, false, /* B8 */
		false, false, false, false, false, false, false, false, /* C0 */
		false, false, false, false, false, false, false, false, /* C8 */
		false, false, false, false, false, false, false, false, /* D0 */
		false, false, false, false, false, false, false, false, /* D8 */
		false, false, false, false, false, false, false, false, /* E0 */
		false, false, false, false, false, false, false, false, /* E8 */
		false, false, false, false, false, false, false, false, /* F0 */
		false, false, false, false, false, false, false, false, /* F8 */
	};
	return no_escape[c];
}


/**
 * Obtains a set of markers delimiting sections in a URL string
 *
 * \param url_s		URL string
 * \param markers	Updated to mark sections in the URL string
 * \param joining	True iff URL string is a relative URL for joining
 */
static void nsurl__get_string_markers(const char * const url_s,
		struct url_markers *markers, bool joining)
{
	const char *pos = url_s; /** current position in url_s */
	bool is_http = false;
	bool trailing_whitespace = false;

	/* Initialise marker set */
	struct url_markers marker = { 0, 0, 0,   0, 0, 0,
				      0, 0, 0,   0, NSURL_SCHEME_OTHER };

	/* Skip any leading whitespace in url_s */
	while (ascii_is_space(*pos))
		pos++;

	/* Record start point */
	marker.start = pos - url_s;

	marker.scheme_end = marker.authority = marker.colon_first = marker.at =
			marker.colon_last = marker.path = marker.start;

	if (*pos == '\0') {
		/* Nothing but whitespace, early exit */
		marker.query = marker.fragment = marker.end = marker.path;
		*markers = marker;
		return;
	}

	/* Get scheme */
	if (ascii_is_alpha(*pos)) {
		pos++;

		while (*pos != ':' && *pos != '\0') {
			if (!ascii_is_alphanumerical(*pos) && (*pos != '+') &&
					(*pos != '-') && (*pos != '.')) {
				/* This character is not valid in the
				 * scheme */
				break;
			}
			pos++;
		}

		if (*pos == ':') {
			/* This delimits the end of the scheme */
			size_t off;

			marker.scheme_end = pos - url_s;

			off = marker.scheme_end - marker.start;

			/* Detect http(s) and mailto for scheme specifc
			 * normalisation */
			if (off == SLEN("http") &&
					(((*(pos - off + 0) == 'h') ||
					  (*(pos - off + 0) == 'H')) &&
					 ((*(pos - off + 1) == 't') ||
					  (*(pos - off + 1) == 'T')) &&
					 ((*(pos - off + 2) == 't') ||
					  (*(pos - off + 2) == 'T')) &&
					 ((*(pos - off + 3) == 'p') ||
					  (*(pos - off + 3) == 'P')))) {
				marker.scheme_type = NSURL_SCHEME_HTTP;
				is_http = true;
			} else if (off == SLEN("https") &&
					(((*(pos - off + 0) == 'h') ||
					  (*(pos - off + 0) == 'H')) &&
					 ((*(pos - off + 1) == 't') ||
					  (*(pos - off + 1) == 'T')) &&
					 ((*(pos - off + 2) == 't') ||
					  (*(pos - off + 2) == 'T')) &&
					 ((*(pos - off + 3) == 'p') ||
					  (*(pos - off + 3) == 'P')) &&
					 ((*(pos - off + 4) == 's') ||
					  (*(pos - off + 4) == 'S')))) {
				marker.scheme_type = NSURL_SCHEME_HTTPS;
				is_http = true;
			} else if (off == SLEN("file") &&
					(((*(pos - off + 0) == 'f') ||
					  (*(pos - off + 0) == 'F')) &&
					 ((*(pos - off + 1) == 'i') ||
					  (*(pos - off + 1) == 'I')) &&
					 ((*(pos - off + 2) == 'l') ||
					  (*(pos - off + 2) == 'L')) &&
					 ((*(pos - off + 3) == 'e') ||
					  (*(pos - off + 3) == 'E')))) {
				marker.scheme_type = NSURL_SCHEME_FILE;
			} else if (off == SLEN("ftp") &&
					(((*(pos - off + 0) == 'f') ||
					  (*(pos - off + 0) == 'F')) &&
					 ((*(pos - off + 1) == 't') ||
					  (*(pos - off + 1) == 'T')) &&
					 ((*(pos - off + 2) == 'p') ||
					  (*(pos - off + 2) == 'P')))) {
				marker.scheme_type = NSURL_SCHEME_FTP;
			} else if (off == SLEN("mailto") &&
					(((*(pos - off + 0) == 'm') ||
					  (*(pos - off + 0) == 'M')) &&
					 ((*(pos - off + 1) == 'a') ||
					  (*(pos - off + 1) == 'A')) &&
					 ((*(pos - off + 2) == 'i') ||
					  (*(pos - off + 2) == 'I')) &&
					 ((*(pos - off + 3) == 'l') ||
					  (*(pos - off + 3) == 'L')) &&
					 ((*(pos - off + 4) == 't') ||
					  (*(pos - off + 4) == 'T')) &&
					 ((*(pos - off + 5) == 'o') ||
					  (*(pos - off + 5) == 'O')))) {
				marker.scheme_type = NSURL_SCHEME_MAILTO;
			} else if (off == SLEN("data") &&
					(((*(pos - off + 0) == 'd') ||
					  (*(pos - off + 0) == 'D')) &&
					 ((*(pos - off + 1) == 'a') ||
					  (*(pos - off + 1) == 'A')) &&
					 ((*(pos - off + 2) == 't') ||
					  (*(pos - off + 2) == 'T')) &&
					 ((*(pos - off + 3) == 'a') ||
					  (*(pos - off + 3) == 'A')))) {
				marker.scheme_type = NSURL_SCHEME_DATA;
			}

			/* Skip over colon */
			pos++;

			/* Mark place as start of authority */
			marker.authority = marker.colon_first = marker.at =
					marker.colon_last = marker.path =
					pos - url_s;

		} else {
			/* Not found a scheme  */
			if (joining == false) {
				/* Assuming no scheme == http */
				marker.scheme_type = NSURL_SCHEME_HTTP;
				is_http = true;
			}
		}
	}

	/* Get authority
	 *
	 * Two slashes always indicates the start of an authority.
	 *
	 * We are more relaxed in the case of http:
	 *   a. when joining, one or more slashes indicates start of authority
	 *   b. when not joining, we assume authority if no scheme was present
	 * and in the case of mailto: when we assume there is an authority.
	 */
	if ((*pos == '/' && *(pos + 1) == '/') ||
			(is_http && ((joining && *pos == '/') ||
					(joining == false &&
					marker.scheme_end != marker.start))) ||
			marker.scheme_type == NSURL_SCHEME_MAILTO) {

		/* Skip over leading slashes */
		if (*pos == '/') {
			if (is_http == false) {
				if (*pos == '/') pos++;
				if (*pos == '/') pos++;
			} else {
				while (*pos == '/')
					pos++;
			}

			marker.authority = marker.colon_first = marker.at =
					marker.colon_last = marker.path =
					pos - url_s;
		}

		/* Need to get (or complete) the authority */
		while (*pos != '\0') {
			if (*pos == '/' || *pos == '?' || *pos == '#') {
				/* End of the authority */
				break;

			} else if (marker.scheme_type != NSURL_SCHEME_MAILTO &&
					*pos == ':' && marker.colon_first ==
					marker.authority) {
				/* could be username:password or host:port
				 * separator */
				marker.colon_first = pos - url_s;

			} else if (marker.scheme_type != NSURL_SCHEME_MAILTO &&
					*pos == ':' && marker.colon_first !=
					marker.authority) {
				/* could be host:port separator */
				marker.colon_last = pos - url_s;

			} else if (*pos == '@' && marker.at ==
					marker.authority) {
				/* Credentials @ host separator */
				marker.at = pos - url_s;
			}

			pos++;
		}

		marker.path = pos - url_s;

	} else if ((*pos == '\0' || *pos == '/') &&
			joining == false && is_http == true) {
		marker.path = pos - url_s;
	}

	/* Get path
	 *
	 * Needs to start with '/' if there's no authority
	 */
	if (*pos == '/' || ((marker.path == marker.authority) &&
			(*pos != '?') && (*pos != '#') && (*pos != '\0'))) {
		while (*(++pos) != '\0') {
			if (*pos == '?' || *pos == '#') {
				/* End of the path */
				break;
			}
		}
	}

	marker.query = pos - url_s;

	/* Get query */
	if (*pos == '?') {
		while (*(++pos) != '\0') {
			if (*pos == '#') {
				/* End of the query */
				break;
			}
		}
	}

	marker.fragment = pos - url_s;

	/* Get fragment */
	if (*pos == '#') {
		while (*(++pos) != '\0')
			;
	}

	/* We got to the end of url_s.
	 * Need to skip back over trailing whitespace to find end of URL */
	pos--;
	if (pos >= url_s && ascii_is_space(*pos)) {
		trailing_whitespace = true;
		while (pos >= url_s && ascii_is_space(*pos))
			pos--;
	}

	marker.end = pos + 1 - url_s;

	if (trailing_whitespace == true) {
		/* Ensure last url section doesn't pass end */
		if (marker.fragment > marker.end)
			marker.fragment = marker.end;
		if (marker.query > marker.end)
			marker.query = marker.end;
		if (marker.path > marker.end)
			marker.path = marker.end;
		if (marker.colon_last > marker.end)
			marker.colon_last = marker.end;
		if (marker.at > marker.end)
			marker.at = marker.end;
		if (marker.colon_last > marker.end)
			marker.colon_last = marker.end;
		if (marker.fragment > marker.end)
			marker.fragment = marker.end;
	}

	nslog_log(__FILE__, "", __LINE__, "marker.start: %"PRIsizet, marker.start);
	nslog_log(__FILE__, "", __LINE__, "marker.scheme_end: %"PRIsizet, marker.scheme_end);
	nslog_log(__FILE__, "", __LINE__, "marker.authority: %"PRIsizet, marker.authority);

	nslog_log(__FILE__, "", __LINE__, "marker.colon_first: %"PRIsizet, marker.colon_first);
	nslog_log(__FILE__, "", __LINE__, "marker.at: %"PRIsizet, marker.at);
	nslog_log(__FILE__, "", __LINE__, "marker.colon_last: %"PRIsizet, marker.colon_last);

	nslog_log(__FILE__, "", __LINE__, "marker.path: %"PRIsizet, marker.path);
	nslog_log(__FILE__, "", __LINE__, "marker.query: %"PRIsizet, marker.query);
	nslog_log(__FILE__, "", __LINE__, "marker.fragment: %"PRIsizet, marker.fragment);

	nslog_log(__FILE__, "", __LINE__, "marker.end: %"PRIsizet, marker.end);

	/* Got all the URL components pegged out now */
	*markers = marker;
}


/**
 * Remove dot segments from a path, as per rfc 3986, 5.2.4
 *
 * \param path		path to remove dot segments from ('\0' terminated)
 * \param output	path with dot segments removed
 * \return size of output
 */
static size_t nsurl__remove_dot_segments(char *path, char *output)
{
	char *path_pos = path;
	char *output_pos = output;

	while (*path_pos != '\0') {
		nslog_log(__FILE__, "", __LINE__, " in:%s", path_pos);
		nslog_log(__FILE__, "", __LINE__, "out:%.*s", (int)(output_pos - output), output);

		if (*path_pos == '.') {
			if (*(path_pos + 1) == '.' &&
					*(path_pos + 2) == '/') {
				/* Found prefix of "../" */
				path_pos += SLEN("../");
				continue;

			} else if (*(path_pos + 1) == '/') {
				/* Found prefix of "./" */
				path_pos += SLEN("./");
				continue;
			}
		} else if (*path_pos == '/' && *(path_pos + 1) == '.') {
			if (*(path_pos + 2) == '/') {
				/* Found prefix of "/./" */
				path_pos += SLEN("/.");
				continue;

			} else if (*(path_pos + 2) == '\0') {
				/* Found "/." at end of path */
				*(output_pos++) = '/';

				/* End of input path */
				break;

			} else if (*(path_pos + 2) == '.') {
				if (*(path_pos + 3) == '/') {
					/* Found prefix of "/../" */
					path_pos += SLEN("/..");

					if (output_pos > output)
						output_pos--;
					while (output_pos > output &&
							*output_pos != '/')
						output_pos--;

					continue;

				} else if (*(path_pos + 3) == '\0') {
					/* Found "/.." at end of path */

					while (output_pos > output &&
							*(output_pos -1 ) !='/')
						output_pos--;

					/* End of input path */
					break;
				}
			}
		} else if (*path_pos == '.') {
			if (*(path_pos + 1) == '\0') {
				/* Found "." at end of path */

				/* End of input path */
				break;

			} else if (*(path_pos + 1) == '.' &&
					*(path_pos + 2) == '\0') {
				/* Found ".." at end of path */

				/* End of input path */
				break;
			}
		}
		/* Copy first character into output path */
		*output_pos++ = *path_pos++;

		/* Copy up to but not including next '/' */
		  while ((*path_pos != '/') && (*path_pos != '\0'))
			*output_pos++ = *path_pos++;
	}

	return output_pos - output;
}


/**
 * Get the length of the longest section
 *
 * \param m	markers delimiting url sections in a string
 * \return the length of the longest section
 */
static size_t nsurl__get_longest_section(struct url_markers *m)
{
	size_t length = m->scheme_end - m->start;	/* scheme */

	if (length < m->at - m->authority)		/* credentials */
		length = m->at - m->authority;

	if (length < m->path - m->at)			/* host */
		length = m->path - m->at;

	if (length < m->query - m->path)		/* path */
		length = m->query - m->path;

	if (length < m->fragment - m->query)		/* query */
		length = m->fragment - m->query;

	if (length < m->end - m->fragment)		/* fragment */
		length = m->end - m->fragment;

	return length;
}


/**
 * Create the components of a NetSurf URL object for a section of a URL string
 *
 * \param url_s		URL string
 * \param section	Sets which section of URL string is to be normalised
 * \param pegs		Set of markers delimiting the URL string's sections
 * \param pos_norm	A buffer large enough for the normalised string (*3 + 1)
 * \param url		A NetSurf URL object, to which components may be added
 * \return NSERROR_OK on success, appropriate error otherwise
 *
 * The section of url_s is normalised appropriately.
 */
static nserror nsurl__create_from_section(const char * const url_s,
		const enum url_sections section,
		const struct url_markers *pegs,
		char *pos_norm,
		struct nsurl_components *url)
{
	nserror ret;
	int ascii_offset;
	int start = 0;
	int end = 0;
	const char *pos;
	const char *pos_url_s;
	char *norm_start = pos_norm;
	char *host;
	size_t copy_len;
	size_t length;
	size_t host_len;
	enum {
		NSURL_F_NO_PORT		= (1 << 0)
	} flags = 0;

	switch (section) {
	case URL_SCHEME:
		start = pegs->start;
		end = pegs->scheme_end;
		break;

	case URL_CREDENTIALS:
		start = pegs->authority;
		end = pegs->at;
		break;

	case URL_HOST:
		start = (pegs->at == pegs->authority &&
				*(url_s + pegs->at) != '@') ?
				pegs->at :
				pegs->at + 1;
		end = pegs->path;
		break;

	case URL_PATH:
		start = pegs->path;
		end = pegs->query;
		break;

	case URL_QUERY:
		start = (*(url_s + pegs->query) != '?') ?
				pegs->query :
				pegs->query + 1;
		end = pegs->fragment;
		break;

	case URL_FRAGMENT:
		start = (*(url_s + pegs->fragment) != '#') ?
				pegs->fragment :
				pegs->fragment + 1;
		end = pegs->end;
		break;
	}

	if (end < start)
		end = start;

	length = end - start;

	/* Stage 1: Normalise the required section */

	pos = pos_url_s = url_s + start;
	copy_len = 0;
	for (; pos < url_s + end; pos++) {
		if (*pos == '%' && (pos + 2 < url_s + end)) {
			/* Might be an escaped character needing unescaped */

			/* Find which character which was escaped */
			ascii_offset = ascii_hex_to_value_2_chars(*(pos + 1),
					*(pos + 2));

			if (ascii_offset < 0) {
				/* % with invalid hex digits. */
				copy_len++;
				continue;
			}

			if ((section != URL_SCHEME && section != URL_HOST) &&
				(nsurl__is_unreserved(ascii_offset) == false)) {
				/* This character should be escaped after all,
				 * just let it get copied */
				copy_len += 3;
				pos += 2;
				continue;
			}

			if (copy_len > 0) {
				/* Copy up to here */
				memcpy(pos_norm, pos_url_s, copy_len);
				pos_norm += copy_len;
				copy_len = 0;
			}

			/* Put the unescaped character in the normalised URL */
			*(pos_norm++) = (char)ascii_offset;
			pos += 2;
			pos_url_s = pos + 1;

			length -= 2;

		} else if ((section != URL_SCHEME && section != URL_HOST) &&
				(nsurl__is_no_escape(*pos) == false)) {

			/* This needs to be escaped */
			if (copy_len > 0) {
				/* Copy up to here */
				memcpy(pos_norm, pos_url_s, copy_len);
				pos_norm += copy_len;
				copy_len = 0;
			}

			/* escape */
			*(pos_norm++) = '%';
			*(pos_norm++) = digit2uppercase_hex(
					((unsigned char)*pos) >> 4);
			*(pos_norm++) = digit2uppercase_hex(
					((unsigned char)*pos) & 0xf);
			pos_url_s = pos + 1;

			length += 2;

		} else if ((section == URL_SCHEME || section == URL_HOST) &&
				ascii_is_alpha_upper(*pos)) {
			/* Lower case this letter */

			if (copy_len > 0) {
				/* Copy up to here */
				memcpy(pos_norm, pos_url_s, copy_len);
				pos_norm += copy_len;
				copy_len = 0;
			}
			/* Copy lower cased letter into normalised URL */
			*(pos_norm++) = ascii_to_lower(*pos);
			pos_url_s = pos + 1;

		} else {
			/* This character is safe in normalised URL */
			copy_len++;
		}
	}

	if (copy_len > 0) {
		/* Copy up to here */
		memcpy(pos_norm, pos_url_s, copy_len);
		pos_norm += copy_len;
	}

	/* Mark end of section */
	(*pos_norm) = '\0';

	/* Stage 2: Create the URL components for the required section */
	switch (section) {
	case URL_SCHEME:
		if (length == 0) {
			/* No scheme, assuming http */
			url->scheme = lwc_string_ref(corestring_lwc_http);
		} else {
			/* Add scheme to URL */
			if (lwc_intern_string(norm_start, length,
					&url->scheme) != lwc_error_ok) {
				return NSERROR_NOMEM;
			}
		}

		break;

	case URL_CREDENTIALS:
		url->username = NULL;
		url->password = NULL;

		/* file: URLs don't have credentials */
		if (url->scheme_type == NSURL_SCHEME_FILE) {
			break;
		}

		if (length != 0 && *norm_start != ':') {
			char *sec_start = norm_start;
			if (pegs->colon_first != pegs->authority &&
					pegs->at > pegs->colon_first + 1) {
				/* there's a password */
				sec_start += pegs->colon_first -
						pegs->authority + 1;
				if (lwc_intern_string(sec_start,
						pegs->at - pegs->colon_first -1,
						&url->password) !=
						lwc_error_ok) {
					return NSERROR_NOMEM;
				}

				/* update start pos and length for username */
				sec_start = norm_start;
				length -= pegs->at - pegs->colon_first;
			} else if (pegs->colon_first != pegs->authority &&
					pegs->at == pegs->colon_first + 1) {
				/* strip username colon */
				length--;
			}

			/* Username */
			if (lwc_intern_string(sec_start, length,
					&url->username) != lwc_error_ok) {
				return NSERROR_NOMEM;
			}
		}

		break;

	case URL_HOST:
		url->host = NULL;
		url->port = NULL;

		/* file: URLs don't have a host */
		if (url->scheme_type == NSURL_SCHEME_FILE) {
			break;
		}

		if (length != 0) {
			size_t colon = 0;
			char *sec_start = norm_start;
			if (pegs->at < pegs->colon_first &&
					pegs->colon_last == pegs->authority) {
				/* There's one colon and it's after @ marker */
				colon = pegs->colon_first;
			} else if (pegs->colon_last != pegs->authority) {
				/* There's more than one colon */
				colon = pegs->colon_last;
			} else {
				/* There's no colon that could be a port
				 * separator */
				flags |= NSURL_F_NO_PORT;
			}

			if (!(flags & NSURL_F_NO_PORT)) {
				/* Determine whether colon is a port separator
				 */
				sec_start += colon - pegs->at;
				while (++sec_start < norm_start + length) {
					if (!ascii_is_digit(*sec_start)) {
						/* Character after port isn't a
						 * digit; not a port separator
						 */
						flags |= NSURL_F_NO_PORT;
						break;
					}
				}
			}

			if (!(flags & NSURL_F_NO_PORT)) {
				/* There's a port */
				size_t skip = (pegs->at == pegs->authority) ?
						1 : 0;
				sec_start = norm_start + colon - pegs->at +
						skip;
				if (url->scheme != NULL &&
						url->scheme_type ==
						NSURL_SCHEME_HTTP &&
						length -
						(colon - pegs->at + skip) == 2 &&
						*sec_start == '8' &&
						*(sec_start + 1) == '0') {
					/* Scheme is http, and port is default
					 * (80) */
					flags |= NSURL_F_NO_PORT;
				}

				if (length <= (colon - pegs->at + skip)) {
					/* No space for a port after the colon
					 */
					flags |= NSURL_F_NO_PORT;
				}

				/* Add non-redundant ports to NetSurf URL */
				sec_start = norm_start + colon - pegs->at +
						skip;
				if (!(flags & NSURL_F_NO_PORT) &&
						lwc_intern_string(sec_start,
						length -
						(colon - pegs->at + skip),
						&url->port) != lwc_error_ok) {
					return NSERROR_NOMEM;
				}

				/* update length for host */
				skip = (pegs->at == pegs->authority) ? 0 : 1;
				length = colon - pegs->at - skip;
			}

			/* host */
			/* Encode host according to IDNA2008 */
			ret = idna_encode(norm_start, length, &host, &host_len);
			if (ret == NSERROR_OK) {
				/* valid idna encoding */
				if (lwc_intern_string(host, host_len,
						&url->host) != lwc_error_ok) {
					free(host);
					return NSERROR_NOMEM;
				}
				free(host);
			} else {
				/* fall back to straight interning */
				if (lwc_intern_string(norm_start, length,
						      &url->host) != lwc_error_ok) {
					return NSERROR_NOMEM;
				}
			}
		}

		break;

	case URL_PATH:
		if (length != 0) {
			if (lwc_intern_string(norm_start, length,
					&url->path) != lwc_error_ok) {
				return NSERROR_NOMEM;
			}
		} else if ((url->host != NULL &&
				url->scheme_type != NSURL_SCHEME_MAILTO) ||
				url->scheme_type == NSURL_SCHEME_FILE) {
			/* Set empty path to "/" if:
			 *   - there's a host and its not a mailto: URL
			 *   - its a file: URL
			 */
			if (lwc_intern_string("/", SLEN("/"),
					&url->path) != lwc_error_ok) {
				return NSERROR_NOMEM;
			}
		} else {
			url->path = NULL;
		}

		break;

	case URL_QUERY:
		if (length != 0) {
			if (lwc_intern_string(norm_start, length,
					&url->query) != lwc_error_ok) {
				return NSERROR_NOMEM;
			}
		} else {
			url->query = NULL;
		}

		break;

	case URL_FRAGMENT:
		if (length != 0) {
			if (lwc_intern_string(norm_start, length,
					&url->fragment) != lwc_error_ok) {
				return NSERROR_NOMEM;
			}
		} else {
			url->fragment = NULL;
		}

		break;
	}

	return NSERROR_OK;
}


/**
 * Get nsurl string info; total length, component lengths, & components present
 *
 * \param url		NetSurf URL components
 * \param parts		Which parts of the URL are required in the string
 * \param url_l		Updated to total string length
 * \param lengths	Updated with individual component lengths
 * \param pflags	Updated to contain relevant string flags
 */
static void nsurl__get_string_data(const struct nsurl_components *url,
		nsurl_component parts, size_t *url_l,
		struct nsurl_component_lengths *lengths,
		enum nsurl_string_flags *pflags)
{
	enum nsurl_string_flags flags = *pflags;
	*url_l = 0;

	/* Intersection of required parts and available parts gives
	 * the output parts */
	if (url->scheme && parts & NSURL_SCHEME) {
		flags |= NSURL_F_SCHEME;

		lengths->scheme = lwc_string_length(url->scheme);
		*url_l += lengths->scheme;
	}

	if (url->username && parts & NSURL_USERNAME) {
		flags |= NSURL_F_USERNAME;

		lengths->username = lwc_string_length(url->username);
		*url_l += lengths->username;
	}

	if (url->password && parts & NSURL_PASSWORD) {
		flags |= NSURL_F_PASSWORD;

		lengths->password = lwc_string_length(url->password);
		*url_l += SLEN(":") + lengths->password;
	}

	if (url->host && parts & NSURL_HOST) {
		flags |= NSURL_F_HOST;

		lengths->host = lwc_string_length(url->host);
		*url_l += lengths->host;
	}

	if (url->port && parts & NSURL_PORT) {
		flags |= NSURL_F_PORT;

		lengths->port = lwc_string_length(url->port);
		*url_l += SLEN(":") + lengths->port;
	}

	if (url->path && parts & NSURL_PATH) {
		flags |= NSURL_F_PATH;

		lengths->path = lwc_string_length(url->path);
		*url_l += lengths->path;
	}

	if (url->query && parts & NSURL_QUERY) {
		flags |= NSURL_F_QUERY;

		lengths->query = lwc_string_length(url->query);
		*url_l += lengths->query;
	}

	if (url->fragment && parts & NSURL_FRAGMENT) {
		flags |= NSURL_F_FRAGMENT;

		lengths->fragment = lwc_string_length(url->fragment);
		*url_l += lengths->fragment;
	}

	/* Turn on any spanned punctuation */
	if ((flags & NSURL_F_SCHEME) && (parts > NSURL_SCHEME)) {
		flags |= NSURL_F_SCHEME_PUNCTUATION;

		*url_l += SLEN(":");
	}

	if ((flags & NSURL_F_SCHEME) && (flags > NSURL_F_SCHEME) &&
			url->path && lwc_string_data(url->path)[0] == '/') {
		flags |= NSURL_F_AUTHORITY_PUNCTUATION;

		*url_l += SLEN("//");
	}

	if ((flags & (NSURL_F_USERNAME | NSURL_F_PASSWORD)) &&
				flags & NSURL_F_HOST) {
		flags |= NSURL_F_CREDENTIALS_PUNCTUATION;

		*url_l += SLEN("@");
	}

	/* spanned query question mark */
	if ((flags & ~(NSURL_F_QUERY | NSURL_F_FRAGMENT)) &&
	    (flags & NSURL_F_QUERY)) {
		flags |= NSURL_F_QUERY_PUNCTUATION;

		*url_l += SLEN("?");
	}

	/* spanned fragment hash mark */
	if ((flags & ~NSURL_F_FRAGMENT) && (flags & NSURL_F_FRAGMENT)) {
		flags |= NSURL_F_FRAGMENT_PUNCTUATION;

		*url_l += SLEN("#");
	}

	*pflags = flags;
}


/**
 * Copy url string into provided buffer
 *
 * \param url		NetSurf URL components
 * \param url_s		Updated to contain the string
 * \param l		Individual component lengths
 * \param flags		String flags
 */
static void nsurl__get_string(const struct nsurl_components *url, char *url_s,
		struct nsurl_component_lengths *l,
		enum nsurl_string_flags flags)
{
	char *pos;

	/* Copy the required parts into the url string */
	pos = url_s;

	if (flags & NSURL_F_SCHEME) {
		memcpy(pos, lwc_string_data(url->scheme), l->scheme);
		pos += l->scheme;
	}

	if (flags & NSURL_F_SCHEME_PUNCTUATION) {
		*(pos++) = ':';
	}

	if (flags & NSURL_F_AUTHORITY_PUNCTUATION) {
		*(pos++) = '/';
		*(pos++) = '/';
	}

	if (flags & NSURL_F_USERNAME) {
		memcpy(pos, lwc_string_data(url->username), l->username);
		pos += l->username;
	}

	if (flags & NSURL_F_PASSWORD) {
		*(pos++) = ':';
		memcpy(pos, lwc_string_data(url->password), l->password);
		pos += l->password;
	}

	if (flags & NSURL_F_CREDENTIALS_PUNCTUATION) {
		*(pos++) = '@';
	}

	if (flags & NSURL_F_HOST) {
		memcpy(pos, lwc_string_data(url->host), l->host);
		pos += l->host;
	}

	if (flags & NSURL_F_PORT) {
		*(pos++) = ':';
		memcpy(pos, lwc_string_data(url->port), l->port);
		pos += l->port;
	}

	if (flags & NSURL_F_PATH) {
		memcpy(pos, lwc_string_data(url->path), l->path);
		pos += l->path;
	}

	if (flags & NSURL_F_QUERY) {
		if (flags & NSURL_F_QUERY_PUNCTUATION)
			*(pos++) = '?';
		memcpy(pos, lwc_string_data(url->query), l->query);
		pos += l->query;
	}

	if (flags & NSURL_F_FRAGMENT) {
		if (flags & NSURL_F_FRAGMENT_PUNCTUATION)
			*(pos++) = '#';
		memcpy(pos, lwc_string_data(url->fragment), l->fragment);
		pos += l->fragment;
	}

	*pos = '\0';
}


/* exported interface, documented in nsurl.h */
nserror nsurl__components_to_string(
		const struct nsurl_components *components,
		nsurl_component parts, size_t pre_padding,
		char **url_s_out, size_t *url_l_out)
{
	struct nsurl_component_lengths str_len = { 0, 0, 0, 0,  0, 0, 0, 0 };
	enum nsurl_string_flags str_flags = 0;
	size_t url_l;
	char *url_s;

	assert(components != NULL);

	/* Get the string length and find which parts of url need copied */
	nsurl__get_string_data(components, parts, &url_l,
			&str_len, &str_flags);

	if (url_l == 0) {
		return NSERROR_BAD_URL;
	}

	/* Allocate memory for url string */
	url_s = malloc(pre_padding + url_l + 1); /* adding 1 for '\0' */
	if (url_s == NULL) {
		return NSERROR_NOMEM;
	}

	/* Copy the required parts into the url string */
	nsurl__get_string(components, url_s + pre_padding, &str_len, str_flags);

	*url_s_out = url_s;
	*url_l_out = url_l;

	return NSERROR_OK;
}


/**
 * Calculate hash value
 *
 * \param url		NetSurf URL object to set hash value for
 */
void nsurl__calc_hash(nsurl *url)
{
	uint32_t hash = 0;

	if (url->components.scheme)
		hash ^= lwc_string_hash_value(url->components.scheme);

	if (url->components.username)
		hash ^= lwc_string_hash_value(url->components.username);

	if (url->components.password)
		hash ^= lwc_string_hash_value(url->components.password);

	if (url->components.host)
		hash ^= lwc_string_hash_value(url->components.host);

	if (url->components.port)
		hash ^= lwc_string_hash_value(url->components.port);

	if (url->components.path)
		hash ^= lwc_string_hash_value(url->components.path);

	if (url->components.query)
		hash ^= lwc_string_hash_value(url->components.query);

	url->hash = hash;
}

/**
 * Check that a hostname is valid
 *
 * Valid hostnames are valid DNS names.  This means they must consist only of
 * the ASCII characters a-z A-Z 0-9 '.', '_', or '-'.
 *
 * Unfortunately we also need to deal with IPv6 literals which are constrained
 * but strange.  Surrounded by '[' and ']' there are hex digits and colons
 *
 * \param host	The hostname to check
 * \return NSERROR_OK if the hostname is valid
 */
static nserror nsurl__check_host_valid(lwc_string *host)
{
	const char *chptr = lwc_string_data(host);
	size_t nchrs = lwc_string_length(host);

	if (*chptr == '[' && chptr[nchrs-1] == ']') {
		/* Treat this as an IPv6 Literal */
		chptr++;
		nchrs -= 2;
		while (nchrs--) {
			const char ch = *chptr++;
			if (!ascii_is_hex(ch) && ch != ':') {
				/* Not hex digit or colon */
				return NSERROR_INVALID;
			}
		}
		return NSERROR_OK;
	}

	while (nchrs--) {
		const char ch = *chptr++;
		if (!ascii_is_alphanumerical(ch) && !(ch == '.' || ch == '-' || ch == '_')) {
			/* Not alphanumeric dot or dash */
			return NSERROR_INVALID;
		}
	}
	return NSERROR_OK;
}

/******************************************************************************
 * NetSurf URL Public API                                                     *
 ******************************************************************************/

/* exported interface, documented in nsurl.h */
nserror nsurl_create(const char * const url_s, nsurl **url)
{
	struct url_markers m;
	struct nsurl_components c;
	size_t length;
	char *buff;
	nserror e = NSERROR_OK;
	bool match;

	assert(url_s != NULL);

	/* Peg out the URL sections */
	nsurl__get_string_markers(url_s, &m, false);

	/* Get the length of the longest section */
	length = nsurl__get_longest_section(&m);

	/* Allocate enough memory to url escape the longest section */
	buff = malloc(length * 3 + 1);
	if (buff == NULL)
		return NSERROR_NOMEM;

	/* Set scheme type */
	c.scheme_type = m.scheme_type;

	/* Build NetSurf URL object from sections */
	e |= nsurl__create_from_section(url_s, URL_SCHEME, &m, buff, &c);
	e |= nsurl__create_from_section(url_s, URL_CREDENTIALS, &m, buff, &c);
	e |= nsurl__create_from_section(url_s, URL_HOST, &m, buff, &c);
	e |= nsurl__create_from_section(url_s, URL_PATH, &m, buff, &c);
	e |= nsurl__create_from_section(url_s, URL_QUERY, &m, buff, &c);
	e |= nsurl__create_from_section(url_s, URL_FRAGMENT, &m, buff, &c);

	/* Finished with buffer */
	free(buff);

	if (e != NSERROR_OK) {
		nsurl__components_destroy(&c);
		return NSERROR_NOMEM;
	}

	/* Validate URL */
	if ((lwc_string_isequal(c.scheme, corestring_lwc_http,
			&match) == lwc_error_ok && match == true) ||
			(lwc_string_isequal(c.scheme, corestring_lwc_https,
			&match) == lwc_error_ok && match == true)) {
		/* http, https must have host */
		if (c.host == NULL) {
			nsurl__components_destroy(&c);
			return NSERROR_BAD_URL;
		}
		/* host names must be a-z, 0-9, hyphen, underscore, and dot only */
		if (nsurl__check_host_valid(c.host) != NSERROR_OK) {
			nsurl__components_destroy(&c);
			return NSERROR_BAD_URL;
		}
	}

	e = nsurl__components_to_string(&c, NSURL_WITH_FRAGMENT,
			offsetof(nsurl, string), (char **)url, &length);
	if (e != NSERROR_OK) {
		return e;
	}

	(*url)->components = c;
	(*url)->length = length;

	/* Get the nsurl's hash */
	nsurl__calc_hash(*url);

	/* Give the URL a reference */
	(*url)->count = 1;

	return NSERROR_OK;
}


/* exported interface, documented in nsurl.h */
nserror nsurl_join(const nsurl *base, const char *rel, nsurl **joined)
{
	struct url_markers m;
	struct nsurl_components c;
	size_t length;
	char *buff;
	char *buff_pos;
	char *buff_start;
	nserror error = 0;
	enum {
		NSURL_F_REL		=  0,
		NSURL_F_BASE_SCHEME	= (1 << 0),
		NSURL_F_BASE_AUTHORITY	= (1 << 1),
		NSURL_F_BASE_PATH	= (1 << 2),
		NSURL_F_MERGED_PATH	= (1 << 3),
		NSURL_F_BASE_QUERY	= (1 << 4)
	} joined_parts;

	assert(base != NULL);
	assert(rel != NULL);

	nslog_log(__FILE__, "", __LINE__, "base: \"%s\", rel: \"%s\"", nsurl_access(base), rel);

	/* Peg out the URL sections */
	nsurl__get_string_markers(rel, &m, true);

	/* Get the length of the longest section */
	length = nsurl__get_longest_section(&m);

	/* Initially assume that the joined URL can be formed entierly from
	 * the relative URL.
	 */
	joined_parts = NSURL_F_REL;

	/* Update joined_compnents to indicate any required parts from the
	 * base URL.
	 */
	if (m.scheme_end - m.start <= 0) {
		/* The relative url has no scheme.
		 * Use base URL's scheme. */
		joined_parts |= NSURL_F_BASE_SCHEME;

		if (m.path - m.authority <= 0) {
			/* The relative URL has no authority.
			 * Use base URL's authority. */
			joined_parts |= NSURL_F_BASE_AUTHORITY;

			if (m.query - m.path <= 0) {
				/* The relative URL has no path.
				 * Use base URL's path. */
				joined_parts |= NSURL_F_BASE_PATH;

				if (m.fragment - m.query <= 0) {
					/* The relative URL has no query.
					 * Use base URL's query. */
					joined_parts |= NSURL_F_BASE_QUERY;
				}

			} else if (*(rel + m.path) != '/') {
				/* Relative URL has relative path */
				joined_parts |= NSURL_F_MERGED_PATH;
			}
		}
	}

	/* Allocate enough memory to url escape the longest section, plus
	 * space for path merging (if required).
	 */
	if (joined_parts & NSURL_F_MERGED_PATH) {
		/* Need to merge paths */
		length += (base->components.path != NULL) ?
				lwc_string_length(base->components.path) : 0;
	}
	length *= 4;
	/* Plus space for removing dots from path */
	length += (m.query - m.path) + ((base->components.path != NULL) ?
			lwc_string_length(base->components.path) : 0);

	buff = malloc(length + 5);
	if (buff == NULL) {
		return NSERROR_NOMEM;
	}

	buff_pos = buff;

	/* Form joined URL from base or rel components, as appropriate */

	if (joined_parts & NSURL_F_BASE_SCHEME) {
		c.scheme_type = base->components.scheme_type;

		c.scheme = nsurl__component_copy(base->components.scheme);
	} else {
		c.scheme_type = m.scheme_type;

		error = nsurl__create_from_section(rel, URL_SCHEME, &m,	buff, &c);
		if (error != NSERROR_OK) {
			free(buff);
			return error;
		}
	}

	if (joined_parts & NSURL_F_BASE_AUTHORITY) {
		c.username = nsurl__component_copy(base->components.username);
		c.password = nsurl__component_copy(base->components.password);
		c.host = nsurl__component_copy(base->components.host);
		c.port = nsurl__component_copy(base->components.port);
	} else {
		error = nsurl__create_from_section(rel, URL_CREDENTIALS, &m,
						   buff, &c);
		if (error == NSERROR_OK) {
			error = nsurl__create_from_section(rel, URL_HOST, &m,
							   buff, &c);
		}
		if (error != NSERROR_OK) {
			free(buff);
			return error;
		}
	}

	if (joined_parts & NSURL_F_BASE_PATH) {
		c.path = nsurl__component_copy(base->components.path);

	} else if (joined_parts & NSURL_F_MERGED_PATH) {
		struct url_markers m_path;
		size_t new_length;

		/* RFC3986 said to append relative path to "/" if the
		 * base path had no path and an authority.
		 *
		 * However, that specification is redundant, and base paths
		 * are normalised, so file, http, and https URLs will always
		 * have a non-empty path.  (Empty paths become "/".)
		 */

		{
			/* Append relative path to all but last segment of
			 * base path. */
			size_t path_end = lwc_string_length(
					base->components.path);
			const char *path = lwc_string_data(
					base->components.path);

			while (*(path + path_end) != '/' &&
					path_end != 0) {
				path_end--;
			}
			if (*(path + path_end) == '/')
				path_end++;

			/* Copy the base part */
			memcpy(buff_pos, path, path_end);
			buff_pos += path_end;

			/* Copy the relative part */
			memcpy(buff_pos, rel + m.path, m.query - m.path);
			buff_pos += m.query - m.path;
		}

		/* add termination to string */
		*buff_pos++ = '\0';

		new_length = nsurl__remove_dot_segments(buff, buff_pos);

		m_path.path = 0;
		m_path.query = new_length;

		buff_start = buff_pos + new_length;
		error = nsurl__create_from_section(buff_pos, URL_PATH, &m_path,
				buff_start, &c);
		if (error != NSERROR_OK) {
			free(buff);
			return error;
		}

	} else {
		struct url_markers m_path;
		size_t new_length;

		memcpy(buff_pos, rel + m.path, m.query - m.path);
		buff_pos += m.query - m.path;
		*(buff_pos++) = '\0';

		new_length = nsurl__remove_dot_segments(buff, buff_pos);

		m_path.path = 0;
		m_path.query = new_length;

		buff_start = buff_pos + new_length;

		error = nsurl__create_from_section(buff_pos, URL_PATH, &m_path,
				buff_start, &c);
		if (error != NSERROR_OK) {
			free(buff);
			return error;
		}
	}

	if (joined_parts & NSURL_F_BASE_QUERY) {
		c.query = nsurl__component_copy(base->components.query);
	} else {
		error = nsurl__create_from_section(rel, URL_QUERY, &m,
				buff, &c);
		if (error != NSERROR_OK) {
			free(buff);
			return error;
		}
	}

	error = nsurl__create_from_section(rel, URL_FRAGMENT, &m, buff, &c);

	/* Free temporary buffer */
	free(buff);

	if (error != NSERROR_OK) {
		return error;
	}

	error = nsurl__components_to_string(&c, NSURL_WITH_FRAGMENT,
			offsetof(nsurl, string), (char **)joined, &length);
	if (error != NSERROR_OK) {
		return error;
	}

	(*joined)->components = c;
	(*joined)->length = length;

	/* Get the nsurl's hash */
	nsurl__calc_hash(*joined);

	/* Give the URL a reference */
	(*joined)->count = 1;

	return NSERROR_OK;
}
