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

#include <string.h>
#include <assert.h>
#include <libwapcaplet/libwapcaplet.h>
#include <dom/dom.h>

#include "utils/errors.h"
#include "utils/corestrings.h"
#include "utils/utils.h"
#include "utils/http.h"
#include "utils/log.h"
#include "utils/messages.h"
#include "content/content_protected.h"
#include "content/content_factory.h"
#include "content/fetch.h"
#include "content/hlcache.h"
#include "content/llcache.h"
#include "utils/nsurl.h"
#include "desktop/system_colour.h"

#include "css/css.h"
#include "css/hints.h"
#include "css/internal.h"

#include "macsurf_debug.h"

/* Define to trace import fetches */
#undef NSCSS_IMPORT_TRACE

/** Screen DPI in fixed point units: defaults to 90, which RISC OS uses */
css_fixed nscss_screen_dpi = F_90;

struct content_css_data;

/**
 * Type of callback called when a CSS object has finished
 *
 * \param css  CSS object that has completed
 * \param pw   Client-specific data
 */
typedef void (*nscss_done_callback)(struct content_css_data *css, void *pw);

/**
 * CSS content data
 */
struct content_css_data
{
	css_stylesheet *sheet;		/**< Stylesheet object */
	char *charset;			/**< Character set of stylesheet */
	struct nscss_import *imports;	/**< Array of imported sheets */
	uint32_t import_count;		/**< Number of sheets imported */
	uint32_t next_to_register;	/**< Index of next import to register */
	nscss_done_callback done;	/**< Completion callback */
	void *pw;			/**< Client data */
};

/**
 * CSS content data
 */
typedef struct nscss_content
{
	struct content base;		/**< Underlying content object */

	struct content_css_data data;	/**< CSS data */
} nscss_content;

/**
 * Context for import fetches
 */
typedef struct {
	struct content_css_data *css;		/**< Object containing import */
	uint32_t index;				/**< Index into parent sheet's
						 *   imports array */
} nscss_import_ctx;

static bool nscss_convert(struct content *c);
static void nscss_destroy(struct content *c);
static nserror nscss_clone(const struct content *old, struct content **newc);
static bool nscss_matches_quirks(const struct content *c, bool quirks);
static content_type nscss_content_type(void);

static nserror nscss_create_css_data(struct content_css_data *c,
		const char *url, const char *charset, bool quirks,
		nscss_done_callback done, void *pw);
static css_error nscss_process_css_data(struct content_css_data *c, const char *data,
		unsigned int size);
static css_error nscss_convert_css_data(struct content_css_data *c);
static void nscss_destroy_css_data(struct content_css_data *c);

static void nscss_content_done(struct content_css_data *css, void *pw);
static css_error nscss_handle_import(void *pw, css_stylesheet *parent,
		lwc_string *url);
static nserror nscss_import(hlcache_handle *handle,
		const hlcache_event *event, void *pw);
static css_error nscss_import_complete(nscss_import_ctx *ctx);

static css_error nscss_register_imports(struct content_css_data *c);
static css_error nscss_register_import(struct content_css_data *c,
		const hlcache_handle *import);


static css_stylesheet *blank_import;


/**
 * Initialise a CSS content
 *
 * \param handler content handler
 * \param imime_type mime-type
 * \param params Content-Type parameters
 * \param llcache handle to content
 * \param fallback_charset The character set to fallback to.
 * \param quirks allow quirks
 * \param c Content to initialise
 * \return NSERROR_OK or error cod eon faliure
 */
static nserror
nscss_create(const content_handler *handler,
	     lwc_string *imime_type,
	     const http_parameter *params,
	     llcache_handle *llcache,
	     const char *fallback_charset,
	     bool quirks,
	     struct content **c)
{
	nscss_content *result;
	const char *charset = NULL;
	const char *xnsbase = NULL;
	lwc_string *charset_value = NULL;
	nserror error;

	MS_LOG("nscss create");
	{
		nsurl *u = llcache_handle_get_url(llcache);
		macsurf_debug_log_writef(
			"nscss create url=%s",
			u ? nsurl_access(u) : "(null)");
	}

	result = calloc(1, sizeof(nscss_content));
	if (result == NULL)
		return NSERROR_NOMEM;

	error = content__init(&result->base, handler, imime_type,
			params, llcache, fallback_charset, quirks);
	if (error != NSERROR_OK) {
		free(result);
		return error;
	}

	/* Find charset specified on HTTP layer, if any */
	error = http_parameter_list_find_item(params, corestring_lwc_charset,
			&charset_value);
	if (error != NSERROR_OK || lwc_string_length(charset_value) == 0) {
		/* No charset specified, use fallback, if any */
		/** \todo libcss will take this as gospel, which is wrong */
		charset = fallback_charset;
	} else {
		charset = lwc_string_data(charset_value);
	}

	/* Compute base URL for stylesheet */
	xnsbase = llcache_handle_get_header(llcache, "X-NS-Base");
	if (xnsbase == NULL) {
		xnsbase = nsurl_access(content_get_url(&result->base));
	}

	error = nscss_create_css_data(&result->data,
			xnsbase, charset, result->base.quirks,
			nscss_content_done, result);
	if (error != NSERROR_OK) {
		content_broadcast_error(&result->base, NSERROR_NOMEM, NULL);
		lwc_string_unref(charset_value);
		free(result);
		return error;
	}

	lwc_string_unref(charset_value);

	*c = (struct content *) result;

	return NSERROR_OK;
}

/**
 * Create a struct content_css_data, creating a stylesheet object
 *
 * \param c        Struct to populate
 * \param url      URL of stylesheet
 * \param charset  Stylesheet charset
 * \param quirks   Stylesheet quirks mode
 * \param done     Callback to call when content has completed
 * \param pw       Client data for \a done
 * \return NSERROR_OK on success, NSERROR_NOMEM on memory exhaustion
 */
static nserror nscss_create_css_data(struct content_css_data *c,
		const char *url, const char *charset, bool quirks,
		nscss_done_callback done, void *pw)
{
	css_error error;
	css_stylesheet_params params;

	c->pw = pw;
	c->done = done;
	c->next_to_register = (uint32_t) -1;
	c->import_count = 0;
	c->imports = NULL;
	if (charset != NULL)
		c->charset = strdup(charset);
	else
		c->charset = NULL;

	params.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
	params.level = CSS_LEVEL_DEFAULT;
	params.charset = charset;
	params.url = url;
	params.title = NULL;
	params.allow_quirks = quirks;
	params.inline_style = false;
	params.resolve = nscss_resolve_url;
	params.resolve_pw = NULL;
	params.import = nscss_handle_import;
	params.import_pw = c;
	params.color = ns_system_colour;
	params.color_pw = NULL;
	params.font = NULL;
	params.font_pw = NULL;

	error = css_stylesheet_create(&params, &c->sheet);
	if (error != CSS_OK) {
		return NSERROR_NOMEM;
	}

	return NSERROR_OK;
}

/**
 * Process CSS source data
 *
 * \param c     Content structure
 * \param data  Data to process
 * \param size  Number of bytes to process
 * \return true on success, false on failure
 */
/* fixes115 — count column tracks in a grid-template-columns value
 * (the substring between `:` and `;`/`}` of the declaration).
 *
 * Handles the dominant patterns mactrove and similar Drupal/CMS themes
 * use without trying to be a full CSS Grid grammar parser:
 *   none / auto                                -> 1
 *   <track> <track> <track> ...                -> count of tokens
 *   repeat(<int>, <inner-tracks>)              -> int * count(inner)
 *   repeat(auto-fill|auto-fit, ...)            -> 3 * count(inner) (heuristic)
 *   minmax(...) / fit-content(...) / calc(...) -> counts as 1 track
 *
 * Pure text-level parser — does NOT depend on libcss. We rewrite the
 * declaration to `-macsurf-grid: N` before libcss parses the sheet,
 * so the existing -macsurf-grid select/layout path handles it. */
static int
macsurf__count_grid_columns_text(const char *p, const char *end)
{
	int tracks = 0;
	int depth = 0;
	int in_repeat = 0;
	int repeat_mult = 1;
	int repeat_inner = 0;
	int repeat_depth = 0;

	while (p < end) {
		char ch = *p;

		if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' ||
				ch == ',') {
			p++;
			continue;
		}

		if (ch == '(') {
			depth++;
			p++;
			continue;
		}

		if (ch == ')') {
			if (depth > 0) depth--;
			p++;
			if (in_repeat && depth < repeat_depth) {
				if (repeat_inner < 1) repeat_inner = 1;
				tracks += repeat_mult * repeat_inner;
				in_repeat = 0;
				repeat_inner = 0;
				repeat_mult = 1;
			}
			continue;
		}

		if (depth == 0 && (end - p) >= 7 &&
				(p[0] == 'r' || p[0] == 'R') &&
				(p[1] == 'e' || p[1] == 'E') &&
				(p[2] == 'p' || p[2] == 'P') &&
				(p[3] == 'e' || p[3] == 'E') &&
				(p[4] == 'a' || p[4] == 'A') &&
				(p[5] == 't' || p[5] == 'T') &&
				p[6] == '(') {
			p += 7;
			depth++;
			in_repeat = 1;
			repeat_depth = depth;
			repeat_mult = 1;
			repeat_inner = 0;
			while (p < end && (*p == ' ' || *p == '\t')) p++;
			if (p < end && *p >= '0' && *p <= '9') {
				int n = 0;
				while (p < end && *p >= '0' && *p <= '9') {
					n = n * 10 + (*p - '0');
					p++;
				}
				if (n > 0 && n < 1000) repeat_mult = n;
			} else {
				/* auto-fill / auto-fit / unknown — heuristic. */
				repeat_mult = 3;
				while (p < end && *p != ',' && *p != ')') p++;
			}
			while (p < end && (*p == ' ' || *p == '\t')) p++;
			if (p < end && *p == ',') p++;
			continue;
		}

		/* Any other non-whitespace token at the current depth counts
		 * as one track at its level. Skip the rest of the token until
		 * a separator. Nested-function bodies (minmax, fit-content,
		 * calc) are skipped via the depth tracking above. */
		if (depth == 0) {
			tracks++;
		} else if (in_repeat && depth == repeat_depth) {
			repeat_inner++;
		}
		while (p < end) {
			char c2 = *p;
			if (c2 == ' ' || c2 == '\t' || c2 == '\n' ||
					c2 == '\r' || c2 == ',' ||
					c2 == '(' || c2 == ')') break;
			p++;
		}
	}

	if (tracks < 1) tracks = 1;
	if (tracks > 255) tracks = 255;
	return tracks;
}


