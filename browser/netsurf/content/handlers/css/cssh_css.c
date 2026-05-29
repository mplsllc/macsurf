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

#include "utils/ns_errors.h"
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

/** Screen DPI in fixed point units.
 *
 * Upstream NetSurf defaults to 90 (the value RISC OS uses). MacSurf never
 * overrode it, so every CSS length rendered at 90/96 = 93.75% of its CSS px
 * value (css_unit_css2device_px = css_px * device_dpi / 96). That silent
 * shrink desynced em-based containers from HTML width="" attributes and
 * intrinsic image sizes: macintoshgarden.org's #wrapper{width:59em} resolved
 * to 708px instead of 767px, leaving too little room beside its float:right
 * sidebar, so the main-content <table width="560px"> (a block formatting
 * context that can't overlap a float) dropped below the entire sidebar and
 * the page rendered with a blank content column. fixes300b: use 96 so
 * 1 CSS px == 1 device px (the modern browser convention). Pages render
 * ~6.7% larger than before; physical units (pt/in/cm) are treated as if the
 * screen is 96dpi, which is what every mainstream browser does. */
css_fixed nscss_screen_dpi = F_96;

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

	/* fixes160d — per-sheet byte accumulator and skip latch for the
	 * oversize CSS gate. total_bytes sums the size arguments of every
	 * nscss_process_data call for this content; when it passes the cap,
	 * the skip latch is set and all further data is rejected and the
	 * sheet is dropped from the cascade. The latch matches the image
	 * oversize-gate pattern from fixes160b. */
	unsigned long total_bytes;
	int skipped;
} nscss_content;

/* fixes160d — per-stylesheet byte cap. Apple.com ships four CSS files
 * in the 200-256 KB range; each one expands into libcss's struct form
 * to several times its source size. With 20 sheets attached to the
 * cascade context, selector matching across 1000+ boxes blows past
 * what the post-layout path survives even on a 194 MB Carbon partition.
 * 128 KB drops the four heaviest Apple sheets, halves the cascade
 * workload, and leaves the small utility sheets in place.
 *
 * fixes161c — raised to 256 KB. The Apple post-READY crash log
 * (2026-05-21) showed two ~149 KB Apple sheets being dropped, leaving
 * the page in a half-styled cascade state that contributed to the
 * downstream crash.
 *
 * fixes174 — raised to 1 MB. Apple's iPhone page ships a 289 KB
 * overview.built.css that was still being dropped at 256 KB,
 * leaving the page unstyled in places. Browsers are supposed to
 * load whatever they're given; the 16 MB Carbon partition has
 * room for it. Now matches the per-entry HTTP cache cap. */
#define MACOS9_CSS_MAX_BYTES (1024UL * 1024UL)

/*
 * fixes321 — per-page cumulative CSS budget across all sheets. Raised
 * 384 KB -> 1 MB. Heavy modern sites (developers.google.com/fonts ships
 * ~776 KB of CSS across several sheets) hit the old 384 KB cap, dropped
 * 3 stylesheets, and rendered unstyled (SITE log: blocker=css_budget,
 * css_total=794603/393216, css_skip=3). 1 MB fits that page with
 * headroom and matches MACOS9_CSS_MAX_BYTES, so a single max-size sheet
 * and the per-page total now share one ceiling. The 16 MB Carbon
 * partition has room; if a real page OOMs the cascade, this is the first
 * dial to turn back down. Keep in sync with the css_total_cap display
 * literal in html.c's SITE logger. */