/* fixes115 — case-insensitive 1-byte compare; checks word boundary so
 * "grid-template-columns" doesn't match `xgrid-template-columns`. */
static int
macsurf__match_prop_name(const char *buf, size_t len, size_t pos,
		const char *name, size_t name_len)
{
	size_t i;
	if (pos + name_len > len) return 0;
	if (pos > 0) {
		char prev = buf[pos - 1];
		if ((prev >= 'a' && prev <= 'z') ||
				(prev >= 'A' && prev <= 'Z') ||
				(prev >= '0' && prev <= '9') ||
				prev == '-' || prev == '_') {
			return 0;
		}
	}
	for (i = 0; i < name_len; i++) {
		char b = buf[pos + i];
		char n = name[i];
		if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
		if (n >= 'A' && n <= 'Z') n = (char)(n - 'A' + 'a');
		if (b != n) return 0;
	}
	return 1;
}


/* fixes117 — extract up to MAX_TRACKS track-width tokens from a
 * grid-template-columns value, expanding repeat(N, ...) and collapsing
 * minmax()/fit-content()/calc() to a single token. Each emitted token
 * is appended to `out` followed by a single space; the total bytes
 * written is returned. Tracks beyond MAX_TRACKS are silently dropped.
 *
 * Token normalisation: each token is emitted verbatim from its source
 * substring (e.g. "210px", "1fr", "50%", "auto"). The libcss parser
 * (p_macsurf_grid.c V2 grammar) only recognises <length> / <flex> /
 * <percentage>; idents and minmax-results fall back to "1fr" so the
 * grid still has equal cells in those positions. */
#define MACSURF_GRID_TRACK_MAX 8

static int
macsurf__emit_one_track(char *out, size_t cap,
		const char *tok, size_t tok_len)
{
	const char *fr_fallback = "1fr";
	const char *tok_to_emit = tok;
	size_t emit_len = tok_len;
	size_t i;
	bool is_value = false;

	/* Detect if the token looks like a value (starts with digit, '.',
	 * '+' or '-'). Anything else (auto, min-content, max-content,
	 * leftover minmax/calc) becomes "1fr". */
	if (tok_len > 0) {
		char c = tok[0];
		if ((c >= '0' && c <= '9') || c == '.' || c == '+' ||
				c == '-') {
			is_value = true;
		}
	}
	if (!is_value) {
		tok_to_emit = fr_fallback;
		emit_len = 3;
	}

	if (emit_len + 1 > cap) return -1;
	for (i = 0; i < emit_len; i++) {
		out[i] = tok_to_emit[i];
	}
	out[emit_len] = ' ';
	return (int)(emit_len + 1);
}

/* Walk a flat (paren-free, repeat-free) chunk of track tokens, emitting
 * one normalised token per track up to `room` remaining slots. Returns
 * the number of tracks emitted; *out_pos advanced; *room decremented. */
static void
macsurf__emit_flat_tracks(const char *p, const char *end,
		char *out, size_t cap, size_t *out_pos,
		int *room, int *emitted)
{
	while (p < end && *room > 0) {
		const char *tok_start;
		size_t tok_len;
		int n;

		while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' ||
				*p == '\r' || *p == ',')) p++;
		if (p >= end) break;

		tok_start = p;
		while (p < end && *p != ' ' && *p != '\t' && *p != '\n' &&
				*p != '\r' && *p != ',' && *p != '(' &&
				*p != ')') {
			p++;
		}
		tok_len = (size_t)(p - tok_start);
		if (tok_len == 0) {
			/* fixes118a: p is sitting on '(' or ')' which the
			 * whitespace skip above doesn't consume and the
			 * tokeniser above stops at -- advance past it or
			 * we loop forever. Triggered by repeat() inner
			 * content with nested parens like
			 * `repeat(auto-fill, minmax(150px, 1fr))`. */
			if (p < end) p++;
			continue;
		}

		/* fixes148b3: if this token is immediately followed by '(',
		 * it's a function name (minmax / fit-content / calc / ...).
		 * Skip the balanced parenthesised body so its inner tokens
		 * don't get flat-tokenised into extra tracks, and emit ONE
		 * "1fr" for the function as a whole -- matching the top-
		 * level emit_grid_tracks handling.  Without this branch,
		 * repeat(3, minmax(100px, 1fr)) flat-tokenises each
		 * minmax(...) inside the repeat into three tokens
		 * (minmax-name -> "1fr", 100px, 1fr) and emits 3 * 3 = 9
		 * tokens (capped to MAX 8). */
		if (p < end && *p == '(') {
			int depth = 1;
			int nfr;
			p++;
			while (p < end && depth > 0) {
				if (*p == '(') depth++;
				else if (*p == ')') {
					depth--;
					if (depth == 0) { p++; break; }
				}
				p++;
			}
			nfr = macsurf__emit_one_track(out + *out_pos,
					cap - *out_pos, "1fr", 3);
			if (nfr < 0) return;
			*out_pos += (size_t)nfr;
			(*room)--;
			(*emitted)++;
			continue;
		}

		n = macsurf__emit_one_track(out + *out_pos,
				cap - *out_pos, tok_start, tok_len);
		if (n < 0) return;
		*out_pos += (size_t)n;
		(*room)--;
		(*emitted)++;
	}
}

/* fixes117 — emit a track-list rewrite for one grid-template-columns
 * declaration. Returns bytes written to `out` (excluding trailing NUL),
 * or 0 if the value couldn't be coerced into a usable track-list (in
 * which case the caller should fall back to the count-only rewrite). */
static size_t
macsurf__emit_grid_tracks(const char *p, const char *end,
		char *out, size_t cap)
{
	size_t pos = 0;
	int emitted = 0;
	int room = MACSURF_GRID_TRACK_MAX;

	while (p < end && room > 0) {
		while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' ||
				*p == '\r' || *p == ',')) p++;
		if (p >= end) break;

		/* repeat(N, T1 T2 ...) -> emit T1 T2 ... N times. */
		if ((end - p) >= 7 &&
				(p[0] == 'r' || p[0] == 'R') &&
				(p[1] == 'e' || p[1] == 'E') &&
				(p[2] == 'p' || p[2] == 'P') &&
				(p[3] == 'e' || p[3] == 'E') &&
				(p[4] == 'a' || p[4] == 'A') &&
				(p[5] == 't' || p[5] == 'T') &&
				p[6] == '(') {
			int mult = 1;
			const char *inner_start;
			const char *inner_end;
			int depth = 1;
			int r;

			p += 7;
			while (p < end && (*p == ' ' || *p == '\t')) p++;
			if (p < end && *p >= '0' && *p <= '9') {
				int n = 0;
				while (p < end && *p >= '0' && *p <= '9') {
					n = n * 10 + (*p - '0');
					p++;
				}
				if (n > 0 && n < 1000) mult = n;
			} else {
				mult = 3;
				while (p < end && *p != ',' && *p != ')') p++;
			}
			while (p < end && (*p == ' ' || *p == '\t')) p++;
			if (p < end && *p == ',') p++;

			inner_start = p;
			while (p < end && depth > 0) {
				if (*p == '(') depth++;
				else if (*p == ')') {
					depth--;
					if (depth == 0) break;
				}
				p++;
			}
			inner_end = p;
			if (p < end && *p == ')') p++;

			for (r = 0; r < mult && room > 0; r++) {
				macsurf__emit_flat_tracks(inner_start,
						inner_end, out, cap, &pos,
						&room, &emitted);
			}
			continue;
		}

		/* minmax(A, B) / fit-content(N) / calc(...) -> single
		 * "1fr"-equivalent token. */
		if (p < end &&
				(((end - p) >= 7 &&
				  (p[0] == 'm' || p[0] == 'M') &&
				  (p[1] == 'i' || p[1] == 'I') &&
				  (p[2] == 'n' || p[2] == 'N') &&
				  (p[3] == 'm' || p[3] == 'M') &&
				  (p[4] == 'a' || p[4] == 'A') &&
				  (p[5] == 'x' || p[5] == 'X') &&
				  p[6] == '(') ||
				 ((end - p) >= 12 &&
				  (p[0] == 'f' || p[0] == 'F') &&
				  (p[1] == 'i' || p[1] == 'I') &&
				  (p[2] == 't' || p[2] == 'T') &&
				  p[3] == '-' &&
				  (p[4] == 'c' || p[4] == 'C') &&
				  p[11] == '(') ||
				 ((end - p) >= 5 &&
				  (p[0] == 'c' || p[0] == 'C') &&
				  (p[1] == 'a' || p[1] == 'A') &&
				  (p[2] == 'l' || p[2] == 'L') &&
				  (p[3] == 'c' || p[3] == 'C') &&
				  p[4] == '('))) {
			int depth = 0;
			int n;
			while (p < end) {
				if (*p == '(') depth++;
				else if (*p == ')') {
					depth--;
					if (depth == 0) { p++; break; }
				}
				p++;
			}
			n = macsurf__emit_one_track(out + pos, cap - pos,
					"1fr", 3);
			if (n < 0) break;
			pos += (size_t)n;
			room--;
			emitted++;
			continue;
		}

		/* Plain track token. */
		{
			const char *tok_start = p;
			size_t tok_len;
			int n;
			while (p < end && *p != ' ' && *p != '\t' &&
					*p != '\n' && *p != '\r' &&
					*p != ',' && *p != '(' && *p != ')') {
				p++;
			}
			tok_len = (size_t)(p - tok_start);
			if (tok_len == 0) {
				if (p < end) p++;
				continue;
			}
			n = macsurf__emit_one_track(out + pos, cap - pos,
					tok_start, tok_len);
			if (n < 0) break;
			pos += (size_t)n;
			room--;
			emitted++;
		}
	}

	if (emitted == 0) return 0;
	/* Trim the trailing space we appended after the last token. */
	if (pos > 0 && out[pos - 1] == ' ') pos--;
	return pos;
}


/* fixes115/117 — rewrite every `grid-template-columns: VALUE` declaration
 * inside the buffer to `-macsurf-grid: <tracks>` followed by enough
 * spaces to preserve the original byte count. The replacement is shorter
 * because the property name shrinks 22->14 chars; if track-list emission
 * fits we use it (fixes117), else fall back to the legacy column-count
 * rewrite (fixes115). The caller owns the returned malloc'd buffer and
 * must free it. Returns NULL on OOM. */
static char *
macsurf__rewrite_grid_template_columns(const char *data, size_t size)
{
	static const char NEEDLE[] = "grid-template-columns";
	static const size_t NEEDLE_LEN = 21;
	char *out;
	size_t i;

	out = (char *)malloc(size);
	if (out == NULL) return NULL;
	memcpy(out, data, size);

	i = 0;
	while (i + NEEDLE_LEN < size) {
		size_t j;
		size_t val_start;
		size_t val_end;
		size_t total_replace;
		size_t prefix_len;
		size_t tracks_room;
		size_t tracks_written;
		char buf[256];
		int paren_depth = 0;

		if (!macsurf__match_prop_name(out, size, i,
				NEEDLE, NEEDLE_LEN)) {
			i++;
			continue;
		}

		j = i + NEEDLE_LEN;
		while (j < size && (out[j] == ' ' || out[j] == '\t' ||
				out[j] == '\n' || out[j] == '\r')) j++;
		if (j >= size || out[j] != ':') {
			i++;
			continue;
		}

		val_start = j + 1;
		while (val_start < size && (out[val_start] == ' ' ||
				out[val_start] == '\t')) val_start++;

		val_end = val_start;
		while (val_end < size) {
			char c = out[val_end];
			if (c == '(') paren_depth++;
			else if (c == ')') {
				if (paren_depth > 0) paren_depth--;
			} else if (paren_depth == 0 &&
					(c == ';' || c == '}' || c == '!')) {
				break;
			}
			val_end++;
		}

		total_replace = val_end - i;
		prefix_len = (size_t)sprintf(buf, "-macsurf-grid: ");
		if (prefix_len >= total_replace) {
			/* Can't even fit the prefix -- leave the source
			 * declaration untouched. libcss will error on the
			 * unknown property and skip it gracefully. */
			i = val_end;
			continue;
		}

		tracks_room = total_replace - prefix_len;
		if (tracks_room > sizeof(buf) - prefix_len - 1) {
			tracks_room = sizeof(buf) - prefix_len - 1;
		}

		tracks_written = macsurf__emit_grid_tracks(
				out + val_start, out + val_end,
				buf + prefix_len, tracks_room);

		if (tracks_written == 0 ||
				prefix_len + tracks_written > total_replace) {
			/* Fall back to legacy count-only rewrite. */
			int columns = macsurf__count_grid_columns_text(
					out + val_start, out + val_end);
			int n = sprintf(buf, "-macsurf-grid: %d", columns);
			if (n > 0 && (size_t)n <= total_replace) {
				memcpy(out + i, buf, (size_t)n);
				memset(out + i + n, ' ',
						total_replace - (size_t)n);
			}
		} else {
			size_t total = prefix_len + tracks_written;
			memcpy(out + i, buf, total);
			memset(out + i + total, ' ', total_replace - total);
		}
		i = val_end;
	}

	return out;
}


/* fixes150 — rewrite every `grid-template-rows: VALUE` declaration to
 * `-macsurf-grid-rows: <tracks>` in place. Both names are 18 chars so
 * the prefix substitution is byte-for-byte; only the value is reformed
 * via macsurf__emit_grid_tracks (same flat-tokeniser as columns, with
 * the fixes148b3 minmax/fit-content/calc collapse). The caller owns the
 * returned malloc'd buffer and must free it. Returns NULL on OOM. */
static char *
macsurf__rewrite_grid_template_rows(const char *data, size_t size)
{
	static const char NEEDLE[] = "grid-template-rows";
	static const size_t NEEDLE_LEN = 18;
	char *out;
	size_t i;

	out = (char *)malloc(size);
	if (out == NULL) return NULL;
	memcpy(out, data, size);

	i = 0;
	while (i + NEEDLE_LEN < size) {
		size_t j;
		size_t val_start;
		size_t val_end;
		size_t total_replace;
		size_t prefix_len;
		size_t tracks_room;
		size_t tracks_written;
		char buf[256];
		int paren_depth = 0;

		if (!macsurf__match_prop_name(out, size, i,
				NEEDLE, NEEDLE_LEN)) {
			i++;
			continue;
		}

		j = i + NEEDLE_LEN;
		while (j < size && (out[j] == ' ' || out[j] == '\t' ||
				out[j] == '\n' || out[j] == '\r')) j++;
		if (j >= size || out[j] != ':') {
			i++;
			continue;
		}

		val_start = j + 1;
		while (val_start < size && (out[val_start] == ' ' ||
				out[val_start] == '\t')) val_start++;

		val_end = val_start;
		while (val_end < size) {
			char c = out[val_end];
			if (c == '(') paren_depth++;
			else if (c == ')') {
				if (paren_depth > 0) paren_depth--;
			} else if (paren_depth == 0 &&
					(c == ';' || c == '}' || c == '!')) {
				break;
			}
			val_end++;
		}

		total_replace = val_end - i;
		prefix_len = (size_t)sprintf(buf, "-macsurf-grid-rows: ");
		if (prefix_len >= total_replace) {
			i = val_end;
			continue;
		}

		tracks_room = total_replace - prefix_len;
		if (tracks_room > sizeof(buf) - prefix_len - 1) {
			tracks_room = sizeof(buf) - prefix_len - 1;
		}

		tracks_written = macsurf__emit_grid_tracks(
				out + val_start, out + val_end,
				buf + prefix_len, tracks_room);

		if (tracks_written == 0 ||
				prefix_len + tracks_written > total_replace) {
			/* No usable tracks -- leave the original declaration
			 * in place. libcss will reject the unknown
			 * grid-template-rows property and skip it. */
			i = val_end;
			continue;
		}

		{
			size_t total = prefix_len + tracks_written;
			memcpy(out + i, buf, total);
			memset(out + i + total, ' ', total_replace - total);
		}
		i = val_end;
	}

	return out;
}


/* fixes158 — grid placement rewriter.
 *
 * Replaces the fixes151 column-only rewriter. Walks `{ ... }` blocks
 * looking for any of:
 *   grid-column        : A | A/B | A/-1 | span N | A/span N
 *   grid-row           : same forms
 *   grid-column-start  : positive integer
 *   grid-column-end    : positive integer (or -1)
 *   grid-row-start     : positive integer
 *   grid-row-end       : positive integer (or -1)
 *
 * All recognised declarations within a single brace-depth-1 block are
 * combined into one packed `-macsurf-grid-col-span: <int32>` emitted at
 * the closing `}`. The int packs four 8-bit fields:
 *   bits  0..7  col_span    1..254 / 255 = fill
 *   bits  8..15 col_start   1..254 = explicit line, 0 = auto
 *   bits 16..23 row_start   same
 *   bits 24..31 row_span    same as col_span
 *
 * Original declarations are stripped from the output. Unrecognised
 * value forms (named lines, negative starts, calc(), keywords) leave
 * that specific declaration in the output untouched and don't update
 * the accumulator — libcss will then drop them as unknown properties.
 *
 * V1 limitations (documented, not bugs):
 *   - only positive integer line numbers are recognised in the
 *     longhand `-end` declarations; we don't carry around the matching
 *     `-start` to do A/B math across non-contiguous declarations
 *   - `grid-column-end: -1` is honoured as the "fill" sentinel only
 *     if `grid-column-start` has been seen earlier in the same block
 *   - `grid-area` is NOT parsed
 *   - named grid lines, calc(), and `auto` are deferred to V2
 *
 * Brace tracking uses a small stack so nested rules (e.g. inside
 * @media) accumulate per-innermost-block. Max nesting depth 8 is
 * enough for any real-world CSS.
 */

#define MACSURF_GRID_PLACEMENT_MAX_DEPTH 8

struct macsurf_grid_placement {
	bool any;
	uint8_t col_start;
	uint8_t col_span;
	uint8_t row_start;
	uint8_t row_span;
};

/* Parse a positive integer at *p, return value (1..254 clamped) or
 * 0 if no digits. Advance *p past the digits. Returns 255 if the
 * value is -1 (the "fill" sentinel). */