#define MACOS9_CSS_TOTAL_BUDGET (1024UL * 1024UL)

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

	/* fixes275g: write value first; trailing space is optional. The
	 * caller (emit_grid_tracks) trims any trailing space after the
	 * last track anyway, so when we're short by exactly 1 byte we
	 * can still emit the value and let the next caller's check decide.
	 * This recovers the 3rd track in the in-place rewrite case where
	 * `30px 30px 30px` (14 chars) needs to fit `30px 30px 30px ` (15
	 * chars with naive trailing spaces). */
	if (emit_len > cap) return -1;
	for (i = 0; i < emit_len; i++) {
		out[i] = tok_to_emit[i];
	}
	if (emit_len + 1 <= cap) {
		out[emit_len] = ' ';
		return (int)(emit_len + 1);
	}
	return (int)emit_len;
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
			static const char N_GA[]   = "grid-area";
			size_t name_len = 0;
			int prop = 0; /* 1=GC 2=GR 3=GCS 4=GCE 5=GRS 6=GRE 7=GA */

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
					N_GA, sizeof(N_GA) - 1)) {
				prop = 7; name_len = sizeof(N_GA) - 1;
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
				} else if (prop == 7) {
					/* fixes178c: grid-area shorthand.
					 *
					 * 4-value form:  rs / cs / re / ce
					 * 2-value form:  rs / cs   (span 1)
					 *
					 * Named-area form (single ident) is
					 * NOT supported in V1 — that needs
					 * grid-template-areas resolution
					 * which is deferred. A single-token
					 * ident value falls through with no
					 * placement emitted.
					 *
					 * Tokens are positive ints 1..254 or
					 * -1 (the "fill row" sentinel encoded
					 * as 255). */
					int rs2 = 0, cs2 = 0, re2 = 0, ce2 = 0;
					int slashes = 0;
					const char *q = p;
					/* Count `/` separators (excluding any
					 * outside whitespace) so we can pick
					 * which form to expect. */
					{
						const char *qq = p;
						while (qq < end) {
							if (*qq == '/') slashes++;
							qq++;
						}
					}
					rs2 = macsurf__grid_parse_int_or_minus1(
							&q, end);
					if (rs2 >= 1 && rs2 <= 254 && q < end &&
							*q == '/') {
						q++;
						cs2 = macsurf__grid_parse_int_or_minus1(
								&q, end);
						if (slashes >= 3) {
							if (q < end && *q == '/') q++;
							re2 = macsurf__grid_parse_int_or_minus1(
									&q, end);
							if (q < end && *q == '/') q++;
							ce2 = macsurf__grid_parse_int_or_minus1(
									&q, end);
						}
					}
					if (rs2 >= 1 && rs2 <= 254 &&
							cs2 >= 1 && cs2 <= 254) {
						acc->row_start = (uint8_t)rs2;
						acc->col_start = (uint8_t)cs2;
						if (slashes >= 3) {
							/* row_end */
							if (re2 == 255) {
								acc->row_span = 255;
							} else if (re2 > rs2 &&
									re2 <= 254) {
								int sp2 = re2 - rs2;
								if (sp2 > 254) sp2 = 254;
								acc->row_span = (uint8_t)sp2;
							} else {
								acc->row_span = 1;
							}
							/* col_end */
							if (ce2 == 255) {
								acc->col_span = 255;
							} else if (ce2 > cs2 &&
									ce2 <= 254) {
								int sp2 = ce2 - cs2;
								if (sp2 > 254) sp2 = 254;
								acc->col_span = (uint8_t)sp2;
							} else {
								acc->col_span = 1;
							}
						} else {
							acc->row_span = 1;
							acc->col_span = 1;
						}
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


/* fixes175 — rewrite every author `text-shadow: VALUE` declaration to
 * `-macsurf-text-shadow: VALUE` so the existing vendor parser
 * (p_macsurf_text_shadow.c from fixes50) picks up standard CSS3
 * text-shadow without touching libcss internals.
 *
 * Standard CSS3 text-shadow accepts an optional blur radius and lets the
 * color appear first or last. The vendor parser is stricter and accepts
 * only `<hoff> <voff> <color>`. This pass renames the declaration but
 * does NOT normalise the value, so authors using a blur radius or
 * color-first form will still see the declaration parse-fail at libcss.
 * Closing that gap is a separate, narrower fix on the vendor parser.
 *
 * The grow-buffer scheme is the same one fixes151 uses for grid-column:
 * malloc with worst-case headroom, copy bytes through, substitute
 * property name on match.
 *
 * Property-level aliasing inside libcss (the fixes141 approach) hung the
 * browser pre-reformat on real hardware; the preprocessor route is the
 * safe pattern for vendor->standard bridges. */


/* fixes178b: grid-template-areas + named grid-area resolution.
 *
 * Document-scope V1: scans the input for any
 *   grid-template-areas: "a b c" "d d e" ...
 * declarations and builds a flat name->bbox table. Then scans for any
 *   grid-area: <ident>
 * single-token declarations and rewrites them to numeric 4-value form
 *   grid-area: rs / cs / re / ce
 * which the existing prop==7 handler in the placement pass turns into a
 * packed `-macsurf-grid-col-span` declaration.
 *
 * Limitations:
 *  - 32 areas max, 32 char per name max, 8x8 grid max
 *  - last-write-wins on name conflicts across multiple
 *    grid-template-areas declarations
 *  - cells named `.` are treated as empty / skipped
 *  - the original grid-template-areas declaration stays in the output;
 *    libcss does not have a parser for it, so it parse-fails silently.
 *  - if the area parse hits any error, the pass returns NULL and the
 *    caller falls back to the unmodified input.
 */

#define MACSURF_GTA_NAMES_MAX 32
#define MACSURF_GTA_NAME_MAX  32

struct macsurf_gta_area {
	char  name[MACSURF_GTA_NAME_MAX];
	uint8_t col_start; /* 1-indexed */
	uint8_t row_start;
	uint8_t col_end;   /* 1-indexed, inclusive */
	uint8_t row_end;
};

struct macsurf_gta_table {
	struct macsurf_gta_area entries[MACSURF_GTA_NAMES_MAX];
	int n;
};

static int macsurf_gta__is_ident_char(char c)
{
	return (c >= 'a' && c <= 'z') ||
	       (c >= 'A' && c <= 'Z') ||
	       (c >= '0' && c <= '9') ||
	       c == '-' || c == '_';
}

static int macsurf_gta__find(struct macsurf_gta_table *tbl,
		const char *name, size_t name_len)
{
	int k;
	for (k = 0; k < tbl->n; k++) {
		size_t L = strlen(tbl->entries[k].name);
		if (L == name_len &&
		    memcmp(tbl->entries[k].name, name, L) == 0)
			return k;
	}
	return -1;
}

/* Parse one `grid-template-areas: "..." "..." ...` declaration value
 * starting at `p` (just past the `:`) and ending at `end`. Updates the
 * shared table.
 *
 * Per CSS Grid spec each `"..."` is a row of whitespace-separated cell
 * tokens. Each cell is either a single `.` (null cell) or an
 * identifier. All cells in a contiguous rectangular bbox sharing the
 * same name belong to the same area. We compute a bbox per unique
 * name. Non-rectangular groupings (illegal per spec) end up with the
 * minimum bbox containing all instances. */
static void macsurf_gta__parse_decl(const char *p, const char *end,
		struct macsurf_gta_table *tbl)
{
	int row = 0;
	while (p < end) {
		const char *str_start;
		const char *str_end;
		int col = 0;
		/* Find next `"..."`. */
		while (p < end && *p != '"') p++;
		if (p >= end) return;
		p++;
		str_start = p;
		while (p < end && *p != '"') p++;
		if (p >= end) return;
		str_end = p;
		p++;
		/* Walk cells inside the string. */
		{
			const char *q = str_start;
			while (q < str_end) {
				const char *cell_s;
				const char *cell_e;
				size_t L;
				while (q < str_end &&
				       (*q == ' ' || *q == '\t'))
					q++;
				if (q >= str_end) break;
				cell_s = q;
				while (q < str_end &&
				       *q != ' ' && *q != '\t')
					q++;
				cell_e = q;
				L = (size_t)(cell_e - cell_s);
				if (L == 0) { col++; continue; }
				if (L == 1 && cell_s[0] == '.') {
					col++;
					continue;
				}
				if (L < MACSURF_GTA_NAME_MAX) {
					int idx = macsurf_gta__find(tbl,
						cell_s, L);
					if (idx < 0 &&
					    tbl->n < MACSURF_GTA_NAMES_MAX) {
						idx = tbl->n++;
						memcpy(tbl->entries[idx].name,
						       cell_s, L);
						tbl->entries[idx].name[L] = 0;
						tbl->entries[idx].col_start =
							(uint8_t)(col + 1);
						tbl->entries[idx].col_end =
							(uint8_t)(col + 1);
						tbl->entries[idx].row_start =
							(uint8_t)(row + 1);
						tbl->entries[idx].row_end =
							(uint8_t)(row + 1);
					} else if (idx >= 0) {
						uint8_t cs1 = (uint8_t)(col + 1);
						uint8_t rs1 = (uint8_t)(row + 1);
						if (cs1 < tbl->entries[idx].col_start)
							tbl->entries[idx].col_start = cs1;
						if (cs1 > tbl->entries[idx].col_end)
							tbl->entries[idx].col_end = cs1;
						if (rs1 < tbl->entries[idx].row_start)
							tbl->entries[idx].row_start = rs1;
						if (rs1 > tbl->entries[idx].row_end)
							tbl->entries[idx].row_end = rs1;
					}
				}
				col++;
			}
		}
		row++;
		if (row > 254) return;
	}
}

static char *
macsurf__rewrite_grid_template_areas(const char *data, size_t in_size,
		size_t *out_size_p)
{
	static const char N_GTA[] = "grid-template-areas";
	static const size_t N_GTA_LEN = 19;
	static const char N_GA[]  = "grid-area";
	static const size_t N_GA_LEN  = 9;
	struct macsurf_gta_table tbl;
	size_t cap;
	size_t pos = 0;
	char *out;
	size_t i = 0;

	tbl.n = 0;
	memset(&tbl.entries, 0, sizeof(tbl.entries));

	/* --- Pass 1: collect area definitions. */
	while (i + N_GTA_LEN <= in_size) {
		if (data[i] == 'g' &&
		    macsurf__match_prop_name(data, in_size, i,
				N_GTA, N_GTA_LEN)) {
			size_t j = i + N_GTA_LEN;
			while (j < in_size && (data[j] == ' ' ||
					data[j] == '\t' ||
					data[j] == '\n' ||
					data[j] == '\r')) j++;
			if (j < in_size && data[j] == ':') {
				size_t val_start = j + 1;
				size_t val_end = val_start;
				while (val_end < in_size) {
					char vc = data[val_end];
					if (vc == ';' || vc == '}') break;
					val_end++;
				}
				macsurf_gta__parse_decl(data + val_start,
						data + val_end, &tbl);
				i = val_end;
				continue;
			}
		}
		i++;
	}

	if (tbl.n == 0) {
		*out_size_p = in_size;
		out = (char *)malloc(in_size + 1);
		if (out == NULL) return NULL;
		memcpy(out, data, in_size);
		out[in_size] = 0;
		return out;
	}

	/* --- Pass 2: rewrite `grid-area: <ident>` to numeric form. */
	cap = in_size + 256;
	if (cap < 1024) cap = 1024;
	out = (char *)malloc(cap);
	if (out == NULL) return NULL;
	i = 0;
	while (i < in_size) {
		if (data[i] == 'g' && i + N_GA_LEN <= in_size &&
		    macsurf__match_prop_name(data, in_size, i,
				N_GA, N_GA_LEN)) {
			size_t j = i + N_GA_LEN;
			size_t val_start;
			size_t val_end;
			size_t name_s, name_e;
			int idx;
			while (j < in_size && (data[j] == ' ' ||
					data[j] == '\t' ||
					data[j] == '\n' ||
					data[j] == '\r')) j++;
			if (j >= in_size || data[j] != ':') goto emit_byte;
			val_start = j + 1;
			while (val_start < in_size &&
				(data[val_start] == ' ' ||
				 data[val_start] == '\t')) val_start++;
			val_end = val_start;
			while (val_end < in_size) {
				char vc = data[val_end];
				if (vc == ';' || vc == '}' ||
						vc == '!') break;
				val_end++;
			}
			/* Look at the value: only single-IDENT form is
			 * the named-area shorthand. If it contains `/`
			 * or a digit anywhere, leave it for the numeric
			 * handler. */
			{
				const char *q;
				int has_slash = 0;
				for (q = data + val_start;
						q < data + val_end; q++) {
					if (*q == '/') { has_slash = 1; break; }
				}
				if (has_slash) goto emit_byte;
			}
			name_s = val_start;
			while (name_s < val_end &&
				(data[name_s] == ' ' ||
				 data[name_s] == '\t')) name_s++;
			name_e = name_s;
			while (name_e < val_end &&
				macsurf_gta__is_ident_char(data[name_e]))
				name_e++;
			if (name_e == name_s ||
				(name_e - name_s) >= MACSURF_GTA_NAME_MAX)
				goto emit_byte;
			idx = macsurf_gta__find(&tbl, data + name_s,
					(size_t)(name_e - name_s));
			if (idx < 0) goto emit_byte;
			{
				/* Emit `grid-area: rs / cs / re / ce`. */
				char tmp[80];
				int n;
				size_t need;
				n = sprintf(tmp,
					"grid-area: %u / %u / %u / %u",
					(unsigned)tbl.entries[idx].row_start,
					(unsigned)tbl.entries[idx].col_start,
					(unsigned)(tbl.entries[idx].row_end + 1),
					(unsigned)(tbl.entries[idx].col_end + 1));
				if (n <= 0) goto emit_byte;
				need = pos + (size_t)n + 1;
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
emit_byte:
		{
			size_t need = pos + 2;
			while (need >= cap) {
				size_t newcap = cap * 2;
				char *neu = (char *)realloc(out, newcap);
				if (neu == NULL) {
					free(out);
					return NULL;
				}
				out = neu;
				cap = newcap;
			}
		}
		out[pos++] = data[i++];
	}
	*out_size_p = pos;
	return out;
}


static char *
macsurf__rewrite_text_shadow(const char *data, size_t in_size,
		size_t *out_size_p)
{
	static const char NEEDLE[] = "text-shadow";
	static const size_t NEEDLE_LEN = 11;
	static const char REPLACE[] = "-macsurf-text-shadow";
	static const size_t REPLACE_LEN = 20;
	char *out;
	size_t cap;
	size_t pos = 0;
	size_t i = 0;

	/* Worst case: every NEEDLE_LEN-byte window is a match. Each match
	 * grows by REPLACE_LEN - NEEDLE_LEN = 9 bytes. Plus 256 padding so
	 * single-byte appends near the tail never reallocate. */
	cap = in_size +
		(in_size / NEEDLE_LEN + 1) * (REPLACE_LEN - NEEDLE_LEN) + 256;
	out = (char *)malloc(cap);
	if (out == NULL) return NULL;

	while (i < in_size) {
		if (macsurf__match_prop_name(data, in_size, i,
				NEEDLE, NEEDLE_LEN)) {
			/* Boundary check passed; confirm this is a real
			 * declaration by looking for optional whitespace
			 * then ':'. The boundary check already rejects
			 * `-macsurf-text-shadow` (prev byte is '-'). */
			size_t j = i + NEEDLE_LEN;
			while (j < in_size && (data[j] == ' ' ||
					data[j] == '\t' ||
					data[j] == '\n' ||
					data[j] == '\r')) j++;
			if (j < in_size && data[j] == ':') {
				if (pos + REPLACE_LEN >= cap) {
					free(out);
					return NULL;
				}
				memcpy(out + pos, REPLACE, REPLACE_LEN);
				pos += REPLACE_LEN;
				i += NEEDLE_LEN;
				continue;
			}
		}
		if (pos + 1 >= cap) {
			free(out);
			return NULL;
		}
		out[pos++] = data[i++];
	}

	*out_size_p = pos;
	return out;
}


/* fixes183 — rewrite standard `transform:` → `-macsurf-transform:` so
 * author CSS reaches the existing vendor transform paint path (sin/cos
 * LUT rotation, translate, scale composition). Mirrors fixes175 text-shadow
 * pattern. `transform-origin`, `transform-style`, `transform-box` are NOT
 * matched because the post-NEEDLE scan requires `:` next (those longhands
 * have `-` next so they fall through unchanged). fixes141 attempted this
 * via property_handlers[] aliasing and hung pre-reformat; this preprocessor
 * route bypasses libcss internals entirely. */
static char *
macsurf__rewrite_transform(const char *data, size_t in_size,
		size_t *out_size_p)
{
	static const char NEEDLE[] = "transform";
	static const size_t NEEDLE_LEN = 9;
	static const char REPLACE[] = "-macsurf-transform";
	static const size_t REPLACE_LEN = 18;
	char *out;
	size_t cap;
	size_t pos = 0;
	size_t i = 0;

	cap = in_size +
		(in_size / NEEDLE_LEN + 1) * (REPLACE_LEN - NEEDLE_LEN) + 256;
	out = (char *)malloc(cap);
	if (out == NULL) return NULL;

	while (i < in_size) {
		if (macsurf__match_prop_name(data, in_size, i,
				NEEDLE, NEEDLE_LEN)) {
			size_t j = i + NEEDLE_LEN;
			while (j < in_size && (data[j] == ' ' ||
					data[j] == '\t' ||
					data[j] == '\n' ||
					data[j] == '\r')) j++;
			if (j < in_size && data[j] == ':') {
				if (pos + REPLACE_LEN >= cap) {
					free(out);
					return NULL;
				}
				memcpy(out + pos, REPLACE, REPLACE_LEN);
				pos += REPLACE_LEN;
				i += NEEDLE_LEN;
				continue;
			}
		}
		if (pos + 1 >= cap) {
			free(out);
			return NULL;
		}
		out[pos++] = data[i++];
	}

	*out_size_p = pos;
	return out;
}

/* fixes191g -- rewrite standard `object-position:` to
 * `-macsurf-object-position:` so author CSS reaches the compact
 * keyword-only V1 parser. Like transform/text-shadow, this only matches
 * the exact property name followed by optional whitespace and `:`, so
 * unrelated longhands are left alone. */
static char *
macsurf__rewrite_object_position(const char *data, size_t in_size,
		size_t *out_size_p)
{
	static const char NEEDLE[] = "object-position";
	static const size_t NEEDLE_LEN = 15;
	static const char REPLACE[] = "-macsurf-object-position";
	static const size_t REPLACE_LEN = 24;
	char *out;
	size_t cap;
	size_t pos = 0;
	size_t i = 0;

	cap = in_size +
		(in_size / NEEDLE_LEN + 1) * (REPLACE_LEN - NEEDLE_LEN) + 256;
	out = (char *)malloc(cap);
	if (out == NULL) return NULL;

	while (i < in_size) {
		if (macsurf__match_prop_name(data, in_size, i,
				NEEDLE, NEEDLE_LEN)) {
			size_t j = i + NEEDLE_LEN;
			while (j < in_size && (data[j] == ' ' ||
					data[j] == '\t' ||
					data[j] == '\n' ||
					data[j] == '\r')) j++;
			if (j < in_size && data[j] == ':') {
				if (pos + REPLACE_LEN >= cap) {
					free(out);
					return NULL;
				}
				memcpy(out + pos, REPLACE, REPLACE_LEN);
				pos += REPLACE_LEN;
				i += NEEDLE_LEN;
				continue;
			}
		}
		if (pos + 1 >= cap) {
			free(out);
			return NULL;
		}
		out[pos++] = data[i++];
	}

	out[pos] = '\0';
	if (out_size_p != NULL) *out_size_p = pos;
	return out;
}


/* fixes266 — rewrite `background-image: linear-gradient(...)` to
 * `-macsurf-gradient: linear-gradient(...)` so author CSS that uses the
 * standard CSS3 syntax reaches the existing -macsurf-gradient paint path
 * (fixes47/49 linear-gradient, fixes74 radial-gradient).
 *
 * Scope and limits:
 *   - Matches only when the value (after optional whitespace) STARTS with
 *     `linear-gradient(` or `radial-gradient(`. URLs and other functions
 *     fall through untouched (libcss continues to drop them).
 *   - Stacked layered backgrounds like `background-image: linear-gradient(a),
 *     linear-gradient(b)` rename to `-macsurf-gradient: ...` but the parser
 *     fails on the trailing `, linear-gradient(...)` — net result: stacked
 *     gradients drop, same as pre-fix behaviour. Single-layer gradients
 *     paint, which is the strict improvement.
 *   - Does NOT touch `background:` shorthand. Authors who use shorthand for
 *     gradients (rare) need to opt in with `background-image:`.
 *   - REPLACE is one byte longer than NEEDLE (17 vs 16), so each match
 *     grows the output by 1; pre-allocate accordingly. */
static char *
macsurf__rewrite_background_image_gradient(const char *data, size_t in_size,
		size_t *out_size_p)
{
	static const char NEEDLE[] = "background-image";
	static const size_t NEEDLE_LEN = 16;
	static const char REPLACE[] = "-macsurf-gradient";
	static const size_t REPLACE_LEN = 17;
	char *out;
	size_t cap;
	size_t pos = 0;
	size_t i = 0;

	cap = in_size +
		(in_size / NEEDLE_LEN + 1) * (REPLACE_LEN - NEEDLE_LEN) + 256;
	out = (char *)malloc(cap);
	if (out == NULL) return NULL;

	while (i < in_size) {
		if (macsurf__match_prop_name(data, in_size, i,
				NEEDLE, NEEDLE_LEN)) {
			size_t j = i + NEEDLE_LEN;
			while (j < in_size && (data[j] == ' ' ||
					data[j] == '\t' ||
					data[j] == '\n' ||
					data[j] == '\r')) j++;
			if (j < in_size && data[j] == ':') {
				size_t k = j + 1;
				bool is_gradient = false;
				while (k < in_size && (data[k] == ' ' ||
						data[k] == '\t' ||
						data[k] == '\n' ||
						data[k] == '\r')) k++;
				/* Test for `linear-gradient(` or `radial-gradient(`
				 * (case-insensitive) at value start. */
				if (k + 16 <= in_size) {
					const char *p = data + k;
					if ((p[0] == 'l' || p[0] == 'L') &&
					    (p[1] == 'i' || p[1] == 'I') &&
					    (p[2] == 'n' || p[2] == 'N') &&
					    (p[3] == 'e' || p[3] == 'E') &&
					    (p[4] == 'a' || p[4] == 'A') &&
					    (p[5] == 'r' || p[5] == 'R') &&
					     p[6] == '-' &&
					    (p[7] == 'g' || p[7] == 'G') &&
					    (p[8] == 'r' || p[8] == 'R') &&
					    (p[9] == 'a' || p[9] == 'A') &&
					    (p[10] == 'd' || p[10] == 'D') &&
					    (p[11] == 'i' || p[11] == 'I') &&
					    (p[12] == 'e' || p[12] == 'E') &&
					    (p[13] == 'n' || p[13] == 'N') &&
					    (p[14] == 't' || p[14] == 'T') &&
					     p[15] == '(')
						is_gradient = true;
				}
				if (k + 16 <= in_size && !is_gradient) {
					const char *p = data + k;
					if ((p[0] == 'r' || p[0] == 'R') &&
					    (p[1] == 'a' || p[1] == 'A') &&
					    (p[2] == 'd' || p[2] == 'D') &&
					    (p[3] == 'i' || p[3] == 'I') &&
					    (p[4] == 'a' || p[4] == 'A') &&
					    (p[5] == 'l' || p[5] == 'L') &&
					     p[6] == '-' &&
					    (p[7] == 'g' || p[7] == 'G') &&
					    (p[8] == 'r' || p[8] == 'R') &&
					    (p[9] == 'a' || p[9] == 'A') &&
					    (p[10] == 'd' || p[10] == 'D') &&
					    (p[11] == 'i' || p[11] == 'I') &&
					    (p[12] == 'e' || p[12] == 'E') &&
					    (p[13] == 'n' || p[13] == 'N') &&
					    (p[14] == 't' || p[14] == 'T') &&
					     p[15] == '(')
						is_gradient = true;
				}
				if (is_gradient) {
					if (pos + REPLACE_LEN >= cap) {
						free(out);
						return NULL;
					}
					memcpy(out + pos, REPLACE, REPLACE_LEN);
					pos += REPLACE_LEN;
					i += NEEDLE_LEN;
					continue;
				}
			}
		}
		if (pos + 1 >= cap) {
			free(out);
			return NULL;
		}
		out[pos++] = data[i++];
	}

	out[pos] = '\0';
	if (out_size_p != NULL) *out_size_p = pos;
	return out;
}


/* fixes279 (#27) — strip trailing stacked gradients from
 * `-macsurf-gradient:` declarations. Author CSS commonly stacks layered
 * backgrounds like `background-image: linear-gradient(a), linear-gradient(b)`.
 * After fixes266's rename pass we have `-macsurf-gradient: linear-gradient(a),
 * linear-gradient(b)` which libcss's vendor-gradient parser rejects on the
 * trailing `, linear-gradient(b)`. This pass walks each `-macsurf-gradient:`
 * declaration, finds the first depth-0 comma in the value, and replaces it
 * plus everything up to `;` / `}` / `!` with spaces. The first gradient
 * survives intact.
 *
 * In-place same-size rewrite (only spaces are introduced). */
static char *
macsurf__strip_stacked_gradients(const char *data, size_t in_size)
{
	static const char NEEDLE[] = "-macsurf-gradient";
	static const size_t NEEDLE_LEN = 17;
	char *out;
	size_t i;
	int changed = 0;

	out = (char *)malloc(in_size);
	if (out == NULL) return NULL;
	memcpy(out, data, in_size);

	i = 0;
	while (i + NEEDLE_LEN < in_size) {
		size_t j;
		size_t k;
		int paren = 0;
		bool found_comma = false;
		size_t comma_pos = 0;

		if (!macsurf__match_prop_name(out, in_size, i,
				NEEDLE, NEEDLE_LEN)) {
			i++;
			continue;
		}
		j = i + NEEDLE_LEN;
		while (j < in_size && (out[j] == ' ' || out[j] == '\t' ||
				out[j] == '\n' || out[j] == '\r')) j++;
		if (j >= in_size || out[j] != ':') {
			i = j;
			continue;
		}
		k = j + 1;
		/* Scan value, tracking paren depth. */
		while (k < in_size) {
			char c = out[k];
			if (c == '(') paren++;
			else if (c == ')') {
				if (paren > 0) paren--;
			} else if (paren == 0 && c == ',') {
				found_comma = true;
				comma_pos = k;
				break;
			} else if (paren == 0 &&
					(c == ';' || c == '}' || c == '!')) {
				break;
			}
			k++;
		}
		if (found_comma) {
			/* Wipe from the comma to the value terminator. */
			size_t w = comma_pos;
			int p2 = 0;
			while (w < in_size) {
				char c = out[w];
				if (c == '(') p2++;
				else if (c == ')') {
					if (p2 > 0) p2--;
				} else if (p2 == 0 && (c == ';' ||
						c == '}' || c == '!')) {
					break;
				}
				out[w] = ' ';
				w++;
			}
			changed = 1;
		}
		i = k;
	}

	if (!changed) {
		free(out);
		return NULL;
	}
	return out;
}


/* fixes280 — narrow calc() evaluator for the aspect-ratio padding hack.
 *
 * Real-world pattern: `padding-top: calc(105 / 478 * 100%)`. mactrove's
 * Platinum theme uses this to maintain a fixed aspect ratio on absolutely-
 * positioned banner images. If libcss can't fold the expression, padding-
 * top stays 0, the container collapses, and the positioned images render
 * at 0×0.
 *
 * Scope: recognise EXACTLY the patterns
 *   calc( <num> / <num> * <num>% )
 *   calc( <num> * <num>% )
 *   calc( <num>% * <num> )
 *   calc( <num> / <num> )       -> emit unitless number scaled to 4 decimals
 * Numbers are integer or decimal; whitespace flexible. Anything more
 * complex falls through unchanged for libcss's existing calc parser.
 *
 * In-place same-size rewrite: output is padded with trailing spaces so
 * the declaration's length stays put.
 *
 * Why a preprocessor instead of fixing libcss's calc: libcss's calc
 * resolution happens at bytecode-eval time and supports lengths better
 * than percentages-with-arithmetic. This pattern is constant-folded at
 * source-rewrite time so libcss sees a plain percentage. */
static int
macsurf__cc_skip_ws(const char *s, int len, int p)
{
	while (p < len && (s[p] == ' ' || s[p] == '\t' ||
			s[p] == '\n' || s[p] == '\r'))
		p++;
	return p;
}

static bool
macsurf__cc_read_number(const char *s, int len, int *p,
		double *out)
{
	int start = *p;
	bool seen_digit = false;
	double sign = 1.0;
	double value = 0.0;
	double frac = 0.0;
	double div = 10.0;

	if (*p < len && s[*p] == '-') { sign = -1.0; (*p)++; }
	else if (*p < len && s[*p] == '+') { (*p)++; }
	while (*p < len && s[*p] >= '0' && s[*p] <= '9') {
		value = value * 10.0 + (s[*p] - '0');
		seen_digit = true;
		(*p)++;
	}
	if (*p < len && s[*p] == '.') {
		(*p)++;
		while (*p < len && s[*p] >= '0' && s[*p] <= '9') {
			frac += (s[*p] - '0') / div;
			div *= 10.0;
			seen_digit = true;
			(*p)++;
		}
	}
	if (!seen_digit) {
		*p = start;
		return false;
	}
	*out = sign * (value + frac);
	return true;
}

static char *
macsurf__rewrite_calc_aspect(const char *data, size_t in_size)
{
	static const char NEEDLE[] = "calc(";
	static const size_t NEEDLE_LEN = 5;
	char *out;
	size_t i;
	int changed = 0;

	out = (char *)malloc(in_size);
	if (out == NULL) return NULL;
	memcpy(out, data, in_size);

	i = 0;
	while (i + NEEDLE_LEN < in_size) {
		int p, q;
		double a, b, c;
		bool ok;
		char rep[40];
		size_t rlen;
		size_t close_pos;
		int paren;
		bool has_pct = false;
		double result = 0.0;
		bool result_is_pct = false;
		int op1 = 0;
		int op2 = 0;

		if (memcmp(out + i, NEEDLE, NEEDLE_LEN) != 0) {
			i++;
			continue;
		}

		p = (int)(i + NEEDLE_LEN);
		paren = 1;
		q = p;
		while (q < (int)in_size && paren > 0) {
			if (out[q] == '(') paren++;
			else if (out[q] == ')') {
				paren--;
				if (paren == 0) break;
			}
			q++;
		}
		if (q >= (int)in_size) { i++; continue; }
		close_pos = (size_t)q;

		/* Try to read: <num>[%] [op <num>[%] [op <num>[%]]] */
		p = macsurf__cc_skip_ws(out, q, p);
		ok = macsurf__cc_read_number(out, q, &p, &a);
		if (!ok) { i = close_pos + 1; continue; }
		p = macsurf__cc_skip_ws(out, q, p);
		if (p < q && out[p] == '%') { has_pct = true; result_is_pct = true; p++; }
		p = macsurf__cc_skip_ws(out, q, p);

		if (p >= q) {
			/* calc(num) or calc(num%) — already simple, skip. */
			i = close_pos + 1;
			continue;
		}

		if (out[p] == '*' || out[p] == '/') {
			op1 = out[p];
			p++;
			p = macsurf__cc_skip_ws(out, q, p);
			ok = macsurf__cc_read_number(out, q, &p, &b);
			if (!ok) { i = close_pos + 1; continue; }
			p = macsurf__cc_skip_ws(out, q, p);
			if (p < q && out[p] == '%') {
				has_pct = true;
				result_is_pct = true;
				p++;
			}
			p = macsurf__cc_skip_ws(out, q, p);
		} else {
			i = close_pos + 1;
			continue;
		}

		if (p < q && (out[p] == '*' || out[p] == '/')) {
			op2 = out[p];
			p++;
			p = macsurf__cc_skip_ws(out, q, p);
			ok = macsurf__cc_read_number(out, q, &p, &c);
			if (!ok) { i = close_pos + 1; continue; }
			p = macsurf__cc_skip_ws(out, q, p);
			if (p < q && out[p] == '%') {
				has_pct = true;
				result_is_pct = true;
				p++;
			}
			p = macsurf__cc_skip_ws(out, q, p);
		}

		if (p != q) { i = close_pos + 1; continue; }
		(void)has_pct;

		/* Compute. */
		result = a;
		if (op1 == '*') result = result * b;
		else if (op1 == '/') {
			if (b == 0.0) { i = close_pos + 1; continue; }
			result = result / b;
		}
		if (op2 == '*') result = result * c;
		else if (op2 == '/') {
			if (c == 0.0) { i = close_pos + 1; continue; }
			result = result / c;
		}

		rlen = (size_t)sprintf(rep, result_is_pct ? "%.4f%%" : "%.4f",
				result);
		/* Replace calc(...) [length = close_pos - i + 1] with rep,
		 * pad remainder with spaces. */
		{
			size_t span = close_pos - i + 1;
			if (rlen > span) {
				/* Won't fit — leave alone. */
				i = close_pos + 1;
				continue;
			}
			memcpy(out + i, rep, rlen);
			memset(out + i + rlen, ' ', span - rlen);
		}
		i = close_pos + 1;
		changed = 1;
	}

	if (!changed) { free(out); return NULL; }
	return out;
}


/* fixes281 — fallback solid color for repeating-linear-gradient.
 *
 * mactrove's Platinum theme paints window title bars with
 *   background: repeating-linear-gradient(to bottom,
 *                 #ffffff 0, #ffffff 1px,
 *                 #cccccc 1px, #cccccc 2px, ...);
 * to get a horizontal 1-px-stripe pattern. fixes185's current handling
 * is to strip the "repeating-" prefix, leaving a plain linear-gradient
 * whose first two color stops are barely 1px apart — libcss collapses
 * that to nearly solid white, and title bars appear empty.
 *
 * V1 fix: detect `repeating-linear-gradient(...)` / `repeating-radial-
 * gradient(...)` (case-insensitive) and replace the entire call with a
 * neutral platinum-grey hex color `#dddddd`. In the `background:`
 * shorthand context this sets background-color, giving title bars a
 * visible solid backdrop instead of being invisible.
 *
 * Trade-off: loses the striped texture but matches average tonal value
 * and is far better than nothing. A real repeating-gradient plotter
 * (QuickDraw pattern fill) is a separate round.
 *
 * In-place same-size rewrite (pads with trailing spaces). */
static char *
macsurf__rewrite_repeating_gradient_solid(const char *data, size_t in_size)
{
	char *out;
	size_t i;
	int changed = 0;

	out = (char *)malloc(in_size);
	if (out == NULL) return NULL;
	memcpy(out, data, in_size);

	i = 0;
	while (i + 26 < in_size) {
		const char *p = out + i;
		bool match_lin = false;
		bool match_rad = false;
		size_t j;
		size_t span;
		int paren;
		static const char LIN[] = "repeating-linear-gradient(";
		static const char RAD[] = "repeating-radial-gradient(";
		static const size_t REPL_LEN = 7; /* "#dddddd" */

		/* Case-insensitive match for both. */
		{
			size_t m;
			match_lin = true;
			for (m = 0; m < 26; m++) {
				char a = p[m];
				char b = LIN[m];
				if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
				if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
				if (a != b) { match_lin = false; break; }
			}
			if (!match_lin) {
				match_rad = true;
				for (m = 0; m < 26; m++) {
					char a = p[m];
					char b = RAD[m];
					if (a >= 'A' && a <= 'Z')
						a = (char)(a - 'A' + 'a');
					if (b >= 'A' && b <= 'Z')
						b = (char)(b - 'A' + 'a');
					if (a != b) {
						match_rad = false;
						break;
					}
				}
			}
		}
		if (!match_lin && !match_rad) {
			i++;
			continue;
		}

		/* Find matching close paren. */
		j = i + 26;
		paren = 1;
		while (j < in_size && paren > 0) {
			if (out[j] == '(') paren++;
			else if (out[j] == ')') {
				paren--;
				if (paren == 0) break;
			}
			j++;
		}
		if (j >= in_size) { i++; continue; }

		span = j - i + 1;
		if (span < REPL_LEN) { i = j + 1; continue; }

		/* Replace with #dddddd + spaces. */
		memcpy(out + i, "#dddddd", REPL_LEN);
		memset(out + i + REPL_LEN, ' ', span - REPL_LEN);
		i = j + 1;
		changed = 1;
	}

	if (!changed) { free(out); return NULL; }
	return out;
}


/* fixes185 — modern-CSS compatibility preprocessor. Rewrites unsupported
 * modern syntax into supported equivalents (or drops it) so author CSS
 * keeps cascading instead of having rules silently dropped by libcss.
 * In-place rewrites only (length-preserving): no new property names,
 * no libcss internal changes. Mirrors the fixes175 text-shadow / fixes183
 * transform preprocessor pattern.
 *
 * Rewrites:
 *  - `:focus-visible` -> `:focus        ` (8 trailing spaces; same length)
 *  - `:focus-within`  -> `:focus       ` (7 trailing spaces; same length)
 *  - declarations dropped (name+value replaced with spaces, terminator kept):
 *    -webkit-line-clamp, line-clamp, image-rendering,
 *    font-variant-numeric, break-inside, outline-offset
 *  - `repeating-linear-gradient(` -> `          linear-gradient(`
 *    (10 leading spaces; first cycle still renders via fixes49)
 *  - `repeating-radial-gradient(` -> `          radial-gradient(`
 */
static char *
macsurf__rewrite_modern_compat(const char *data, size_t in_size,
		size_t *out_size_p)
{
	static const char *const DROP_PROPS[] = {
		"-webkit-line-clamp",
		"line-clamp",
		"image-rendering",
		"font-variant-numeric",
		"break-inside",
		"outline-offset",
		/* fixes191f -- transition/animation/keyframes-triggered props
		 * silently dropped. MacSurf has no animation timer playback in
		 * this round; the final static computed value still applies via
		 * the regular cascade. The shorthand and all longhands need to
		 * drop together so the value isn't a stray ident the parser
		 * trips on. */
		"transition",
		"transition-property",
		"transition-duration",
		"transition-timing-function",
		"transition-delay",
		"animation",
		"animation-name",
		"animation-duration",
		"animation-timing-function",
		"animation-delay",
		"animation-iteration-count",
		"animation-direction",
		"animation-fill-mode",
		"animation-play-state",
		/* fixes201 -- pointer-events PROMOTED out of the drop list.
		 * libcss now has a real CSS_PROP_POINTER_EVENTS with parser
		 * + selector + accessor wiring; the cascade respects the
		 * declaration and the hit-test path in box_at_point honours
		 * `pointer-events: none`. fixes191e's "drop" entry is gone. */
		/* fixes191f -- user-select / overscroll-behavior etc. are
		 * silently dropped. MacSurf has no native equivalent for
		 * these; pages render fine without them. */
		"user-select",
		"-webkit-user-select",
		"-moz-user-select",
		"overscroll-behavior",
		"overscroll-behavior-x",
		"overscroll-behavior-y",
		"-webkit-overflow-scrolling",
		"-webkit-font-smoothing",
		"-moz-osx-font-smoothing",
		"-webkit-tap-highlight-color",
		"-webkit-box-orient",
		"will-change",
		"contain",
		"content-visibility",
		"scroll-margin-top",
		"scroll-margin-bottom",
		"scroll-margin-left",
		"scroll-margin-right",
		"scroll-margin",
		"scroll-padding",
		"scroll-snap-type",
		"scroll-snap-align",
		"font-display"
	};
	static const size_t N_DROP_PROPS =
			sizeof(DROP_PROPS) / sizeof(DROP_PROPS[0]);
	static const char REPEAT_LIN_FROM[] = "repeating-linear-gradient(";
	static const char REPEAT_LIN_TO[]   = "          linear-gradient(";
	static const char REPEAT_RAD_FROM[] = "repeating-radial-gradient(";
	static const char REPEAT_RAD_TO[]   = "          radial-gradient(";
	static const size_t REPEAT_LEN = 26;
	static const char FV[] = ":focus-visible";
	static const char FV_REP[] = ":focus        ";
	static const size_t FV_LEN = 14;
	static const char FW[] = ":focus-within";
	static const char FW_REP[] = ":focus       ";
	static const size_t FW_LEN = 13;
	char *out;
	size_t i, k;
	int changed = 0;

	out = (char *)malloc(in_size + 1);
	if (out == NULL) return NULL;
	memcpy(out, data, in_size);
	out[in_size] = '\0';

	/* Pass A — :focus-visible -> :focus + spaces */
	i = 0;
	while (i + FV_LEN <= in_size) {
		size_t m;
		int match = 1;
		char next;
		if (out[i] != ':') { i++; continue; }
		for (m = 0; m < FV_LEN; m++) {
			char a = out[i + m];
			char b = FV[m];
			if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
			if (a != b) { match = 0; break; }
		}
		if (!match) { i++; continue; }
		next = (i + FV_LEN < in_size) ? out[i + FV_LEN] : '\0';
		if ((next >= 'a' && next <= 'z') ||
				(next >= 'A' && next <= 'Z') ||
				(next >= '0' && next <= '9') ||
				next == '-' || next == '_') {
			i++;
			continue;
		}
		memcpy(out + i, FV_REP, FV_LEN);
		changed = 1;
		i += FV_LEN;
	}

	/* Pass B — :focus-within -> :focus + spaces */
	i = 0;
	while (i + FW_LEN <= in_size) {
		size_t m;
		int match = 1;
		char next;
		if (out[i] != ':') { i++; continue; }
		for (m = 0; m < FW_LEN; m++) {
			char a = out[i + m];
			char b = FW[m];
			if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
			if (a != b) { match = 0; break; }
		}
		if (!match) { i++; continue; }
		next = (i + FW_LEN < in_size) ? out[i + FW_LEN] : '\0';
		if ((next >= 'a' && next <= 'z') ||
				(next >= 'A' && next <= 'Z') ||
				(next >= '0' && next <= '9') ||
				next == '-' || next == '_') {
			i++;
			continue;
		}
		memcpy(out + i, FW_REP, FW_LEN);
		changed = 1;
		i += FW_LEN;
	}

	/* Pass C — declaration drops: replace `name<ws>:<value>` with spaces,
	 * keeping the terminator (`;` or `}`) intact. */
	for (k = 0; k < N_DROP_PROPS; k++) {
		const char *name = DROP_PROPS[k];
		size_t nlen = strlen(name);
		i = 0;
		while (i < in_size) {
			size_t j;
			size_t end;
			size_t p;
			if (!macsurf__match_prop_name(out, in_size, i,
					name, nlen)) {
				i++;
				continue;
			}
			j = i + nlen;
			while (j < in_size && (out[j] == ' ' ||
					out[j] == '\t' ||
					out[j] == '\n' ||
					out[j] == '\r')) j++;
			if (j >= in_size || out[j] != ':') {
				i++;
				continue;
			}
			end = j + 1;
			while (end < in_size && out[end] != ';' &&
					out[end] != '}') end++;
			for (p = i; p < end; p++) {
				if (out[p] != '\n' && out[p] != '\r') {
					out[p] = ' ';
				}
			}
			changed = 1;
			i = end;
		}
	}

	/* Pass D — repeating-*-gradient( -> linear-/radial-gradient( with
	 * leading whitespace pad. Left word-boundary check identical to
	 * macsurf__match_prop_name. */
	i = 0;
	while (i + REPEAT_LEN <= in_size) {
		int hit_lin;
		int hit_rad;
		size_t m;
		char prev;
		hit_lin = 1;
		hit_rad = 1;
		for (m = 0; m < REPEAT_LEN; m++) {
			char a = out[i + m];
			char bl = REPEAT_LIN_FROM[m];
			char br = REPEAT_RAD_FROM[m];
			if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
			if (a != bl) hit_lin = 0;
			if (a != br) hit_rad = 0;
			if (!hit_lin && !hit_rad) break;
		}
		if (!hit_lin && !hit_rad) { i++; continue; }
		if (i > 0) {
			prev = out[i - 1];
			if ((prev >= 'a' && prev <= 'z') ||
					(prev >= 'A' && prev <= 'Z') ||
					(prev >= '0' && prev <= '9') ||
					prev == '-' || prev == '_') {
				i++;
				continue;
			}
		}
		if (hit_lin) {
			memcpy(out + i, REPEAT_LIN_TO, REPEAT_LEN);
		} else {
			memcpy(out + i, REPEAT_RAD_TO, REPEAT_LEN);
		}
		changed = 1;
		i += REPEAT_LEN;
	}

	if (!changed) {
		free(out);
		return NULL;
	}
	*out_size_p = in_size;
	return out;
}


/* fixes191a — inset shorthand expander.
 *
 * Rewrite `inset: A [B [C [D]]]` declarations to the four longhands
 * `top:A; right:B; bottom:C; left:D` per CSS Logical Properties.
 * Expansion table:
 *   inset: A;         -> top:A; right:A; bottom:A; left:A;
 *   inset: A B;       -> top:A; right:B; bottom:A; left:B;
 *   inset: A B C;     -> top:A; right:B; bottom:C; left:B;
 *   inset: A B C D;   -> top:A; right:B; bottom:C; left:D;
 * Values: length, percentage, auto, calc(), var() (tokens kept verbatim).
 * `!important` on the shorthand is propagated to all four longhands.
 *
 * Output grows (longest case ~5x: "inset:0" -> 36 chars). Allocates a
 * fresh buffer; returns NULL on no-op.
 *
 * Word-boundary safe via macsurf__match_prop_name: only triggers on
 * property-position "inset", never on `border-style: inset` or
 * `box-shadow: inset ...`. */
static char *
macsurf__rewrite_inset(const char *data, size_t in_size, size_t *out_size_p)
{
	static const char NAME[] = "inset";
	static const size_t NAME_LEN = 5;
	char *out;
	size_t cap;
	size_t out_pos;
	size_t i;
	int changed = 0;

	cap = in_size + 256;
	out = (char *)malloc(cap);
	if (out == NULL) return NULL;
	out_pos = 0;
	i = 0;

	while (i < in_size) {
		size_t name_end;
		size_t val_start;
		size_t val_end;
		const char *toks[4];
		size_t tlens[4];
		int ntok;
		int important;
		size_t scan;
		size_t expanded_max;
		const char *part_v[4];
		size_t part_l[4];
		static const char *const FIELDS[4] = {
			"top:", "right:", "bottom:", "left:"
		};
		static const size_t FIELD_LENS[4] = {4, 6, 7, 5};
		int k;

		if (!macsurf__match_prop_name(data, in_size, i,
				NAME, NAME_LEN)) {
			if (out_pos + 1 >= cap) goto grow_fail;
			out[out_pos++] = data[i++];
			continue;
		}
		name_end = i + NAME_LEN;
		while (name_end < in_size && (data[name_end] == ' ' ||
				data[name_end] == '\t' ||
				data[name_end] == '\n' ||
				data[name_end] == '\r')) name_end++;
		if (name_end >= in_size || data[name_end] != ':') {
			if (out_pos + 1 >= cap) goto grow_fail;
			out[out_pos++] = data[i++];
			continue;
		}
		val_start = name_end + 1;
		val_end = val_start;
		while (val_end < in_size && data[val_end] != ';' &&
				data[val_end] != '}') val_end++;

		/* Tokenise value into up to 4 components.
		 * Whitespace splits; balanced () groups are one token. */
		ntok = 0;
		important = 0;
		scan = val_start;
		while (scan < val_end && ntok < 4) {
			size_t tok_start;
			while (scan < val_end && (data[scan] == ' ' ||
					data[scan] == '\t' ||
					data[scan] == '\n' ||
					data[scan] == '\r')) scan++;
			if (scan >= val_end) break;
			tok_start = scan;
			if (data[scan] == '!') {
				/* !important suffix -- stop tokenising. */
				important = 1;
				scan = val_end;
				break;
			}
			while (scan < val_end && data[scan] != ' ' &&
					data[scan] != '\t' &&
					data[scan] != '\n' &&
					data[scan] != '\r') {
				if (data[scan] == '(') {
					int depth = 1;
					scan++;
					while (scan < val_end && depth > 0) {
						if (data[scan] == '(') depth++;
						else if (data[scan] == ')') depth--;
						scan++;
					}
				} else {
					scan++;
				}
			}
			toks[ntok] = data + tok_start;
			tlens[ntok] = scan - tok_start;
			ntok++;
		}

		if (ntok < 1 || ntok > 4) {
			/* Malformed value -- pass through unchanged. */
			while (i < val_end) {
				if (out_pos + 1 >= cap) goto grow_fail;
				out[out_pos++] = data[i++];
			}
			continue;
		}

		/* Expand to TRBL per CSS shorthand rules. */
		switch (ntok) {
		case 1:
			part_v[0] = part_v[1] = part_v[2] = part_v[3] = toks[0];
			part_l[0] = part_l[1] = part_l[2] = part_l[3] = tlens[0];
			break;
		case 2:
			part_v[0] = toks[0]; part_l[0] = tlens[0];
			part_v[1] = toks[1]; part_l[1] = tlens[1];
			part_v[2] = toks[0]; part_l[2] = tlens[0];
			part_v[3] = toks[1]; part_l[3] = tlens[1];
			break;
		case 3:
			part_v[0] = toks[0]; part_l[0] = tlens[0];
			part_v[1] = toks[1]; part_l[1] = tlens[1];
			part_v[2] = toks[2]; part_l[2] = tlens[2];
			part_v[3] = toks[1]; part_l[3] = tlens[1];
			break;
		default:
			part_v[0] = toks[0]; part_l[0] = tlens[0];
			part_v[1] = toks[1]; part_l[1] = tlens[1];
			part_v[2] = toks[2]; part_l[2] = tlens[2];
			part_v[3] = toks[3]; part_l[3] = tlens[3];
			break;
		}

		/* Grow if needed: 4 fields, each "name:value [!important];" */
		expanded_max = 0;
		for (k = 0; k < 4; k++) {
			expanded_max += FIELD_LENS[k] + part_l[k] +
				(important ? 11 : 0) + 2;
		}
		while (out_pos + expanded_max + 8 >= cap) {
			char *grown;
			size_t newcap = cap * 2 + 256;
			grown = (char *)realloc(out, newcap);
			if (grown == NULL) goto grow_fail;
			out = grown;
			cap = newcap;
		}

		for (k = 0; k < 4; k++) {
			memcpy(out + out_pos, FIELDS[k], FIELD_LENS[k]);
			out_pos += FIELD_LENS[k];
			memcpy(out + out_pos, part_v[k], part_l[k]);
			out_pos += part_l[k];
			if (important) {
				memcpy(out + out_pos, " !important", 11);
				out_pos += 11;
			}
			if (k < 3) {
				out[out_pos++] = ';';
			}
		}
		changed = 1;
		i = val_end;  /* keep terminator (; or }) for caller. */
	}

	if (!changed) {
		free(out);
		return NULL;
	}
	*out_size_p = out_pos;
	return out;

grow_fail:
	free(out);
	return NULL;
}


/* fixes273 (Bundle I) — at-rule preprocessor for @supports and @layer.
 *
 * libcss in this vintage doesn't parse @supports or @layer rules
 * natively. Without preprocessing, author CSS that wraps modern
 * feature-detection or cascade-layer organisation around their rules
 * loses everything inside the wrapper.
 *
 * V1 strategy (zero libcss surgery):
 *
 *   @supports ( PROP : VAL ) { INNER }
 *     -> INNER (assume support, let cascade handle the inner rules;
 *        any libcss-unsupported declarations drop naturally)
 *
 *   @supports not ( PROP : VAL ) { INNER }
 *     -> (drop entirely; the block was intended for browsers WITHOUT
 *        the feature, but we DO support most modern properties)
 *
 *   @supports ( A ) and ( B ) { INNER } / @supports ( A ) or ( B ) { ... }
 *     -> INNER (conservative include for complex queries)
 *
 *   @layer NAME { INNER }     -> INNER (flatten cascade priority)
 *   @layer NAME, NAME, ... ;  -> (strip declaration-only form)
 *
 * Brace-counting scan to find matching '}'. String / comment / paren
 * contents are tracked to avoid counting braces inside them. Output
 * grows when blocks are kept, shrinks when blocks are dropped. */
static char *
macsurf__rewrite_at_rules(const char *data, size_t in_size,
		size_t *out_size_p)
{
	char *out;
	size_t cap;
	size_t out_pos = 0;
	size_t i = 0;
	int changed = 0;

	cap = in_size + 256;
	out = (char *)malloc(cap);
	if (out == NULL) return NULL;

	while (i < in_size) {
		bool is_supports = false;
		bool is_layer = false;
		size_t name_len = 0;

		/* Skip CSS comments — without this, `/* @supports ... *\/`
		 * comment text falsely triggers the preprocessor and orphans
		 * the comment opening, which libcss then treats as eating the
		 * rest of the stylesheet. */
		if (data[i] == '/' && i + 1 < in_size && data[i + 1] == '*') {
			size_t k = i + 2;
			while (k + 1 < in_size) {
				if (data[k] == '*' && data[k + 1] == '/') {
					k += 2;
					break;
				}
				k++;
			}
			if (k + 1 >= in_size) k = in_size;
			while (out_pos + (k - i) + 1 >= cap) {
				char *bigger;
				cap = cap * 2 + (k - i) + 64;
				bigger = (char *)realloc(out, cap);
				if (bigger == NULL) { free(out); return NULL; }
				out = bigger;
			}
			memcpy(out + out_pos, data + i, k - i);
			out_pos += (k - i);
			i = k;
			continue;
		}

		/* Skip CSS string literals — same reason as comments. */
		if (data[i] == '"' || data[i] == '\'') {
			char quote = data[i];
			size_t k = i + 1;
			while (k < in_size && data[k] != quote) {
				if (data[k] == '\\' && k + 1 < in_size) k += 2;
				else k++;
			}
			if (k < in_size) k++;   /* include closing quote */
			while (out_pos + (k - i) + 1 >= cap) {
				char *bigger;
				cap = cap * 2 + (k - i) + 64;
				bigger = (char *)realloc(out, cap);
				if (bigger == NULL) { free(out); return NULL; }
				out = bigger;
			}
			memcpy(out + out_pos, data + i, k - i);
			out_pos += (k - i);
			i = k;
			continue;
		}

		/* Detect @supports / @layer at this position. Must be at
		 * statement start (preceded by whitespace, ';', '{', '}',
		 * or start-of-input). */
		if (data[i] == '@' && i + 8 < in_size) {
			char prev = (i > 0) ? data[i - 1] : ' ';
			if (prev == ' ' || prev == '\t' || prev == '\n' ||
			    prev == '\r' || prev == ';' || prev == '{' ||
			    prev == '}' || i == 0) {
				if (i + 9 <= in_size &&
				    memcmp(data + i, "@supports", 9) == 0 &&
				    (data[i + 9] == ' ' || data[i + 9] == '\t' ||
				     data[i + 9] == '(' || data[i + 9] == '\n')) {
					is_supports = true;
					name_len = 9;
				} else if (i + 6 <= in_size &&
					   memcmp(data + i, "@layer", 6) == 0 &&
					   (data[i + 6] == ' ' || data[i + 6] == '\t' ||
					    data[i + 6] == ';' || data[i + 6] == '\n')) {
					is_layer = true;
					name_len = 6;
				}
			}
		}

		if (!is_supports && !is_layer) {
			if (out_pos + 1 >= cap) {
				char *bigger;
				cap = cap * 2 + 64;
				bigger = (char *)realloc(out, cap);
				if (bigger == NULL) { free(out); return NULL; }
				out = bigger;
			}
			out[out_pos++] = data[i++];
			continue;
		}

		changed = 1;

		{
			size_t j = i + name_len;
			bool has_not = false;
			size_t brace_open;
			size_t brace_close;
			int depth;
			size_t inner_start;
			size_t inner_end;
			bool keep_inner;

			/* Skip whitespace after the at-rule name. */
			while (j < in_size && (data[j] == ' ' ||
					data[j] == '\t' || data[j] == '\n' ||
					data[j] == '\r')) j++;

			/* For @supports, check for leading 'not' keyword. */
			if (is_supports && j + 3 < in_size &&
			    memcmp(data + j, "not", 3) == 0 &&
			    (data[j + 3] == ' ' || data[j + 3] == '\t' ||
			     data[j + 3] == '(')) {
				has_not = true;
				j += 3;
				while (j < in_size && (data[j] == ' ' ||
						data[j] == '\t')) j++;
			}

			/* For @layer in declaration form (ends at ';' before
			 * any '{') — strip the whole statement. */
			if (is_layer) {
				size_t scan = j;
				size_t semi = (size_t)-1;
				while (scan < in_size && data[scan] != '{' &&
						data[scan] != ';') scan++;
				if (scan < in_size && data[scan] == ';') {
					semi = scan;
					i = semi + 1;
					continue;
				}
				/* otherwise falls through to block form below */
			}

			/* Find the opening '{' that begins the inner block.
			 * Tracking parens so '(' / ')' inside the condition
			 * aren't confused. */
			brace_open = (size_t)-1;
			{
				int paren = 0;
				size_t k = j;
				while (k < in_size) {
					char c = data[k];
					if (c == '(') paren++;
					else if (c == ')') paren--;
					else if (c == '{' && paren == 0) {
						brace_open = k;
						break;
					} else if (c == ';' && paren == 0) {
						break;   /* shouldn't happen here */
					}
					k++;
				}
			}
			if (brace_open == (size_t)-1) {
				/* Malformed; skip the '@' and continue. */
				out[out_pos++] = data[i++];
				continue;
			}

			/* Find the matching close brace. */
			depth = 1;
			brace_close = (size_t)-1;
			{
				size_t k = brace_open + 1;
				while (k < in_size) {
					char c = data[k];
					if (c == '{') depth++;
					else if (c == '}') {
						depth--;
						if (depth == 0) {
							brace_close = k;
							break;
						}
					}
					k++;
				}
			}
			if (brace_close == (size_t)-1) {
				/* Unterminated; emit rest as-is. */
				while (i < in_size) {
					if (out_pos + 1 >= cap) {
						char *bigger;
						cap = cap * 2 + 64;
						bigger = (char *)realloc(out, cap);
						if (bigger == NULL) { free(out); return NULL; }
						out = bigger;
					}
					out[out_pos++] = data[i++];
				}
				break;
			}

			inner_start = brace_open + 1;
			inner_end = brace_close;

			/* Decide whether to keep the inner block. */
			if (is_supports) {
				keep_inner = !has_not;
			} else {
				keep_inner = true;   /* @layer always flattens */
			}

			if (keep_inner) {
				size_t copy_len = inner_end - inner_start;
				while (out_pos + copy_len + 1 >= cap) {
					char *bigger;
					cap = cap * 2 + copy_len + 64;
					bigger = (char *)realloc(out, cap);
					if (bigger == NULL) { free(out); return NULL; }
					out = bigger;
				}
				memcpy(out + out_pos, data + inner_start,
						copy_len);
				out_pos += copy_len;
			}

			i = brace_close + 1;
		}
	}

	if (!changed) {
		free(out);
		return NULL;
	}

	*out_size_p = out_pos;
	return out;
}


/* fixes274 (Bundle C, #25 + #64) — grid alignment shorthand expansion +
 * justify-items / justify-self bridge.
 *
 * libcss in this vintage does not expose justify-items / justify-self
 * accessors, and there's no `place-items` / `place-content` shorthand
 * support. fixes270 wired the four properties libcss DOES expose
 * (justify-content, align-content, align-items, align-self). This pass
 * extends authoring-side support by:
 *
 *   1. Splitting `place-items: A B` into `align-items: A; justify-items: B`
 *      (or both = A when only one value).
 *   2. Splitting `place-content: A B` into `align-content: A;
 *      justify-content: B`.
 *   3. Shadowing `justify-items: VAL` declarations with an additional
 *      `text-align: VAL` declaration. text-align inherits to grid cells'
 *      inline content, producing visually-correct cell horizontal
 *      alignment for the typical-case (cell content is text or inline
 *      content that text-align governs).
 *   4. Shadowing `justify-self: VAL` similarly with `text-align: VAL`.
 *
 * Spec-loose: real justify-items affects ONLY cell positioning within the
 * track, not inline content alignment. For pure-text cells they look the
 * same; for cells with mixed content (image + text) the visual may differ.
 * Acceptable V1 — the alternative is libcss surgery, which is the trap
 * zone documented in project memory.
 *
 * Each pass-through can grow the output. We malloc a working buffer and
 * append. */

/* CSS-keyword-safe match: confirm the position contains exactly the given
 * property name, not as a substring of a longer identifier. */
static bool
macsurf__cc_prop_at(const char *buf, size_t len, size_t pos,
		const char *name, size_t name_len)
{
	size_t i;
	if (pos + name_len > len) return false;
	if (pos > 0) {
		char prev = buf[pos - 1];
		if ((prev >= 'a' && prev <= 'z') ||
				(prev >= 'A' && prev <= 'Z') ||
				(prev >= '0' && prev <= '9') ||
				prev == '-' || prev == '_') {
			return false;
		}
	}
	for (i = 0; i < name_len; i++) {
		char b = buf[pos + i];
		char n = name[i];
		if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
		if (n >= 'A' && n <= 'Z') n = (char)(n - 'A' + 'a');
		if (b != n) return false;
	}
	return true;
}

/* Find the byte index of the next ';' or '}' at brace-depth zero from
 * position `start`. Skips chars inside parens / strings / comments to
 * avoid confusing those with declaration boundaries. */
static size_t
macsurf__cc_find_decl_end(const char *buf, size_t len, size_t start)
{
	size_t k = start;
	int paren = 0;
	while (k < len) {
		char c = buf[k];
		if (c == '/' && k + 1 < len && buf[k + 1] == '*') {
			k += 2;
			while (k + 1 < len &&
					!(buf[k] == '*' && buf[k + 1] == '/'))
				k++;
			if (k + 1 < len) k += 2;
			continue;
		}
		if (c == '"' || c == '\'') {
			char q = c;
			k++;
			while (k < len && buf[k] != q) {
				if (buf[k] == '\\' && k + 1 < len) k += 2;
				else k++;
			}
			if (k < len) k++;
			continue;
		}
		if (c == '(') paren++;
		else if (c == ')') paren--;
		else if (paren == 0 && (c == ';' || c == '}'))
			return k;
		k++;
	}
	return len;
}

/* Trim whitespace inside [from, to). Returns the new bounds via outs. */
static void
macsurf__cc_trim(const char *buf, size_t from, size_t to,
		size_t *out_from, size_t *out_to)
{
	while (from < to && (buf[from] == ' ' || buf[from] == '\t' ||
			buf[from] == '\n' || buf[from] == '\r')) from++;
	while (to > from && (buf[to - 1] == ' ' || buf[to - 1] == '\t' ||
			buf[to - 1] == '\n' || buf[to - 1] == '\r')) to--;
	*out_from = from;
	*out_to = to;
}

static char *
macsurf__rewrite_grid_alignment(const char *data, size_t in_size,
		size_t *out_size_p)
{
	char *out;
	size_t cap;
	size_t out_pos = 0;
	size_t i = 0;
	int changed = 0;

	cap = in_size * 2 + 256;   /* shadowed declarations can ~double size */
	out = (char *)malloc(cap);
	if (out == NULL) return NULL;

	while (i < in_size) {
		size_t colon;
		size_t value_start;
		size_t value_end;
		size_t end;
		size_t v1_start;
		size_t v1_end;
		size_t v2_start;
		size_t v2_end;
		size_t k;
		const char *match_prop = NULL;
		const char *align_axis = NULL;
		const char *justify_axis = NULL;

		/* Comment + string pass-through (same pattern as
		 * macsurf__rewrite_at_rules). */
		if (data[i] == '/' && i + 1 < in_size && data[i + 1] == '*') {
			size_t kk = i + 2;
			while (kk + 1 < in_size) {
				if (data[kk] == '*' && data[kk + 1] == '/') {
					kk += 2;
					break;
				}
				kk++;
			}
			if (kk + 1 >= in_size) kk = in_size;
			while (out_pos + (kk - i) + 1 >= cap) {
				char *bigger;
				cap = cap * 2 + (kk - i) + 64;
				bigger = (char *)realloc(out, cap);
				if (bigger == NULL) { free(out); return NULL; }
				out = bigger;
			}
			memcpy(out + out_pos, data + i, kk - i);
			out_pos += (kk - i);
			i = kk;
			continue;
		}
		if (data[i] == '"' || data[i] == '\'') {
			char quote = data[i];
			size_t kk = i + 1;
			while (kk < in_size && data[kk] != quote) {
				if (data[kk] == '\\' && kk + 1 < in_size) kk += 2;
				else kk++;
			}
			if (kk < in_size) kk++;
			while (out_pos + (kk - i) + 1 >= cap) {
				char *bigger;
				cap = cap * 2 + (kk - i) + 64;
				bigger = (char *)realloc(out, cap);
				if (bigger == NULL) { free(out); return NULL; }
				out = bigger;
			}
			memcpy(out + out_pos, data + i, kk - i);
			out_pos += (kk - i);
			i = kk;
			continue;
		}

		/* Detect one of the four shorthand / longhand patterns. */
		if (macsurf__cc_prop_at(data, in_size, i, "place-items", 11)) {
			match_prop = "place-items";
			align_axis = "align-items";
			justify_axis = "justify-items";
			k = i + 11;
		} else if (macsurf__cc_prop_at(data, in_size, i,
				"place-content", 13)) {
			match_prop = "place-content";
			align_axis = "align-content";
			justify_axis = "justify-content";
			k = i + 13;
		} else if (macsurf__cc_prop_at(data, in_size, i,
				"justify-items", 13)) {
			match_prop = "justify-items";
			align_axis = NULL;
			justify_axis = "justify-items";
			k = i + 13;
		} else if (macsurf__cc_prop_at(data, in_size, i,
				"justify-self", 12)) {
			match_prop = "justify-self";
			align_axis = NULL;
			justify_axis = "justify-self";
			k = i + 12;
		} else {
			match_prop = NULL;
		}

		if (match_prop == NULL) {
			if (out_pos + 1 >= cap) {
				char *bigger;
				cap = cap * 2 + 64;
				bigger = (char *)realloc(out, cap);
				if (bigger == NULL) { free(out); return NULL; }
				out = bigger;
			}
			out[out_pos++] = data[i++];
			continue;
		}

		/* Walk past whitespace, expect ':'. */
		while (k < in_size && (data[k] == ' ' || data[k] == '\t' ||
				data[k] == '\n' || data[k] == '\r')) k++;
		if (k >= in_size || data[k] != ':') {
			/* Not a real property declaration; emit char and skip. */
			out[out_pos++] = data[i++];
			continue;
		}
		colon = k;
		value_start = k + 1;
		end = macsurf__cc_find_decl_end(data, in_size, value_start);
		value_end = end;

		/* For place-* shorthand, split values; for justify-* longhand,
		 * use the whole value verbatim. */
		macsurf__cc_trim(data, value_start, value_end,
				&value_start, &value_end);
		if (align_axis != NULL) {
			/* place-items or place-content. */
			size_t split = value_start;
			while (split < value_end && data[split] != ' ' &&
					data[split] != '\t') split++;
			if (split < value_end) {
				v1_start = value_start;
				v1_end = split;
				v2_start = split;
				while (v2_start < value_end &&
						(data[v2_start] == ' ' ||
						 data[v2_start] == '\t'))
					v2_start++;
				v2_end = value_end;
			} else {
				v1_start = value_start;
				v1_end = value_end;
				v2_start = value_start;
				v2_end = value_end;
			}
		} else {
			v1_start = v2_start = value_start;
			v1_end = v2_end = value_end;
		}

		changed = 1;

		/* Emit replacement. For shorthand: align-axis: V1; justify-axis: V2;
		 * with text-align: V2 shadow when justify_axis is justify-items.
		 * For longhand: keep original + text-align: V shadow. */
		{
			char tmp[256];
			size_t tlen;
			size_t need;

			tlen = 0;
			if (align_axis != NULL) {
				/* place-* shorthand */
				/* emit align-axis declaration */
				memcpy(tmp + tlen, align_axis, strlen(align_axis));
				tlen += strlen(align_axis);
				tmp[tlen++] = ':';
				tmp[tlen++] = ' ';
				need = v1_end - v1_start;
				if (tlen + need + 2 >= sizeof tmp) need = sizeof tmp - tlen - 2;
				memcpy(tmp + tlen, data + v1_start, need);
				tlen += need;
				tmp[tlen++] = ';';
				tmp[tlen++] = ' ';
			}
			/* emit justify-axis declaration */
			memcpy(tmp + tlen, justify_axis, strlen(justify_axis));
			tlen += strlen(justify_axis);
			tmp[tlen++] = ':';
			tmp[tlen++] = ' ';
			need = v2_end - v2_start;
			if (tlen + need + 2 >= sizeof tmp) need = sizeof tmp - tlen - 2;
			memcpy(tmp + tlen, data + v2_start, need);
			tlen += need;

			/* Shadow with text-align for justify-items / justify-self
			 * paths only (so visual cell-content alignment works).
			 * fixes274e: libcss's text-align parser does not accept
			 * CSS3 logical values (start / end). Map them to physical
			 * keywords here (start/flex-start/self-start -> left,
			 * end/flex-end/self-end -> right). Skip the shadow for
			 * stretch / baseline / normal / auto where there's no
			 * sensible text-align equivalent. */
			if (justify_axis != NULL && (
					strcmp(justify_axis, "justify-items") == 0 ||
					strcmp(justify_axis, "justify-self") == 0)) {
				size_t vlen = v2_end - v2_start;
				const char *src = data + v2_start;
				const char *out_kw = NULL;
				size_t out_kw_len = 0;
				if ((vlen == 6 && memcmp(src, "center", 6) == 0) ||
						(vlen == 4 && memcmp(src, "left", 4) == 0) ||
						(vlen == 5 && memcmp(src, "right", 5) == 0) ||
						(vlen == 7 && memcmp(src, "justify", 7) == 0)) {
					out_kw = src;
					out_kw_len = vlen;
				} else if ((vlen == 5 && memcmp(src, "start", 5) == 0) ||
						(vlen == 10 && memcmp(src, "flex-start", 10) == 0) ||
						(vlen == 10 && memcmp(src, "self-start", 10) == 0)) {
					out_kw = "left";
					out_kw_len = 4;
				} else if ((vlen == 3 && memcmp(src, "end", 3) == 0) ||
						(vlen == 8 && memcmp(src, "flex-end", 8) == 0) ||
						(vlen == 8 && memcmp(src, "self-end", 8) == 0)) {
					out_kw = "right";
					out_kw_len = 5;
				}
				if (out_kw != NULL) {
					tmp[tlen++] = ';';
					tmp[tlen++] = ' ';
					memcpy(tmp + tlen, "text-align: ", 12);
					tlen += 12;
					if (tlen + out_kw_len + 2 >= sizeof tmp)
						out_kw_len = sizeof tmp - tlen - 2;
					memcpy(tmp + tlen, out_kw, out_kw_len);
					tlen += out_kw_len;
				}
			}

			while (out_pos + tlen + 1 >= cap) {
				char *bigger;
				cap = cap * 2 + tlen + 64;
				bigger = (char *)realloc(out, cap);
				if (bigger == NULL) { free(out); return NULL; }
				out = bigger;
			}
			memcpy(out + out_pos, tmp, tlen);
			out_pos += tlen;
		}

		/* Skip past the original declaration up to the terminator. */
		i = end;
	}

	if (!changed) {
		free(out);
		return NULL;
	}
	*out_size_p = out_pos;
	return out;
}


/* fixes275 (#65) — grid-auto-flow preprocessor.
 *
 * Rewrites standard CSS `grid-auto-flow: VALUE` declarations to
 * `-macsurf-grid-flow: N` integer values for the vendor property:
 *
 *   row              → 1
 *   column           → 2
 *   row dense        → 3  (also bare `dense` per CSS spec default-axis = row)
 *   dense row        → 3
 *   column dense     → 4
 *   dense column     → 4
 *
 * Standard CSS lets the two-keyword form appear in either order. We
 * tokenize and dispatch on the keyword set.
 *
 * Output grows: source ~14 bytes ("grid-auto-flow"), output ~22 bytes
 * ("-macsurf-grid-flow: N"). Allocates fresh buffer. */
static char *
macsurf__rewrite_grid_auto_flow(const char *data, size_t in_size,
		size_t *out_size_p)
{
	char *out;
	size_t cap;
	size_t out_pos = 0;
	size_t i = 0;
	int changed = 0;

	cap = in_size + 256;
	out = (char *)malloc(cap);
	if (out == NULL) return NULL;

	while (i < in_size) {
		size_t k;
		size_t end;
		size_t v_start;
		size_t v_end;
		bool has_row;
		bool has_column;
		bool has_dense;
		int flow_val;
		char emit[40];
		size_t emit_len;

		/* Comment pass-through. */
		if (data[i] == '/' && i + 1 < in_size && data[i + 1] == '*') {
			size_t kk = i + 2;
			while (kk + 1 < in_size) {
				if (data[kk] == '*' && data[kk + 1] == '/') {
					kk += 2;
					break;
				}
				kk++;
			}
			if (kk + 1 >= in_size) kk = in_size;
			while (out_pos + (kk - i) + 1 >= cap) {
				char *bigger;
				cap = cap * 2 + (kk - i) + 64;
				bigger = (char *)realloc(out, cap);
				if (bigger == NULL) { free(out); return NULL; }
				out = bigger;
			}
			memcpy(out + out_pos, data + i, kk - i);
			out_pos += (kk - i);
			i = kk;
			continue;
		}
		/* String pass-through. */
		if (data[i] == '"' || data[i] == '\'') {
			char quote = data[i];
			size_t kk = i + 1;
			while (kk < in_size && data[kk] != quote) {
				if (data[kk] == '\\' && kk + 1 < in_size) kk += 2;
				else kk++;
			}
			if (kk < in_size) kk++;
			while (out_pos + (kk - i) + 1 >= cap) {
				char *bigger;
				cap = cap * 2 + (kk - i) + 64;
				bigger = (char *)realloc(out, cap);
				if (bigger == NULL) { free(out); return NULL; }
				out = bigger;
			}
			memcpy(out + out_pos, data + i, kk - i);
			out_pos += (kk - i);
			i = kk;
			continue;
		}

		if (!macsurf__cc_prop_at(data, in_size, i,
				"grid-auto-flow", 14)) {
			if (out_pos + 1 >= cap) {
				char *bigger;
				cap = cap * 2 + 64;
				bigger = (char *)realloc(out, cap);
				if (bigger == NULL) { free(out); return NULL; }
				out = bigger;
			}
			out[out_pos++] = data[i++];
			continue;
		}

		k = i + 14;
		while (k < in_size && (data[k] == ' ' || data[k] == '\t' ||
				data[k] == '\n' || data[k] == '\r')) k++;
		if (k >= in_size || data[k] != ':') {
			out[out_pos++] = data[i++];
			continue;
		}
		v_start = k + 1;
		end = macsurf__cc_find_decl_end(data, in_size, v_start);
		v_end = end;
		macsurf__cc_trim(data, v_start, v_end, &v_start, &v_end);

		/* Tokenise the value into row / column / dense detection. */
		has_row = false;
		has_column = false;
		has_dense = false;
		{
			size_t p = v_start;
			while (p < v_end) {
				size_t tok_start;
				size_t tok_end;
				size_t tlen;
				while (p < v_end && (data[p] == ' ' ||
						data[p] == '\t')) p++;
				tok_start = p;
				while (p < v_end && data[p] != ' ' &&
						data[p] != '\t') p++;
				tok_end = p;
				tlen = tok_end - tok_start;
				if (tlen == 3 && memcmp(data + tok_start,
						"row", 3) == 0) {
					has_row = true;
				} else if (tlen == 6 && memcmp(data + tok_start,
						"column", 6) == 0) {
					has_column = true;
				} else if (tlen == 5 && memcmp(data + tok_start,
						"dense", 5) == 0) {
					has_dense = true;
				}
			}
		}

		if (has_column && has_dense) {
			flow_val = 4;
		} else if (has_column) {
			flow_val = 2;
		} else if (has_dense) {
			flow_val = 3;
		} else if (has_row) {
			flow_val = 1;
		} else {
			/* No recognised keyword — drop and emit nothing. */
			i = end;
			changed = 1;
			continue;
		}

		emit_len = (size_t)sprintf(emit,
				"-macsurf-grid-flow: %d", flow_val);
		while (out_pos + emit_len + 1 >= cap) {
			char *bigger;
			cap = cap * 2 + emit_len + 64;
			bigger = (char *)realloc(out, cap);
			if (bigger == NULL) { free(out); return NULL; }
			out = bigger;
		}
		memcpy(out + out_pos, emit, emit_len);
		out_pos += emit_len;

		changed = 1;
		i = end;
	}

	if (!changed) {
		free(out);
		return NULL;
	}
	*out_size_p = out_pos;
	return out;
}


/* fixes277 (#61) — logical properties → physical aliases (LTR-only V1).
 *
 * CSS Logical Properties Level 1 lets authors write direction-agnostic
 * declarations:
 *   inline-size, block-size
 *   padding-inline-{start,end}, padding-block-{start,end}
 *   margin-inline-{start,end},  margin-block-{start,end}
 *   inset-inline-{start,end},   inset-block-{start,end}
 *   border-inline-{start,end},  border-block-{start,end}
 *
 * In LTR mode (the only writing mode MacSurf supports), these map 1:1 to
 * the physical properties below. We rewrite the property name in the
 * source before libcss sees it.
 *
 * All physical names are SHORTER than their logical equivalents, so this
 * is an in-place rewrite (replace name + pad with trailing spaces inside
 * the original declaration footprint).
 *
 * Limitations: this is LTR-only. When MacSurf gains RTL support, swap
 * start/end for inline-axis writes. block-axis aliases are direction-
 * agnostic and stay the same in RTL. */
static const struct {
	const char *logical;
	size_t llen;
	const char *physical;
	size_t plen;
} macsurf__logical_aliases[] = {
	/* Order: longest first so substring matches don't fire (e.g.
	 * "padding-inline-start" before "padding-inline"). */
	{ "padding-inline-start", 20, "padding-left",     12 },
	{ "padding-inline-end",   18, "padding-right",    13 },
	{ "padding-block-start",  19, "padding-top",      11 },
	{ "padding-block-end",    17, "padding-bottom",   14 },
	{ "margin-inline-start",  19, "margin-left",      11 },
	{ "margin-inline-end",    17, "margin-right",     12 },
	{ "margin-block-start",   18, "margin-top",       10 },
	{ "margin-block-end",     16, "margin-bottom",    13 },
	{ "border-inline-start",  19, "border-left",      11 },
	{ "border-inline-end",    17, "border-right",     12 },
	{ "border-block-start",   18, "border-top",       10 },
	{ "border-block-end",     16, "border-bottom",    13 },
	{ "inset-inline-start",   18, "left",              4 },
	{ "inset-inline-end",     16, "right",             5 },
	{ "inset-block-start",    17, "top",               3 },
	{ "inset-block-end",      15, "bottom",            6 },
	{ "inline-size",          11, "width",             5 },
	{ "block-size",           10, "height",            6 },
	{ NULL, 0, NULL, 0 }
};

static char *
macsurf__rewrite_logical_properties(const char *data, size_t size)
{
	char *out;
	size_t i;
	int changed = 0;

	out = (char *)malloc(size);
	if (out == NULL) return NULL;
	memcpy(out, data, size);

	i = 0;
	while (i < size) {
		size_t a;
		bool matched = false;

		for (a = 0; macsurf__logical_aliases[a].logical != NULL; a++) {
			const char *lname =
				macsurf__logical_aliases[a].logical;
			size_t llen = macsurf__logical_aliases[a].llen;
			const char *pname =
				macsurf__logical_aliases[a].physical;
			size_t plen = macsurf__logical_aliases[a].plen;

			if (!macsurf__match_prop_name(out, size, i,
					lname, llen)) {
				continue;
			}
			/* Replace name in place, pad with spaces. */
			memcpy(out + i, pname, plen);
			memset(out + i + plen, ' ', llen - plen);
			i += llen;
			matched = true;
			changed = 1;
			break;
		}
		if (!matched) i++;
	}

	if (!changed) {
		free(out);
		return NULL;
	}
	return out;
}


/* fixes202 — inline-style preprocessor.
 *
 * External stylesheets and <style> blocks run through nscss_process_data
 * below, which threads the source bytes through a chain of name-rewriting
 * passes (fixes175 text-shadow, fixes183 transform, fixes191g
 * object-position, etc). Inline `style="..."` attributes take a different
 * path: box_construct calls nscss_create_inline_style which hands the
 * bytes straight to libcss with no preprocessing. Result: inline
 * declarations like `transform: rotate(30deg)` or `text-shadow: 2px 2px`
 * are unknown to libcss and silently dropped — TS1/TC1-3 on advanced.html
 * shipped at fixes201b rendered as plain boxes with no shadow / no
 * rotation / no translation / no scale.
 *
 * This helper runs the inline-style-relevant rewrite passes against a
 * buffer and returns a freshly-allocated, length-prefixed result. The
 * caller frees the result with free(). When no rewrites apply, the
 * helper returns NULL and *out_size_p is unchanged; the caller should
 * use the original buffer in that case.
 *
 * Grid template / inset / modern-compat passes are deliberately
 * omitted — they target stylesheet patterns (selectors, multi-line
 * declarations) that don't appear in inline-style attributes. Adding
 * them is cheap if a future inline declaration needs them. */
char *macsurf__rewrite_inline_style(const char *data, size_t in_size,
		size_t *out_size_p);

char *macsurf__rewrite_inline_style(const char *data, size_t in_size,
		size_t *out_size_p)
{
	char *cur = NULL;
	size_t cur_size = in_size;
	const char *src = data;
	size_t src_size = in_size;
	char *next;
	size_t next_size = 0;

	if (data == NULL || in_size == 0 || out_size_p == NULL) {
		return NULL;
	}

	next = macsurf__rewrite_text_shadow(src, src_size, &next_size);
	if (next != NULL) {
		if (cur != NULL) free(cur);
		cur = next;
		cur_size = next_size;
		src = (const char *)cur;
		src_size = cur_size;
	}

	next = macsurf__rewrite_transform(src, src_size, &next_size);
	if (next != NULL) {
		if (cur != NULL) free(cur);
		cur = next;
		cur_size = next_size;
		src = (const char *)cur;
		src_size = cur_size;
	}

	next = macsurf__rewrite_object_position(src, src_size, &next_size);
	if (next != NULL) {
		if (cur != NULL) free(cur);
		cur = next;
		cur_size = next_size;
		src = (const char *)cur;
		src_size = cur_size;
	}

	next = macsurf__rewrite_background_image_gradient(src, src_size,
			&next_size);
	if (next != NULL) {
		if (cur != NULL) free(cur);
		cur = next;
		cur_size = next_size;
		src = (const char *)cur;
		src_size = cur_size;
	}

	if (cur == NULL) {
		return NULL;
	}
	*out_size_p = cur_size;
	return cur;
}


static bool
nscss_process_data(struct content *c, const char *data, unsigned int size)
{
	nscss_content *css = (nscss_content *) c;
	css_error error;
	char *rewritten;
	char *rewritten_rows;
	char *rewritten_col_span = NULL;
	char *rewritten_text_shadow = NULL;
	char *rewritten_transform = NULL;
	char *rewritten_object_position = NULL;
	char *rewritten_modern_compat = NULL;
	char *rewritten_inset = NULL;
	char *rewritten_at_rules = NULL;   /* fixes273 — @supports/@layer */
	char *rewritten_grid_align = NULL; /* fixes274 grid alignment */
	char *rewritten_grid_flow = NULL;  /* fixes275 grid-auto-flow */
	char *rewritten_logical = NULL;    /* fixes277 logical properties */
	char *rewritten_calc = NULL;       /* fixes280 calc() arithmetic */
	char *rewritten_rep_grad = NULL;   /* fixes281 repeating-gradient -> solid */
	size_t col_span_size = 0;
	size_t text_shadow_size = 0;
	size_t transform_size = 0;
	size_t object_position_size = 0;
	size_t modern_compat_size = 0;
	size_t inset_size = 0;
	const char *final_data;
	unsigned int final_size;

	MS_LOG("nscss process data");

	/* fixes160d — oversize CSS gate. Accumulate bytes per sheet; when
	 * past MACOS9_CSS_MAX_BYTES, latch the skip flag, broadcast a CSS
	 * error so NetSurf drops this sheet from the cascade, and refuse
	 * further data without feeding it to libcss. Subsequent calls
	 * exit on the latch alone (no further log spam, no double-count). */
	if (css->skipped) {
		return false;
	}
	css->total_bytes += (unsigned long)size;
	if (css->total_bytes > MACOS9_CSS_MAX_BYTES) {
		extern long macsurf__site_css_skip;
		css->skipped = 1;
		macsurf__site_css_skip++;
		macsurf_debug_log_writef(
			"css skip: oversize bytes=%ld cap=%ld - sheet dropped",
			(long)css->total_bytes, (long)MACOS9_CSS_MAX_BYTES);
		content_broadcast_error(c, NSERROR_CSS, NULL);
		return false;
	}

	/* fixes268 (#9) — total CSS budget across all sheets per page.
	 * The per-sheet cap above protects against a single oversize sheet;
	 * this guard caps cumulative bytes so a stack of 20+ Drupal vendor
	 * sheets doesn't exhaust libcss memory. Document order wins — the
	 * site's first stylesheet (typically main) is fully processed and
	 * later (vendor / module) sheets short-circuit immediately when
	 * the global cap is hit. Counter is reset per page in html_create
	 * via the existing fixes160a SITE-counters reset. */
	{
		extern unsigned long macsurf__site_css_total_bytes;
		const unsigned long total_cap = MACOS9_CSS_TOTAL_BUDGET;
		macsurf__site_css_total_bytes += (unsigned long)size;
		if (macsurf__site_css_total_bytes > total_cap) {
			extern long macsurf__site_css_skip;
			css->skipped = 1;
			macsurf__site_css_skip++;
			macsurf_debug_log_writef(
				"css skip: total budget bytes=%ld cap=%ld - sheet dropped",
				(long)macsurf__site_css_total_bytes,
				(long)total_cap);
			content_broadcast_error(c, NSERROR_CSS, NULL);
			return false;
		}
	}

	/* fixes273 (Bundle I) — earliest pass: unwrap / drop @supports
	 * and @layer wrappers since libcss in this vintage doesn't parse
	 * them natively. Inner rules then flow through the rest of the
	 * preprocessing chain normally. */
	{
		size_t at_size = 0;
		rewritten_at_rules = macsurf__rewrite_at_rules(data,
				(size_t)size, &at_size);
		if (rewritten_at_rules != NULL &&
				at_size <= (size_t)0x7fffffff) {
			data = (const char *)rewritten_at_rules;
			size = (unsigned int)at_size;
		}
	}

	/* fixes274 (Bundle C) — grid alignment shorthand expansion +
	 * justify-items / justify-self → text-align bridge. Splits
	 * place-items / place-content shorthands and shadows the
	 * justify-* longhands with text-align so cell content aligns
	 * visually. Runs early so subsequent property-rewrite passes
	 * see the expanded declarations. */
	{
		size_t ga_size = 0;
		rewritten_grid_align = macsurf__rewrite_grid_alignment(data,
				(size_t)size, &ga_size);
		if (rewritten_grid_align != NULL &&
				ga_size <= (size_t)0x7fffffff) {
			data = (const char *)rewritten_grid_align;
			size = (unsigned int)ga_size;
		}
	}

	/* fixes275 (#65) — grid-auto-flow → -macsurf-grid-flow rewrite. */
	{
		size_t gf_size = 0;
		rewritten_grid_flow = macsurf__rewrite_grid_auto_flow(data,
				(size_t)size, &gf_size);
		if (rewritten_grid_flow != NULL &&
				gf_size <= (size_t)0x7fffffff) {
			data = (const char *)rewritten_grid_flow;
			size = (unsigned int)gf_size;
		}
	}

	/* fixes277 (#61) — logical → physical property aliases (LTR). */
	rewritten_logical = macsurf__rewrite_logical_properties(data,
			(size_t)size);
	if (rewritten_logical != NULL) {
		data = (const char *)rewritten_logical;
		/* In-place rewrite: size unchanged. */
	}

	/* fixes280 — fold calc() arithmetic for simple aspect-ratio
	 * padding-hack patterns: calc(N / M * 100%) -> N*100/M %. */
	rewritten_calc = macsurf__rewrite_calc_aspect(data, (size_t)size);
	if (rewritten_calc != NULL) {
		data = (const char *)rewritten_calc;
		/* In-place same-size rewrite. */
	}

	/* fixes281 — collapse repeating-{linear,radial}-gradient(...) to
	 * a solid platinum-grey #dddddd. Visible fallback for striped
	 * title-bar backgrounds. */
	rewritten_rep_grad = macsurf__rewrite_repeating_gradient_solid(
			data, (size_t)size);
	if (rewritten_rep_grad != NULL) {
		data = (const char *)rewritten_rep_grad;
		/* In-place same-size rewrite. */
	}

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

	/* fixes178b — between rows and placement: resolve
	 * grid-template-areas + named grid-area shorthand into the
	 * numeric 4-value grid-area form that the placement pass below
	 * already handles. Pass-through on no-op (no template-areas
	 * declarations in the input). */
	{
		size_t gta_size = 0;
		char *rewritten_gta = macsurf__rewrite_grid_template_areas(
				final_data, (size_t)final_size, &gta_size);
		if (rewritten_gta != NULL && gta_size <= (size_t)0x7fffffff) {
			if (rewritten_rows != NULL) {
				free(rewritten_rows);
				rewritten_rows = NULL;
			}
			if (rewritten != NULL) {
				free(rewritten);
				rewritten = NULL;
			}
			rewritten_rows = rewritten_gta;
			final_data = (const char *)rewritten_gta;
			final_size = (unsigned int)gta_size;
		} else if (rewritten_gta != NULL) {
			free(rewritten_gta);
		}
	}

	/* fixes151 — third pass for `grid-column: VALUE`. This pass needs
	 * a growable output buffer (new property name is longer than the
	 * source), so it allocates fresh and returns the new size. */
	rewritten_col_span = macsurf__rewrite_grid_column(final_data,
			(size_t)final_size, &col_span_size);
	if (rewritten_col_span != NULL && col_span_size <= (size_t)0x7fffffff) {
		final_data = (const char *)rewritten_col_span;
		final_size = (unsigned int)col_span_size;
	}

	/* fixes175 — fourth pass: rewrite standard `text-shadow:` to the
	 * vendor `-macsurf-text-shadow:` so author CSS uses the bound
	 * paint path. Allocates a new growable buffer. */
	rewritten_text_shadow = macsurf__rewrite_text_shadow(final_data,
			(size_t)final_size, &text_shadow_size);
	if (rewritten_text_shadow != NULL &&
			text_shadow_size <= (size_t)0x7fffffff) {
		final_data = (const char *)rewritten_text_shadow;
		final_size = (unsigned int)text_shadow_size;
	}

	/* fixes183 — fifth pass: rewrite standard `transform:` to the
	 * vendor `-macsurf-transform:` so author CSS reaches the existing
	 * paint path. */
	rewritten_transform = macsurf__rewrite_transform(final_data,
			(size_t)final_size, &transform_size);
	if (rewritten_transform != NULL &&
			transform_size <= (size_t)0x7fffffff) {
		final_data = (const char *)rewritten_transform;
		final_size = (unsigned int)transform_size;
	}

	/* fixes191g — sixth pass: rewrite standard `object-position:` to
	 * `-macsurf-object-position:` so the keyword-only V1 parser can
	 * steer replaced-element alignment without touching libcss struct
	 * layout. */
	rewritten_object_position = macsurf__rewrite_object_position(final_data,
			(size_t)final_size, &object_position_size);
	if (rewritten_object_position != NULL &&
			object_position_size <= (size_t)0x7fffffff) {
		final_data = (const char *)rewritten_object_position;
		final_size = (unsigned int)object_position_size;
	}

	/* fixes185 — seventh pass: modern-CSS compatibility. Rewrites
	 * `:focus-visible` / `:focus-within` to `:focus`, drops unsupported
	 * declarations (line-clamp, image-rendering, font-variant-numeric,
	 * break-inside, outline-offset), and strips the `repeating-` prefix
	 * from `repeating-linear/radial-gradient(` so the first cycle still
	 * renders through the existing gradient handler. In-place,
	 * length-preserving; returns NULL when no rewrites apply. */
	rewritten_modern_compat = macsurf__rewrite_modern_compat(final_data,
			(size_t)final_size, &modern_compat_size);
	if (rewritten_modern_compat != NULL &&
			modern_compat_size <= (size_t)0x7fffffff) {
		final_data = (const char *)rewritten_modern_compat;
		final_size = (unsigned int)modern_compat_size;
	}

	/* fixes191a — inset shorthand expansion to top/right/bottom/left.
	 * Allocates a growable buffer (output is up to ~5x input for the
	 * matched declarations). Pass-through on no-op. */
	rewritten_inset = macsurf__rewrite_inset(final_data,
			(size_t)final_size, &inset_size);
	if (rewritten_inset != NULL &&
			inset_size <= (size_t)0x7fffffff) {
		final_data = (const char *)rewritten_inset;
		final_size = (unsigned int)inset_size;
	}

	/* fixes266 — background-image: linear-gradient(...) → -macsurf-gradient:
	 * linear-gradient(...) so author CSS reaches the existing gradient
	 * paint path. Net +1 byte per match. */
	{
		size_t bg_grad_size = 0;
		char *rewritten_bg_grad = macsurf__rewrite_background_image_gradient(
				final_data, (size_t)final_size, &bg_grad_size);
		char *rewritten_strip = NULL;
		if (rewritten_bg_grad != NULL &&
				bg_grad_size <= (size_t)0x7fffffff) {
			final_data = (const char *)rewritten_bg_grad;
			final_size = (unsigned int)bg_grad_size;
		}

		/* fixes279 (#27) — strip stacked gradients (everything past
		 * the first depth-0 comma in any -macsurf-gradient value).
		 * Same-size in-place rewrite; only spaces introduced. */
		rewritten_strip = macsurf__strip_stacked_gradients(
				final_data, (size_t)final_size);
		if (rewritten_strip != NULL) {
			final_data = (const char *)rewritten_strip;
			/* size unchanged */
		}

		error = nscss_process_css_data(&css->data, final_data, final_size);

		if (rewritten_strip != NULL) free(rewritten_strip);
		if (rewritten_bg_grad != NULL) free(rewritten_bg_grad);
	}

	if (rewritten_inset != NULL) free(rewritten_inset);
	if (rewritten_modern_compat != NULL) free(rewritten_modern_compat);
	if (rewritten_object_position != NULL) free(rewritten_object_position);
	if (rewritten_transform != NULL) free(rewritten_transform);
	if (rewritten_text_shadow != NULL) free(rewritten_text_shadow);
	if (rewritten_col_span != NULL) free(rewritten_col_span);
	if (rewritten_rows != NULL) free(rewritten_rows);
	if (rewritten != NULL) free(rewritten);
	if (rewritten_at_rules != NULL) free(rewritten_at_rules);   /* fixes273 */
	if (rewritten_grid_align != NULL) free(rewritten_grid_align); /* fixes274 */
	if (rewritten_grid_flow != NULL) free(rewritten_grid_flow); /* fixes275 */
	if (rewritten_logical != NULL) free(rewritten_logical); /* fixes277 */
	if (rewritten_calc != NULL) free(rewritten_calc); /* fixes280 */
	if (rewritten_rep_grad != NULL) free(rewritten_rep_grad); /* fixes281 */

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

	/* fixes160d — sheets dropped by the oversize gate already broadcast
	 * an error in nscss_process_data; bail without trying to convert
	 * (the libcss sheet was never fed any data after the latch). */
	if (css->skipped) {
		return false;
	}

	error = nscss_convert_css_data(&css->data);
	if (error != CSS_OK) {
		content_broadcast_error(c, NSERROR_CSS, NULL);
		return false;
	}

	{
		extern long macsurf__site_css_ok;
		macsurf__site_css_ok++;
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