static int
macsurf__grid_parse_int_or_minus1(const char **p, const char *end)
{
	const char *q = *p;
	int n = 0;

	while (q < end && (*q == ' ' || *q == '\t')) q++;
	if (q < end && *q == '-') {
		q++;
		if (q < end && *q == '1') {
			q++;
			*p = q;
			return 255; /* `-1` sentinel = fill */
		}
		return 0; /* anything else negative is unsupported */
	}
	while (q < end && *q >= '0' && *q <= '9') {
		n = n * 10 + (*q - '0');
		q++;
	}
	*p = q;
	if (n < 1) return 0;
	if (n > 254) n = 254;
	return n;
}

/* Try to parse "span N" at *p. Returns N (1..254) on success and
 * advances *p; returns 0 on failure leaving *p unchanged. */
static int
macsurf__grid_parse_span(const char **p, const char *end)
{
	const char *q = *p;

	while (q < end && (*q == ' ' || *q == '\t')) q++;
	if ((end - q) < 4) return 0;
	if (!((q[0] == 's' || q[0] == 'S') &&
			(q[1] == 'p' || q[1] == 'P') &&
			(q[2] == 'a' || q[2] == 'A') &&
			(q[3] == 'n' || q[3] == 'N'))) {
		return 0;
	}
	if ((end - q) > 4) {
		char c = q[4];
		if (c != ' ' && c != '\t') return 0;
	} else {
		return 0;
	}
	q += 4;
	while (q < end && (*q == ' ' || *q == '\t')) q++;
	{
		int n = 0;
		while (q < end && *q >= '0' && *q <= '9') {
			n = n * 10 + (*q - '0');
			q++;
		}
		if (n < 1) return 0;
		if (n > 254) n = 254;
		*p = q;
		return n;
	}
}

/* Parse a `grid-column` / `grid-row` value. Writes into *start_out
 * and *span_out (0 = leave alone). Returns true if anything was
 * parsed. */
static bool
macsurf__grid_parse_axis_value(const char *p, const char *end,
		uint8_t *start_out, uint8_t *span_out)
{
	int span;
	int a;
	const char *cursor = p;

	*start_out = 0;
	*span_out = 0;

	while (cursor < end && (*cursor == ' ' || *cursor == '\t'))
		cursor++;
	if (cursor >= end) return false;

	/* span N (alone) */
	span = macsurf__grid_parse_span(&cursor, end);
	if (span > 0) {
		*span_out = (uint8_t)span;
		return true;
	}

	/* leading positive integer */
	if (*cursor >= '0' && *cursor <= '9') {
		a = macsurf__grid_parse_int_or_minus1(&cursor, end);
		if (a < 1 || a == 255) return false; /* lone -1 unsupported */
		*start_out = (uint8_t)a;
		while (cursor < end && (*cursor == ' ' || *cursor == '\t'))
			cursor++;
		if (cursor >= end || *cursor != '/') {
			/* Just "A" — start only, span 1 implicit. */
			*span_out = 1;
			return true;
		}
		cursor++;
		while (cursor < end && (*cursor == ' ' || *cursor == '\t'))
			cursor++;
		/* `A / span N` */
		span = macsurf__grid_parse_span(&cursor, end);
		if (span > 0) {
			*span_out = (uint8_t)span;
			return true;
		}
		/* `A / B` or `A / -1` */
		{
			int b = macsurf__grid_parse_int_or_minus1(&cursor,
					end);
			if (b == 255) {
				*span_out = 255; /* fill row */
				return true;
			}
			if (b > a) {
				int s = b - a;
				if (s > 254) s = 254;
				*span_out = (uint8_t)s;
				return true;
			}
			/* B <= A or 0 — unsupported, drop */
			*start_out = 0;
			return false;
		}
	}

	return false;
}

/* Match a property name at buf[pos..] case-insensitively, with a
 * boundary check after (must be followed by `:` or whitespace). */
static bool
macsurf__grid_match_name(const char *buf, size_t len, size_t pos,
		const char *name, size_t name_len)
{
	size_t i;
	char next;
	if (!macsurf__match_prop_name(buf, len, pos, name, name_len))
		return false;
	i = pos + name_len;
	if (i >= len) return false;
	next = buf[i];
	if (next == ':' || next == ' ' || next == '\t' || next == '\n' ||
			next == '\r')
		return true;
	return false;
}

/* Emit a packed `-macsurf-grid-col-span: <col_span> <col_start>
 * <row_start> <row_span>;` declaration into out at pos. Grows the
 * buffer as needed.
 *
 * Four space-separated unsigned integers (each 0..255) — avoids the
 * css_fixed Q22.10 range limit a single packed int32 would hit.
 * Leading `;` guards against the previous declaration lacking its
 * own terminator (legal in CSS for the last decl before `}`). */
static bool
macsurf__grid_emit_packed(char **out_p, size_t *cap_p, size_t *pos_p,
		const struct macsurf_grid_placement *acc)
{
	char tmp[64];
	int n;
	size_t need;

	n = sprintf(tmp, "; -macsurf-grid-col-span: %u %u %u %u;",
			(unsigned)acc->col_span,
			(unsigned)acc->col_start,
			(unsigned)acc->row_start,
			(unsigned)acc->row_span);
	if (n <= 0) return false;

	need = *pos_p + (size_t)n + 1;
	while (need >= *cap_p) {
		size_t newcap = *cap_p * 2;
		char *neu = (char *)realloc(*out_p, newcap);
		if (neu == NULL) return false;
		*out_p = neu;
		*cap_p = newcap;
	}
	memcpy(*out_p + *pos_p, tmp, (size_t)n);
	*pos_p += (size_t)n;
	return true;
}


/* Stub of the legacy fixes151 entry point — preserved so any direct
 * callers in older code paths still compile, but it now just calls
 * the unified placement rewriter below. */
static char *
macsurf__rewrite_grid_placement(const char *data, size_t in_size,
		size_t *out_size_p);

static char *
macsurf__rewrite_grid_column(const char *data, size_t in_size,
		size_t *out_size_p)
{
	return macsurf__rewrite_grid_placement(data, in_size, out_size_p);
}


/* Old single-property column rewriter, retained inline-disabled for
 * reference. The new unified rewriter is below. */
#if 0
static char *
macsurf__rewrite_grid_column_legacy(const char *data, size_t in_size,
		size_t *out_size_p)
{
	static const char NEEDLE[] = "grid-column";
	static const size_t NEEDLE_LEN = 11;
	size_t cap;
	size_t pos = 0;
	char *out;
	size_t i = 0;

	cap = in_size + 256;
	if (cap < in_size * 2) cap = in_size * 2;
	if (cap < 1024) cap = 1024;
	out = (char *)malloc(cap);
	if (out == NULL) return NULL;

	while (i < in_size) {
		size_t j;
		size_t val_start;
		size_t val_end;
		int span = 0;
		bool got_span = false;
		const char *p;
		const char *end;

		if (!macsurf__match_prop_name(data, in_size, i,
				NEEDLE, NEEDLE_LEN)) {
			if (pos >= cap - 1) {
				size_t newcap = cap * 2;
				char *neu = (char *)realloc(out, newcap);
				if (neu == NULL) { free(out); return NULL; }
				out = neu;
				cap = newcap;
			}
			out[pos++] = data[i++];
			continue;
		}

		j = i + NEEDLE_LEN;
		while (j < in_size && (data[j] == ' ' || data[j] == '\t' ||
				data[j] == '\n' || data[j] == '\r')) j++;
		if (j >= in_size || data[j] != ':') {
			if (pos >= cap - 1) {
				size_t newcap = cap * 2;
				char *neu = (char *)realloc(out, newcap);
				if (neu == NULL) { free(out); return NULL; }
				out = neu;
				cap = newcap;
			}
			out[pos++] = data[i++];
			continue;
		}

		val_start = j + 1;
		while (val_start < in_size && (data[val_start] == ' ' ||
				data[val_start] == '\t')) val_start++;

		val_end = val_start;
		while (val_end < in_size) {
			char c = data[val_end];
			if (c == ';' || c == '}' || c == '!') break;
			val_end++;
		}

		/* Parse the value range. */
		p = data + val_start;
		end = data + val_end;

		/* "span N" form. */
		if ((end - p) >= 4 &&
				(p[0] == 's' || p[0] == 'S') &&
				(p[1] == 'p' || p[1] == 'P') &&
				(p[2] == 'a' || p[2] == 'A') &&
				(p[3] == 'n' || p[3] == 'N') &&
				(p[4] == ' ' || p[4] == '\t')) {
			const char *q = p + 4;
			int n = 0;
			while (q < end && (*q == ' ' || *q == '\t')) q++;
			while (q < end && *q >= '0' && *q <= '9') {
				n = n * 10 + (*q - '0');
				q++;
			}
			if (n > 0) {
				span = (n > 255) ? 255 : n;
				got_span = true;
			}
		} else if (*p >= '0' && *p <= '9') {
			/* "A / B" or "A / span N" or "A / -1" form. */
			int a = 0;
			int b = 0;
			bool b_neg = false;
			const char *q = p;
			while (q < end && *q >= '0' && *q <= '9') {
				a = a * 10 + (*q - '0');
				q++;
			}
			while (q < end && (*q == ' ' || *q == '\t')) q++;
			if (q < end && *q == '/') {
				q++;
				while (q < end && (*q == ' ' || *q == '\t')) q++;
				if (q < end && *q == '-') {
					b_neg = true;
					q++;
				}
				if (q < end && (*q == 's' || *q == 'S')) {
					/* "span N" tail. */
					const char *r = q;
					if ((end - r) >= 4 &&
							(r[1] == 'p' || r[1] == 'P') &&
							(r[2] == 'a' || r[2] == 'A') &&
							(r[3] == 'n' || r[3] == 'N')) {
						r += 4;
						while (r < end &&
								(*r == ' ' || *r == '\t'))
							r++;
						while (r < end && *r >= '0' &&
								*r <= '9') {
							b = b * 10 + (*r - '0');
							r++;
						}
						if (b > 0) {
							span = (b > 255) ? 255 : b;
							got_span = true;
						}
					}
				} else {
					while (q < end && *q >= '0' && *q <= '9') {
						b = b * 10 + (*q - '0');
						q++;
					}
					if (b_neg) {
						/* `A / -1` style — fill to end. */
						span = 255;
						got_span = true;
					} else if (b > a) {
						int n = b - a;
						span = (n > 255) ? 255 : n;
						got_span = true;
					}
				}
			}
		}

		if (got_span) {
			/* Emit "-macsurf-grid-col-span: N;" (~28 bytes max
			 * for N<=255). Skip the original declaration up to
			 * and including the terminator. */
			char tmp[64];
			int n = sprintf(tmp,
					"-macsurf-grid-col-span: %d", span);
			if (n > 0) {
				size_t need = pos + (size_t)n + 16;
				while (need >= cap) {
					size_t newcap = cap * 2;
					char *neu = (char *)realloc(out,
							newcap);
					if (neu == NULL) {
						free(out);
						return NULL;
					}
					out = neu;
					cap = newcap;
				}
				memcpy(out + pos, tmp, (size_t)n);
				pos += (size_t)n;
				i = val_end;
				continue;
			}
		}

		/* Couldn't parse -- pass through unchanged. */
		{
			size_t span_bytes = val_end - i;
			while (pos + span_bytes >= cap) {
				size_t newcap = cap * 2;
				char *neu = (char *)realloc(out, newcap);
				if (neu == NULL) {
					free(out);
					return NULL;
				}
				out = neu;
				cap = newcap;
			}
			memcpy(out + pos, data + i, span_bytes);
			pos += span_bytes;
			i = val_end;
		}
	}

	*out_size_p = pos;
	return out;
}
#endif  /* legacy column rewriter */


/* fixes158 unified grid-placement rewriter. Walks brace-tracked
 * declarations and rolls all six grid placement properties into a
 * single packed `-macsurf-grid-col-span: <int32>` declaration at the
 * close of each innermost block. */
static char *
macsurf__rewrite_grid_placement(const char *data, size_t in_size,
		size_t *out_size_p)
{
	struct macsurf_grid_placement stack[MACSURF_GRID_PLACEMENT_MAX_DEPTH];
	int depth = 0;
	size_t cap;
	size_t pos = 0;
	char *out;
	size_t i = 0;
	int k;

	for (k = 0; k < MACSURF_GRID_PLACEMENT_MAX_DEPTH; k++) {
		stack[k].any = false;
		stack[k].col_start = 0;
		stack[k].col_span = 0;
		stack[k].row_start = 0;
		stack[k].row_span = 0;
	}

	cap = in_size + 256;
	if (cap < in_size * 2) cap = in_size * 2;
	if (cap < 1024) cap = 1024;
	out = (char *)malloc(cap);
	if (out == NULL) return NULL;

	while (i < in_size) {
		char c = data[i];

		/* Attempt to match a placement declaration if we're inside
		 * a block and the byte could start one. */
		if (depth >= 1 && depth <= MACSURF_GRID_PLACEMENT_MAX_DEPTH &&
				(c == 'g' || c == 'G')) {
			static const char N_GC[]   = "grid-column";
			static const char N_GR[]   = "grid-row";
			static const char N_GCS[]  = "grid-column-start";
			static const char N_GCE[]  = "grid-column-end";
			static const char N_GRS[]  = "grid-row-start";
			static const char N_GRE[]  = "grid-row-end";
			size_t name_len = 0;
			int prop = 0; /* 1=GC 2=GR 3=GCS 4=GCE 5=GRS 6=GRE */

			/* Check longer names first to avoid GC matching
			 * the prefix of GCS/GCE. */
			if (macsurf__grid_match_name(data, in_size, i,
					N_GCS, sizeof(N_GCS) - 1)) {
				prop = 3; name_len = sizeof(N_GCS) - 1;
			} else if (macsurf__grid_match_name(data, in_size, i,
					N_GCE, sizeof(N_GCE) - 1)) {
				prop = 4; name_len = sizeof(N_GCE) - 1;
			} else if (macsurf__grid_match_name(data, in_size, i,
					N_GRS, sizeof(N_GRS) - 1)) {
				prop = 5; name_len = sizeof(N_GRS) - 1;
			} else if (macsurf__grid_match_name(data, in_size, i,
					N_GRE, sizeof(N_GRE) - 1)) {
				prop = 6; name_len = sizeof(N_GRE) - 1;
			} else if (macsurf__grid_match_name(data, in_size, i,
					N_GC, sizeof(N_GC) - 1)) {
				prop = 1; name_len = sizeof(N_GC) - 1;
			} else if (macsurf__grid_match_name(data, in_size, i,
					N_GR, sizeof(N_GR) - 1)) {
				prop = 2; name_len = sizeof(N_GR) - 1;
			}

			if (prop != 0) {
				size_t j = i + name_len;
				size_t val_start;
				size_t val_end;
				const char *p;
				const char *end;
				struct macsurf_grid_placement *acc =
						&stack[depth - 1];
				uint8_t s = 0;
				uint8_t sp = 0;
				int v;

				while (j < in_size && (data[j] == ' ' ||
						data[j] == '\t' ||
						data[j] == '\n' ||
						data[j] == '\r')) j++;
				if (j >= in_size || data[j] != ':') {
					/* Not actually a declaration. */
					goto not_grid;
				}
				val_start = j + 1;
				while (val_start < in_size &&
						(data[val_start] == ' ' ||
						data[val_start] == '\t'))
					val_start++;
				val_end = val_start;
				while (val_end < in_size) {
					char vc = data[val_end];
					if (vc == ';' || vc == '}' ||
							vc == '!') break;
					val_end++;
				}

				p = data + val_start;
				end = data + val_end;

				if (prop == 1) {
					/* grid-column */
					if (macsurf__grid_parse_axis_value(
							p, end, &s, &sp)) {
						if (s != 0) acc->col_start = s;
						if (sp != 0) acc->col_span = sp;
						acc->any = true;
					}
				} else if (prop == 2) {
					/* grid-row */
					if (macsurf__grid_parse_axis_value(
							p, end, &s, &sp)) {
						if (s != 0) acc->row_start = s;
						if (sp != 0) acc->row_span = sp;
						acc->any = true;
					}
				} else if (prop == 3) {
					/* grid-column-start */
					v = macsurf__grid_parse_int_or_minus1(
							&p, end);
					if (v >= 1 && v <= 254) {
						acc->col_start = (uint8_t)v;
						acc->any = true;
					}
				} else if (prop == 4) {
					/* grid-column-end */
					v = macsurf__grid_parse_int_or_minus1(
							&p, end);
					if (v == 255 && acc->col_start > 0) {
						acc->col_span = 255;
						acc->any = true;
					} else if (v >= 1 && v <= 254 &&
							acc->col_start > 0 &&
							v > acc->col_start) {
						int sp2 = v - acc->col_start;
						if (sp2 > 254) sp2 = 254;
						acc->col_span = (uint8_t)sp2;
						acc->any = true;
					}
				} else if (prop == 5) {
					/* grid-row-start */
					v = macsurf__grid_parse_int_or_minus1(
							&p, end);
					if (v >= 1 && v <= 254) {
						acc->row_start = (uint8_t)v;
						acc->any = true;
					}
				} else if (prop == 6) {
					/* grid-row-end */
					v = macsurf__grid_parse_int_or_minus1(
							&p, end);
					if (v == 255 && acc->row_start > 0) {
						acc->row_span = 255;
						acc->any = true;
					} else if (v >= 1 && v <= 254 &&
							acc->row_start > 0 &&
							v > acc->row_start) {
						int sp2 = v - acc->row_start;
						if (sp2 > 254) sp2 = 254;
						acc->row_span = (uint8_t)sp2;
						acc->any = true;
					}
				}

				/* Skip the declaration in the input (including
				 * the trailing `;` if present). The closing `}`
				 * stays in the input — we don't eat it. */
				i = val_end;
				if (i < in_size && data[i] == ';') i++;
				goto next_iter;
			}
not_grid:
			;
		}

		if (c == '{') {
			if (depth < MACSURF_GRID_PLACEMENT_MAX_DEPTH) {
				stack[depth].any = false;
				stack[depth].col_start = 0;
				stack[depth].col_span = 0;
				stack[depth].row_start = 0;
				stack[depth].row_span = 0;
			}
			depth++;
		} else if (c == '}') {
			/* Before emitting `}`, emit any accumulated
			 * placement for the innermost block. */
			if (depth >= 1 && depth <=
					MACSURF_GRID_PLACEMENT_MAX_DEPTH &&
					stack[depth - 1].any) {
				if (!macsurf__grid_emit_packed(&out, &cap, &pos,
						&stack[depth - 1])) {
					free(out);
					return NULL;
				}
				stack[depth - 1].any = false;
			}
			if (depth > 0) depth--;
		}

		/* Emit the current byte. */
		{
			size_t need = pos + 2;
			while (need >= cap) {
				size_t newcap = cap * 2;
				char *neu = (char *)realloc(out, newcap);
				if (neu == NULL) { free(out); return NULL; }
				out = neu;
				cap = newcap;
			}
		}
		out[pos++] = c;
		i++;

next_iter:
		;
	}

	*out_size_p = pos;
	return out;
}


/* fixes159: justify-items / justify-self rewriter.
 *
 * Walks `{ ... }` blocks. For each innermost block, recognises:
 *   justify-items: start | end | center | stretch
 *   justify-self:  start | end | center | stretch
 *
 * Accumulates the two values (4-bit each: 1=start, 2=end, 3=center,
 * 4=stretch) and emits ONE
 *   `-macsurf-justify: <justify_items> <justify_self>;`
 * declaration at the closing `}`. Original declarations are stripped.
 *
 * align-items / align-self are NOT touched here — they flow through
 * libcss's standard parser since they already exist for flexbox.
 *
 * V1 limitations (same shape as fixes158):
 *   - both axes must live in the same CSS rule (cascade collapses
 *     storage per-property)
 *   - unrecognised values (`flex-start`, `baseline`, `safe`, etc.) are
 *     passed through unchanged and dropped by libcss as unknowns
 */

#define MACSURF_JUSTIFY_MAX_DEPTH 8

struct macsurf_justify_accum {
	bool any;
	uint8_t justify_items;
	uint8_t justify_self;
};

/* Parse one of the four supported keyword values starting at *p.
 * Returns 1..4 on match, 0 on no match. Advances *p past the keyword. */
static int
macsurf__justify_parse_keyword(const char **p, const char *end)
{
	const char *q = *p;
	while (q < end && (*q == ' ' || *q == '\t')) q++;
	/* "start" */
	if ((end - q) >= 5 &&
			(q[0] == 's' || q[0] == 'S') &&
			(q[1] == 't' || q[1] == 'T') &&
			(q[2] == 'a' || q[2] == 'A') &&
			(q[3] == 'r' || q[3] == 'R') &&
			(q[4] == 't' || q[4] == 'T')) {
		char nc = ((end - q) > 5) ? q[5] : ' ';
		if (nc == ' ' || nc == '\t' || nc == ';' || nc == '}' ||
				nc == '!') {
			*p = q + 5;
			return 1;
		}
	}
	/* "end" */
	if ((end - q) >= 3 &&
			(q[0] == 'e' || q[0] == 'E') &&
			(q[1] == 'n' || q[1] == 'N') &&
			(q[2] == 'd' || q[2] == 'D')) {
		char nc = ((end - q) > 3) ? q[3] : ' ';
		if (nc == ' ' || nc == '\t' || nc == ';' || nc == '}' ||
				nc == '!') {
			*p = q + 3;
			return 2;
		}
	}
	/* "center" */
	if ((end - q) >= 6 &&
			(q[0] == 'c' || q[0] == 'C') &&
			(q[1] == 'e' || q[1] == 'E') &&
			(q[2] == 'n' || q[2] == 'N') &&
			(q[3] == 't' || q[3] == 'T') &&
			(q[4] == 'e' || q[4] == 'E') &&
			(q[5] == 'r' || q[5] == 'R')) {
		char nc = ((end - q) > 6) ? q[6] : ' ';
		if (nc == ' ' || nc == '\t' || nc == ';' || nc == '}' ||
				nc == '!') {
			*p = q + 6;
			return 3;
		}
	}
	/* "stretch" */
	if ((end - q) >= 7 &&
			(q[0] == 's' || q[0] == 'S') &&
			(q[1] == 't' || q[1] == 'T') &&
			(q[2] == 'r' || q[2] == 'R') &&
			(q[3] == 'e' || q[3] == 'E') &&
			(q[4] == 't' || q[4] == 'T') &&
			(q[5] == 'c' || q[5] == 'C') &&
			(q[6] == 'h' || q[6] == 'H')) {
		char nc = ((end - q) > 7) ? q[7] : ' ';
		if (nc == ' ' || nc == '\t' || nc == ';' || nc == '}' ||
				nc == '!') {
			*p = q + 7;
			return 4;
		}
	}
	return 0;
}

/* fixes159d: forward-decl for the bounded diagnostic logger. The
 * preprocessor lives outside the netsurf frontend module that owns the
 * implementation, so we declare the extern here. */
extern void macsurf_debug_log_writef(const char *fmt, ...);
/* fixes159h: explicit flush for the around-libcss probe — log path
 * skips per-write FlushVol (fixes96 perf), so without this the
 * trailing diagnostic lines vanish into HFS cache on the crash. */
extern void macsurf_debug_log_flush(void);

static bool
macsurf__justify_emit(char **out_p, size_t *cap_p, size_t *pos_p,
		const struct macsurf_justify_accum *acc)
{
	char tmp[64];
	int n;
	size_t need;
	static int macsurf__justify_emit_count = 0;

	n = sprintf(tmp, "; -macsurf-justify: %u %u;",
			(unsigned)acc->justify_items,
			(unsigned)acc->justify_self);
	if (macsurf__justify_emit_count < 16) {
		/* fixes159e: only %d/%ld/%s/%p in this logger — see
		 * project_macsurf_debug_log_specifiers memory. %u in 159d
		 * printed literally without consuming the arg, scrambling
		 * the trailing %s into a crash. */
		macsurf_debug_log_writef(
			"JEMIT[%d] ji=%d js=%d bytes=\"%s\"",
			macsurf__justify_emit_count,
			(int)acc->justify_items,
			(int)acc->justify_self,
			tmp);
		macsurf__justify_emit_count++;
	}
	if (n <= 0) return false;
	need = *pos_p + (size_t)n + 1;
	while (need >= *cap_p) {
		size_t newcap = *cap_p * 2;
		char *neu = (char *)realloc(*out_p, newcap);
		if (neu == NULL) return false;
		*out_p = neu;
		*cap_p = newcap;
	}
	memcpy(*out_p + *pos_p, tmp, (size_t)n);
	*pos_p += (size_t)n;
	return true;
}

static char *
macsurf__rewrite_justify(const char *data, size_t in_size,
		size_t *out_size_p)
{
	struct macsurf_justify_accum stack[MACSURF_JUSTIFY_MAX_DEPTH];
	int depth = 0;
	size_t cap;
	size_t pos = 0;
	char *out;
	size_t i = 0;
	int k;

	for (k = 0; k < MACSURF_JUSTIFY_MAX_DEPTH; k++) {
		stack[k].any = false;
		stack[k].justify_items = 0;
		stack[k].justify_self = 0;
	}

	cap = in_size + 256;
	if (cap < in_size * 2) cap = in_size * 2;
	if (cap < 1024) cap = 1024;
	out = (char *)malloc(cap);
	if (out == NULL) return NULL;

	while (i < in_size) {
		char c = data[i];

		/* Try to match a justify-* declaration at this point. */
		if (depth >= 1 && depth <= MACSURF_JUSTIFY_MAX_DEPTH &&
				(c == 'j' || c == 'J')) {
			static const char N_JI[] = "justify-items";
			static const char N_JS[] = "justify-self";
			size_t name_len = 0;
			int prop = 0; /* 1=items 2=self */

			/* Longer name first (no overlap here, but consistent
			 * with the fixes158 pattern). */
			if (macsurf__match_prop_name(data, in_size, i,
					N_JI, sizeof(N_JI) - 1)) {
				size_t j = i + sizeof(N_JI) - 1;
				if (j < in_size && (data[j] == ':' ||
						data[j] == ' ' ||
						data[j] == '\t' ||
						data[j] == '\n' ||
						data[j] == '\r')) {
					prop = 1; name_len = sizeof(N_JI) - 1;
				}
			}
			if (prop == 0 && macsurf__match_prop_name(data,
					in_size, i, N_JS, sizeof(N_JS) - 1)) {
				size_t j = i + sizeof(N_JS) - 1;
				if (j < in_size && (data[j] == ':' ||
						data[j] == ' ' ||
						data[j] == '\t' ||
						data[j] == '\n' ||
						data[j] == '\r')) {
					prop = 2; name_len = sizeof(N_JS) - 1;
				}
			}

			if (prop != 0) {
				size_t j = i + name_len;
				size_t val_start;
				size_t val_end;
				const char *p;
				const char *end;
				struct macsurf_justify_accum *acc =
						&stack[depth - 1];
				int v;

				while (j < in_size && (data[j] == ' ' ||
						data[j] == '\t' ||
						data[j] == '\n' ||
						data[j] == '\r')) j++;
				if (j >= in_size || data[j] != ':') {
					goto not_justify;
				}
				val_start = j + 1;
				while (val_start < in_size &&
						(data[val_start] == ' ' ||
						data[val_start] == '\t'))
					val_start++;
				val_end = val_start;
				while (val_end < in_size) {
					char vc = data[val_end];
					if (vc == ';' || vc == '}' ||
							vc == '!') break;
					val_end++;
				}

				p = data + val_start;
				end = data + val_end;
				v = macsurf__justify_parse_keyword(&p, end);
				if (v >= 1 && v <= 4) {
					if (prop == 1) acc->justify_items =
							(uint8_t)v;
					else           acc->justify_self  =
							(uint8_t)v;
					acc->any = true;
					i = val_end;
					if (i < in_size && data[i] == ';') i++;
					goto next_iter_j;
				}
				/* Unrecognised keyword — fall through and emit
				 * verbatim. libcss will drop it. */
			}
not_justify:
			;
		}

		if (c == '{') {
			if (depth < MACSURF_JUSTIFY_MAX_DEPTH) {
				stack[depth].any = false;
				stack[depth].justify_items = 0;
				stack[depth].justify_self = 0;
			}
			depth++;
		} else if (c == '}') {
			if (depth >= 1 && depth <=
					MACSURF_JUSTIFY_MAX_DEPTH &&
					stack[depth - 1].any) {
				if (!macsurf__justify_emit(&out, &cap, &pos,
						&stack[depth - 1])) {
					free(out);
					return NULL;
				}
				stack[depth - 1].any = false;
			}
			if (depth > 0) depth--;
		}

		{
			size_t need = pos + 2;
			while (need >= cap) {
				size_t newcap = cap * 2;
				char *neu = (char *)realloc(out, newcap);
				if (neu == NULL) { free(out); return NULL; }
				out = neu;
				cap = newcap;
			}
		}
		out[pos++] = c;
		i++;
next_iter_j:
		;
	}

	*out_size_p = pos;
	return out;
}


static bool
nscss_process_data(struct content *c, const char *data, unsigned int size)
{
	nscss_content *css = (nscss_content *) c;
	css_error error;
	char *rewritten;
	char *rewritten_rows;
	char *rewritten_col_span = NULL;
	char *rewritten_justify = NULL;
	size_t col_span_size = 0;
	size_t justify_size = 0;
	const char *final_data;
	unsigned int final_size;

	MS_LOG("nscss process data");

	/* fixes115 — pre-process the CSS bytes to convert
	 * `grid-template-columns: ...` declarations to `-macsurf-grid: N`
	 * so the existing -macsurf-grid select/layout path picks up
	 * standard CSS Grid track lists from real-world stylesheets.
	 * Mactrove and most modern Drupal themes lean on
	 * grid-template-columns for their main 2-column sidebar layout;
	 * without this they collapse to a single column. fixes112 tried
	 * to do this via a new libcss property entry and destabilised
	 * the cascade on CW8; rewriting the source text before libcss
	 * sees it avoids touching libcss internals entirely. */
	rewritten = macsurf__rewrite_grid_template_columns(data, (size_t)size);
	rewritten_rows = NULL;
	if (rewritten != NULL) {
		/* fixes150 — second pass for grid-template-rows. Same
		 * in-place rewrite scheme; runs against the columns-rewritten
		 * buffer so both passes see the same source layout. */
		rewritten_rows = macsurf__rewrite_grid_template_rows(
				rewritten, (size_t)size);
	}

	/* Pick the buffer for the final pass: rows-rewritten if available,
	 * else columns-rewritten, else original. All previous passes are
	 * in-place rewrites of the same size, so size stays the same here. */
	final_data = rewritten_rows ? (const char *)rewritten_rows :
			rewritten ? (const char *)rewritten : data;
	final_size = size;

	/* fixes151 — third pass for `grid-column: VALUE`. This pass needs
	 * a growable output buffer (new property name is longer than the
	 * source), so it allocates fresh and returns the new size. */
	rewritten_col_span = macsurf__rewrite_grid_column(final_data,
			(size_t)final_size, &col_span_size);
	if (rewritten_col_span != NULL && col_span_size <= (size_t)0x7fffffff) {
		final_data = (const char *)rewritten_col_span;
		final_size = (unsigned int)col_span_size;
	}

	/* fixes159 — fourth pass for `justify-items` / `justify-self`.
	 * Same allocate-fresh shape as the col-span pass. */
	rewritten_justify = macsurf__rewrite_justify(final_data,
			(size_t)final_size, &justify_size);
	if (rewritten_justify != NULL && justify_size <= (size_t)0x7fffffff) {
		final_data = (const char *)rewritten_justify;
		final_size = (unsigned int)justify_size;
	}

	/* fixes159h: triangulate the 159e/f/g crash. Three labelled,
	 * force-flushed log points around the libcss handoff so we can
	 * tell exactly where execution dies between preprocessor-done
	 * and parser-called. Decision matrix in the ship message. */
	{
		static int macsurf__rw_probe_count = 0;
		if (macsurf__rw_probe_count < 4) {
			macsurf_debug_log_writef(
				"POSTRW[%d] final_size=%d about to enter libcss",
				macsurf__rw_probe_count,
				(int)final_size);
			macsurf_debug_log_flush();
		}

		error = nscss_process_css_data(&css->data,
				final_data, final_size);

		if (macsurf__rw_probe_count < 4) {
			macsurf_debug_log_writef(
				"POSTPARSE[%d] libcss returned err=%d",
				macsurf__rw_probe_count,
				(int)error);
			macsurf_debug_log_flush();
		}

		if (rewritten_justify != NULL) free(rewritten_justify);
		if (rewritten_col_span != NULL) free(rewritten_col_span);
		if (rewritten_rows != NULL) free(rewritten_rows);
		if (rewritten != NULL) free(rewritten);

		if (macsurf__rw_probe_count < 4) {
			macsurf_debug_log_writef(
				"POSTFREE[%d] all rewrite buffers freed",
				macsurf__rw_probe_count);
			macsurf_debug_log_flush();
			macsurf__rw_probe_count++;
		}
	}

	if (error != CSS_OK && error != CSS_NEEDDATA) {
		content_broadcast_error(c, NSERROR_CSS, NULL);
	}

	return (error == CSS_OK || error == CSS_NEEDDATA);
}

/**
 * Process CSS data
 *
 * \param c     CSS content object
 * \param data  Data to process
 * \param size  Number of bytes to process
 * \return CSS_OK on success, appropriate error otherwise
 */
static css_error nscss_process_css_data(struct content_css_data *c,
		const char *data, unsigned int size)
{
	return css_stylesheet_append_data(c->sheet,
			(const uint8_t *) data, size);
}

/**
 * Convert a CSS content ready for use
 *
 * \param c  Content to convert
 * \return true on success, false on failure
 */
bool nscss_convert(struct content *c)
{
	nscss_content *css = (nscss_content *) c;
	css_error error;

	MS_LOG("nscss convert");

	error = nscss_convert_css_data(&css->data);
	if (error != CSS_OK) {
		content_broadcast_error(c, NSERROR_CSS, NULL);
		return false;
	}

	return true;
}

/**
 * Convert CSS data ready for use
 *
 * \param c  CSS data to convert
 * \return CSS error
 */
static css_error nscss_convert_css_data(struct content_css_data *c)
{
	css_error error;

	error = css_stylesheet_data_done(c->sheet);

	/* Process pending imports */
	if (error == CSS_IMPORTS_PENDING) {
		/* We must not have registered any imports yet */
		assert(c->next_to_register == (uint32_t) -1);

		/* Start registering, until we find one that
		 * hasn't finished fetching */
		c->next_to_register = 0;
		error = nscss_register_imports(c);
	} else if (error == CSS_OK) {
		/* No imports, and no errors, so complete conversion */
		c->done(c, c->pw);
	} else {
		const char *url;

		if (css_stylesheet_get_url(c->sheet, &url) == CSS_OK) {
			NSLOG(netsurf, INFO, "Failed converting %p %s (%d)",
			      c, url, error);
		} else {
			NSLOG(netsurf, INFO, "Failed converting %p (%d)", c,
			      error);
		}
	}

	return error;
}

/**
 * Clean up a CSS content
 *
 * \param c  Content to clean up
 */
void nscss_destroy(struct content *c)
{
	nscss_content *css = (nscss_content *) c;

	nscss_destroy_css_data(&css->data);
}

/**
 * Clean up CSS data
 *
 * \param c  CSS data to clean up
 */
static void nscss_destroy_css_data(struct content_css_data *c)
{
	uint32_t i;

	for (i = 0; i < c->import_count; i++) {
		if (c->imports[i].c != NULL) {
			hlcache_handle_release(c->imports[i].c);
		}
		c->imports[i].c = NULL;
	}

	free(c->imports);

	if (c->sheet != NULL) {
		css_stylesheet_destroy(c->sheet);
		c->sheet = NULL;
	}

	free(c->charset);
}

nserror nscss_clone(const struct content *old, struct content **newc)
{
	const nscss_content *old_css = (const nscss_content *) old;
	nscss_content *new_css;
	const uint8_t *data;
	size_t size;
	nserror error;

	new_css = calloc(1, sizeof(nscss_content));
	if (new_css == NULL)
		return NSERROR_NOMEM;

	/* Clone content */
	error = content__clone(old, &new_css->base);
	if (error != NSERROR_OK) {
		content_destroy(&new_css->base);
		return error;
	}

	/* Simply replay create/process/convert */
	error = nscss_create_css_data(&new_css->data,
			nsurl_access(content_get_url(&new_css->base)),
			old_css->data.charset,
			new_css->base.quirks,
			nscss_content_done, new_css);
	if (error != NSERROR_OK) {
		content_destroy(&new_css->base);
		return error;
	}

	data = content__get_source_data(&new_css->base, &size);
	if (size > 0) {
		if (nscss_process_data(&new_css->base,
				       (char *)data,
				       (unsigned int)size) == false) {
			content_destroy(&new_css->base);
			return NSERROR_CLONE_FAILED;
		}
	}

	if (old->status == CONTENT_STATUS_READY ||
			old->status == CONTENT_STATUS_DONE) {
		if (nscss_convert(&new_css->base) == false) {
			content_destroy(&new_css->base);
			return NSERROR_CLONE_FAILED;
		}
	}

	*newc = (struct content *) new_css;

	return NSERROR_OK;
}

bool nscss_matches_quirks(const struct content *c, bool quirks)
{
	return c->quirks == quirks;
}

/* exported interface documented in netsurf/css.h */
css_stylesheet *nscss_get_stylesheet(struct hlcache_handle *h)
{
	nscss_content *c = (nscss_content *) hlcache_handle_get_content(h);

	assert(c != NULL);

	return c->data.sheet;
}

/* exported interface documented in netsurf/css.h */
struct nscss_import *nscss_get_imports(hlcache_handle *h, uint32_t *n)
{
	nscss_content *c = (nscss_content *) hlcache_handle_get_content(h);

	assert(c != NULL);
	assert(n != NULL);

	*n = c->data.import_count;

	return c->data.imports;
}

/**
 * Compute the type of a content
 *
 * \return CONTENT_CSS
 */
content_type nscss_content_type(void)
{
	return CONTENT_CSS;
}

/*****************************************************************************
 * Object completion                                                         *
 *****************************************************************************/

/**
 * Handle notification that a CSS object is done
 *
 * \param css  CSS object
 * \param pw   Private data
 */
void nscss_content_done(struct content_css_data *css, void *pw)
{
	struct content *c = pw;
	uint32_t i;
	size_t size;
	css_error error;

	/* Retrieve the size of this sheet */
	error = css_stylesheet_size(css->sheet, &size);
	if (error != CSS_OK) {
		content_broadcast_error(c, NSERROR_CSS, NULL);
		content_set_error(c);
		return;
	}
	c->size += size;

	/* Add on the size of the imported sheets */
	for (i = 0; i < css->import_count; i++) {
		if (css->imports[i].c != NULL) {
			struct content *import = hlcache_handle_get_content(
					css->imports[i].c);

			if (import != NULL) {
				c->size += import->size;
			}
		}
	}

	/* Finally, catch the content's users up with reality */
	content_set_ready(c);
	MS_LOG("nscss firing CONTENT_MSG_DONE");
	content_set_done(c);
}

/*****************************************************************************
 * Import handling                                                           *
 *****************************************************************************/

/**
 * Handle notification of the need for an imported stylesheet
 *
 * \param pw      CSS object requesting the import
 * \param parent  Stylesheet requesting the import
 * \param url     URL of the imported sheet
 * \return CSS_OK on success, appropriate error otherwise
 */
css_error nscss_handle_import(void *pw, css_stylesheet *parent,
		lwc_string *url)
{
	content_type accept = CONTENT_CSS;
	struct content_css_data *c = pw;
	nscss_import_ctx *ctx;
	hlcache_child_context child;
	struct nscss_import *imports;
	const char *referer;
	css_error error;
	nserror nerror;

	nsurl *ns_url;
	nsurl *ns_ref;

	assert(parent == c->sheet);

	error = css_stylesheet_get_url(c->sheet, &referer);
	if (error != CSS_OK) {
		return error;
	}

	ctx = malloc(sizeof(*ctx));
	if (ctx == NULL)
		return CSS_NOMEM;

	ctx->css = c;
	ctx->index = c->import_count;

	/* Increase space in table */
	imports = realloc(c->imports, (c->import_count + 1) *
			sizeof(struct nscss_import));
	if (imports == NULL) {
		free(ctx);
		return CSS_NOMEM;
	}
	c->imports = imports;

	/** \todo fallback charset */
	child.charset = NULL;
	error = css_stylesheet_quirks_allowed(c->sheet, &child.quirks);
	if (error != CSS_OK) {
		free(ctx);
		return error;
	}

	/* Create content */

	/** \todo Why aren't we getting a relative url part, to join? */
	nerror = nsurl_create(lwc_string_data(url), &ns_url);
	if (nerror != NSERROR_OK) {
		free(ctx);
		return CSS_NOMEM;
	}

	/** \todo Constructing nsurl for referer here is silly, avoid */
	nerror = nsurl_create(referer, &ns_ref);
	if (nerror != NSERROR_OK) {
		nsurl_unref(ns_url);
		free(ctx);
		return CSS_NOMEM;
	}

	/* Avoid importing ourself */
	if (nsurl_compare(ns_url, ns_ref, NSURL_COMPLETE)) {
		c->imports[c->import_count].c = NULL;
		/* No longer require context as we're not fetching anything */
		free(ctx);
		ctx = NULL;
	} else {
		nerror = hlcache_handle_retrieve(ns_url,
				0, ns_ref, NULL, nscss_import, ctx,
				&child, accept,
				&c->imports[c->import_count].c);
		if (nerror != NSERROR_OK) {
			free(ctx);
			return CSS_NOMEM;
		}
	}

	nsurl_unref(ns_url);
	nsurl_unref(ns_ref);

#ifdef NSCSS_IMPORT_TRACE
	NSLOG(netsurf, INFO, "Import %d '%s' -> (handle: %p ctx: %p)",
	      c->import_count, lwc_string_data(url),
	      c->imports[c->import_count].c, ctx);
#endif

	c->import_count++;

	return CSS_OK;
}

/**
 * Handler for imported stylesheet events
 *
 * \param handle  Handle for stylesheet
 * \param event   Event object
 * \param pw      Callback context
 * \return NSERROR_OK on success, appropriate error otherwise
 */
nserror nscss_import(hlcache_handle *handle,
		const hlcache_event *event, void *pw)
{
	nscss_import_ctx *ctx = pw;
	css_error error = CSS_OK;

#ifdef NSCSS_IMPORT_TRACE
	NSLOG(netsurf, INFO, "Event %d for %p (%p)", event->type, handle, ctx);
#endif

	assert(ctx->css->imports[ctx->index].c == handle);

	switch (event->type) {
	case CONTENT_MSG_DONE:
		error = nscss_import_complete(ctx);
		break;

	case CONTENT_MSG_ERROR:
		hlcache_handle_release(handle);
		ctx->css->imports[ctx->index].c = NULL;

		error = nscss_import_complete(ctx);
		/* Already released handle */
		break;
	default:
		break;
	}

	/* Preserve out-of-memory. Anything else is OK */
	return error == CSS_NOMEM ? NSERROR_NOMEM : NSERROR_OK;
}

/**
 * Handle an imported stylesheet completing
 *
 * \param ctx  Import context
 * \return CSS_OK on success, appropriate error otherwise
 */
css_error nscss_import_complete(nscss_import_ctx *ctx)
{
	css_error error = CSS_OK;

	/* If this import is the next to be registered, do so */
	if (ctx->css->next_to_register == ctx->index)
		error = nscss_register_imports(ctx->css);

#ifdef NSCSS_IMPORT_TRACE
	NSLOG(netsurf, INFO, "Destroying import context %p for %d", ctx,
	      ctx->index);
#endif

	/* No longer need import context */
	free(ctx);

	return error;
}

/*****************************************************************************
 * Import registration                                                       *
 *****************************************************************************/

/**
 * Register imports with a stylesheet
 *
 * \param c  CSS object containing the imports
 * \return CSS_OK on success, appropriate error otherwise
 */
css_error nscss_register_imports(struct content_css_data *c)
{
	uint32_t index;
	css_error error;

	assert(c->next_to_register != (uint32_t) -1);
	assert(c->next_to_register < c->import_count);

	/* Register imported sheets */
	for (index = c->next_to_register; index < c->import_count; index++) {
		/* Stop registering if we encounter one whose fetch hasn't
		 * completed yet. We'll resume at this point when it has
		 * completed.
		 */
		if (c->imports[index].c != NULL &&
			content_get_status(c->imports[index].c) !=
				CONTENT_STATUS_DONE) {
			break;
		}

		error = nscss_register_import(c, c->imports[index].c);
		if (error != CSS_OK)
			return error;
	}

	/* Record identity of the next import to register */
	c->next_to_register = (uint32_t) index;

	if (c->next_to_register == c->import_count) {
		/* No more imports: notify parent that we're DONE */
		c->done(c, c->pw);
	}

	return CSS_OK;
}


/**
 * Register an import with a stylesheet
 *
 * \param c       CSS object that requested the import
 * \param import  Cache handle of import, or NULL for blank
 * \return CSS_OK on success, appropriate error otherwise
 */
css_error nscss_register_import(struct content_css_data *c,
		const hlcache_handle *import)
{
	css_stylesheet *sheet;
	css_error error;

	if (import != NULL) {
		nscss_content *s =
			(nscss_content *) hlcache_handle_get_content(import);
		sheet = s->data.sheet;
	} else {
		/* Create a blank sheet if needed. */
		if (blank_import == NULL) {
			css_stylesheet_params params;

			params.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
			params.level = CSS_LEVEL_DEFAULT;
			params.charset = NULL;
			params.url = "";
			params.title = NULL;
			params.allow_quirks = false;
			params.inline_style = false;
			params.resolve = nscss_resolve_url;
			params.resolve_pw = NULL;
			params.import = NULL;
			params.import_pw = NULL;
			params.color = ns_system_colour;
			params.color_pw = NULL;
			params.font = NULL;
			params.font_pw = NULL;

			error = css_stylesheet_create(&params, &blank_import);
			if (error != CSS_OK) {
				return error;
			}

			error = css_stylesheet_data_done(blank_import);
			if (error != CSS_OK) {
				css_stylesheet_destroy(blank_import);
				return error;
			}
		}

		sheet = blank_import;
	}

	error = css_stylesheet_register_import(c->sheet, sheet);
	if (error != CSS_OK) {
		return error;
	}

	return error;
}

/**
 * Clean up after the CSS content handler
 */
static void nscss_fini(void)
{
	if (blank_import != NULL) {
		css_stylesheet_destroy(blank_import);
		blank_import = NULL;
	}
	css_hint_fini();
}

/* MacSurf: was a static const designated initializer; CW8 C89 has
 * neither, so populate the vtable at runtime in nscss_init(). */
static content_handler css_content_handler;

/* exported interface documented in netsurf/css.h */
nserror nscss_init(void)
{
	nserror error;

	memset(&css_content_handler, 0, sizeof(css_content_handler));
	css_content_handler.fini = nscss_fini;
	css_content_handler.create = nscss_create;
	css_content_handler.process_data = nscss_process_data;
	css_content_handler.data_complete = nscss_convert;
	css_content_handler.destroy = nscss_destroy;
	css_content_handler.clone = nscss_clone;
	css_content_handler.matches_quirks = nscss_matches_quirks;
	css_content_handler.type = nscss_content_type;
	css_content_handler.no_share = false;

	error = content_factory_register_handler("text/css",
			&css_content_handler);
	if (error != NSERROR_OK)
		goto error;

	error = css_hint_init();
	if (error != NSERROR_OK)
		goto error;

	return NSERROR_OK;

error:
	nscss_fini();

	return error;
}
