/*
 * MacSurf - macos9_fetch.c
 * HTTP fetch via Open Transport.
 *
 * Uses the same sync+blocking endpoint plus OTUseSyncIdleEvents(ep, true)
 * notifier-yield pattern as the Retro68 OT demos, but in MacSurf's current
 * Carbon build the endpoint is opened through OTOpenEndpointInContext()
 * with the shared macos9_ot_context from main.c.
 *
 * InitOpenTransportInContext() is called ONCE in main() - not per-fetch.
 * This file only opens and closes individual endpoints.
 *
 * Also provides HTML tag stripping and word wrapping used by window.c
 * to turn a fetch body into displayable text.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "utils/ns_errors.h"
#include "utils/log.h"
#include "netsurf/fetch.h"
#include "content/fetch.h"   /* fixes721: struct fetch_multipart_data */
#include "macos9.h"

#ifdef __MACOS9__
#include <OpenTransport.h>
#include <OpenTptInternet.h>
#include <Threads.h>
extern OTClientContextPtr macos9_ot_context;
#endif

#include "macos9_useragent.h"

/* fixes368 (#167) — per-host User-Agent table (Classilla "sitecontrol"
 * pattern). See macos9_useragent.h. To add a site override, add a
 * { suffix, ua } row; the suffix is matched on a dot boundary against the
 * tail of the host. This is the single source of truth that replaced the
 * duplicated macos9_ua_for_host statics in the two fetchers. */
static const char MACOS9_UA_DEFAULT[] =
	"MacSurf/1.68.3 (Macintosh; PPC Mac OS 9)";

struct macos9_ua_rule {
	const char *suffix;	/* host suffix, e.g. "facebook.com" */
	const char *ua;		/* User-Agent to send to that host */
};

static const struct macos9_ua_rule macos9_ua_rules[] = {
	/*
	 * fixes391 (#167): mbasic surface, LOGGED-IN path. The KaiOS UA below
	 * gets us logged in, but on a logged-in session mbasic.facebook.com then
	 * 302-redirects to the m.facebook.com touch SPA (verified 2026-06-04
	 * hardware log: GET mbasic -> 302 m.facebook.com/?shouldForceMTouch=1,
	 * which carries the 'unsupported-interstitial' + a 1.5 MB JS bundle that
	 * runs ~14 s on the G3). fixes371's finding that the vintage MSIE5 UA
	 * gets the interstitial was for the LOGGED-OUT login page; once the
	 * c_user/xs cookies are persisted (fixes368) we no longer need the KaiOS
	 * UA on mbasic, and a non-touch vintage UA should stick on mbasic's pure
	 * HTML without the mtouch redirect. This row is more specific than the
	 * "facebook.com" row below and is matched first (first-match-wins).
	 */
	{ "mbasic.facebook.com",
	  "Mozilla/4.0 (compatible; MSIE 5.0; Mac_PowerPC)" },
	/*
	 * Facebook login surface is UA-gated. fixes371 (#167): the vintage
	 * "Mozilla/4.0 ... MSIE 5.0 ... Mac_PowerPC" string we used through
	 * fixes370 now receives Facebook's "unsupported-interstitial" overlay
	 * (verified 2026-06-03: 200 OK but the page is just "Facebook is not
	 * available on this browser", no email/pass form). The KaiOS
	 * feature-phone UA below is the lightest surface Facebook still serves
	 * the real login form to — a plain HTML POST to
	 * /login/device-based/regular/login/ with email/pass AND all hidden
	 * tokens (lsd/jazoest/m_ts/li/try_number) present as STATIC HTML, so
	 * core form.c submits it without running the page's (analytics-only)
	 * scripts. The page is ~68 KB (vs the old ~6 KB) but renders fine.
	 *
	 * IMPORTANT: do NOT append " MacSurf/..." here — Facebook's KaiOS gate
	 * is exact; any extra token drops us back to the unsupported overlay.
	 * This is a per-host spoof for facebook.com ONLY (Classilla sitecontrol
	 * pattern); every other host keeps MacSurf's honest default UA.
	 */
	{ "facebook.com",
	  "Mozilla/5.0 (Mobile; LYF/F90M/LYF-F90M-000-02-44-130319; rv:48.0) Gecko/48.0 Firefox/48.0 KAIOS/2.5" }
	/* add more host->UA overrides here */
	/*
	 * fixes741: macintoshrepository.org UA override REVERTED. fixes740 proved
	 * the https->http 301 IS UA-gated — a modern Firefox UA keeps the server
	 * on https — but the modern site it then serves is heavy and renders
	 * poorly on MacSurf, whereas the default-UA http surface is lighter and
	 * loads better. So we deliberately let it serve http. Do NOT re-add a
	 * modern-UA override here.
	 */
};

const char *macos9_user_agent_default(void)
{
	return MACOS9_UA_DEFAULT;
}

const char *macos9_user_agent_for_host(const char *host)
{
	size_t hl;
	size_t n;
	size_t i;
	if (host == NULL) return MACOS9_UA_DEFAULT;
	hl = strlen(host);
	n = sizeof(macos9_ua_rules) / sizeof(macos9_ua_rules[0]);
	for (i = 0; i < n; i++) {
		size_t sl = strlen(macos9_ua_rules[i].suffix);
		if (hl >= sl &&
		    strncasecmp(host + hl - sl,
				macos9_ua_rules[i].suffix, sl) == 0 &&
		    (hl == sl || host[hl - sl - 1] == '.')) {
			return macos9_ua_rules[i].ua;
		}
	}
	return MACOS9_UA_DEFAULT;
}

static const char *macos9_fetch_filetype(const char *unix_path)
{
	const char *ext;
	if (unix_path == NULL) return "application/octet-stream";
	ext = strrchr(unix_path, '.');
	if (ext == NULL) return "application/octet-stream";
	ext++;
	if (strcasecmp(ext, "html") == 0 || strcasecmp(ext, "htm") == 0)
		return "text/html";
	if (strcasecmp(ext, "css") == 0) return "text/css";
	if (strcasecmp(ext, "png") == 0) return "image/png";
	if (strcasecmp(ext, "gif") == 0) return "image/gif";
	if (strcasecmp(ext, "txt") == 0) return "text/plain";
	return "application/octet-stream";
}

static struct nsurl *macos9_fetch_get_resource_url(const char *path)
{
	(void)path;
	return NULL;
}

static char *macos9_fetch_mimetype(const char *ro_path)
{
	return strdup(macos9_fetch_filetype(ro_path));
}


/*
 * Strip HTML tags, decode a handful of entities, collapse whitespace.
 * Produces a plain-text version of the input into dst, up to dst_cap-1 chars.
 */
long
macos9_strip_html(const char *src, long src_len,
		char *dst, long dst_cap)
{
	long si;
	long di;
	int in_tag;
	int in_script;
	int in_style;
	int last_space;
	char tag_name[16];
	int tag_name_len;

	si = 0;
	di = 0;
	in_tag = 0;
	in_script = 0;
	in_style = 0;
	last_space = 1;
	tag_name_len = 0;
	tag_name[0] = '\0';

	while (si < src_len && di < dst_cap - 1) {
		char c = src[si];

		if (in_tag) {
			if (c == '>') {
				in_tag = 0;
				if (tag_name_len > 0) {
					tag_name[tag_name_len] = '\0';
					if (strcasecmp(tag_name, "script") == 0)
						in_script = 1;
					else if (strcasecmp(tag_name, "/script") == 0)
						in_script = 0;
					else if (strcasecmp(tag_name, "style") == 0)
						in_style = 1;
					else if (strcasecmp(tag_name, "/style") == 0)
						in_style = 0;
					else if (strcasecmp(tag_name, "br") == 0 ||
					         strcasecmp(tag_name, "br/") == 0 ||
					         strcasecmp(tag_name, "p") == 0 ||
					         strcasecmp(tag_name, "/p") == 0 ||
					         strcasecmp(tag_name, "/div") == 0 ||
					         strcasecmp(tag_name, "/tr") == 0 ||
					         strcasecmp(tag_name, "/h1") == 0 ||
					         strcasecmp(tag_name, "/h2") == 0 ||
					         strcasecmp(tag_name, "/h3") == 0 ||
					         strcasecmp(tag_name, "/h4") == 0 ||
					         strcasecmp(tag_name, "/li") == 0) {
						if (di > 0 && dst[di-1] != '\n')
							dst[di++] = '\n';
						last_space = 1;
					}
				}
				tag_name_len = 0;
			} else if (tag_name_len < 15 && c != ' ' && c != '\t') {
				tag_name[tag_name_len++] = c;
			} else if (c == ' ' || c == '\t') {
				if (tag_name_len < 15)
					tag_name[tag_name_len] = '\0';
			}
			si++;
			continue;
		}

		if (c == '<') {
			in_tag = 1;
			tag_name_len = 0;
			si++;
			continue;
		}

		if (in_script || in_style) {
			si++;
			continue;
		}

		if (c == '&') {
			if (si + 4 < src_len && strncmp(src + si, "&amp;", 5) == 0) {
				dst[di++] = '&'; si += 5; last_space = 0; continue;
			}
			if (si + 3 < src_len && strncmp(src + si, "&lt;", 4) == 0) {
				dst[di++] = '<'; si += 4; last_space = 0; continue;
			}
			if (si + 3 < src_len && strncmp(src + si, "&gt;", 4) == 0) {
				dst[di++] = '>'; si += 4; last_space = 0; continue;
			}
			if (si + 5 < src_len && strncmp(src + si, "&quot;", 6) == 0) {
				dst[di++] = '"'; si += 6; last_space = 0; continue;
			}
			if (si + 5 < src_len && strncmp(src + si, "&apos;", 6) == 0) {
				dst[di++] = '\''; si += 6; last_space = 0; continue;
			}
			if (si + 5 < src_len && strncmp(src + si, "&nbsp;", 6) == 0) {
				if (!last_space) { dst[di++] = ' '; last_space = 1; }
				si += 6; continue;
			}
			dst[di++] = c;
			si++;
			last_space = 0;
			continue;
		}

		if (c == '\r' || c == '\n') {
			if (di > 0 && dst[di-1] != '\n')
				dst[di++] = '\n';
			last_space = 1;
			si++;
			continue;
		}

		if (c == ' ' || c == '\t') {
			if (!last_space) {
				dst[di++] = ' ';
				last_space = 1;
			}
			si++;
			continue;
		}

		if ((unsigned char)c < 0x20) {
			si++;
			continue;
		}

		dst[di++] = c;
		last_space = 0;
		si++;
	}

	dst[di] = '\0';
	return di;
}

/*
 * Word-wrap a plain-text buffer into fixed-width lines. Populates
 * line_offsets[] with byte offsets into text[] and line_lengths[] with
 * byte lengths. Breaks on spaces when possible, hard-breaks otherwise.
 * Honors existing '\n' characters as forced line breaks.
 */
long
macos9_word_wrap(const char *text, long text_len,
		long *line_offsets, short *line_lengths,
		long max_lines, short max_chars_per_line)
{
	long i;
	long line_start;
	long last_space;
	long count;

	count = 0;
	line_start = 0;
	last_space = -1;

	if (max_chars_per_line < 8)
		max_chars_per_line = 8;

	for (i = 0; i <= text_len && count < max_lines; i++) {
		int hard_break;
		long width;

		hard_break = 0;
		if (i == text_len) {
			hard_break = 1;
		} else if (text[i] == '\n') {
			hard_break = 1;
		} else if (text[i] == ' ') {
			last_space = i;
		}

		width = i - line_start;

		if (hard_break) {
			line_offsets[count] = line_start;
			line_lengths[count] = (short)(i - line_start);
			count++;
			line_start = i + 1;
			last_space = -1;
			continue;
		}

		if (width >= max_chars_per_line) {
			long break_at;
			if (last_space > line_start) {
				break_at = last_space;
			} else {
				break_at = i;
			}
			line_offsets[count] = line_start;
			line_lengths[count] = (short)(break_at - line_start);
			count++;
			line_start = break_at;
			if (line_start < text_len && text[line_start] == ' ')
				line_start++;
			last_space = -1;
			i = line_start - 1;
		}
	}

	if (count == 0) {
		line_offsets[0] = 0;
		line_lengths[0] = 0;
		count = 1;
	}

	return count;
}

int macos9_ot_init(void) { return 0; }
const char *macos9_ot_get_error(void) { return NULL; }

struct gui_fetch_table macos9_fetch_table = {
	macos9_fetch_filetype,
	macos9_fetch_get_resource_url,
	NULL,
	NULL,
	macos9_fetch_mimetype,
	NULL,
	NULL
};

/* ==================================================================
 * fixes721 (#207 tooling) — multipart/form-data body builder, shared by
 * the HTTP and HTTPS fetchers so an <input type=file> upload (e.g. the
 * debug-log page) works from MacSurf. NetSurf core hands the fetcher a
 * fetch_multipart_data list; we serialise it into a real
 * multipart/form-data body, reading each file part off disk via fopen
 * (the path the NavServices picker stored on the gadget). Returns a
 * malloc'd body (caller frees), fills *out_len and the caller's
 * boundary buffer (>= 64 bytes). NULL on OOM.
 * ================================================================== */
static int macos9_mp_append(char **buf, long *len, long *cap,
		const char *src, long add)
{
	if (add <= 0) return 0;
	if (*len + add > *cap) {
		long ncap = (*cap == 0) ? 8192L : *cap;
		char *nb;
		while (ncap < *len + add) ncap *= 2L;
		nb = (char *)realloc(*buf, (size_t)ncap);
		if (nb == NULL) return -1;
		*buf = nb;
		*cap = ncap;
	}
	memcpy(*buf + *len, src, (size_t)add);
	*len += add;
	return 0;
}

char *macos9_build_multipart(const struct fetch_multipart_data *pm,
		char *boundary_out, long *out_len)
{
	static unsigned long mp_ctr = 0UL;
	char *body = NULL;
	long  len = 0L, cap = 0L;
	char  hdr[4096];
	char  boundary[64];
	const struct fetch_multipart_data *n;
	int   hl;

	*out_len = 0L;
	mp_ctr++;
	sprintf(boundary, "----------MacSurfFormBoundary4D53%lu", mp_ctr);
	if (boundary_out != NULL) strcpy(boundary_out, boundary);

	for (n = pm; n != NULL; n = n->next) {
		if (n->file) {
			hl = sprintf(hdr,
				"--%s\r\nContent-Disposition: form-data; "
				"name=\"%s\"; filename=\"%s\"\r\n"
				"Content-Type: application/octet-stream\r\n\r\n",
				boundary, (n->name != NULL) ? n->name : "",
				(n->value != NULL) ? n->value : "file");
		} else {
			hl = sprintf(hdr,
				"--%s\r\nContent-Disposition: form-data; "
				"name=\"%s\"\r\n\r\n",
				boundary, (n->name != NULL) ? n->name : "");
		}
		if (hl < 0 || macos9_mp_append(&body, &len, &cap, hdr, hl) != 0)
			goto fail;

		if (n->file) {
			FILE *f = fopen((n->rawfile != NULL) ? n->rawfile : "", "rb");
			if (f != NULL) {
				char rb[4096];
				size_t got;
				while ((got = fread(rb, 1, sizeof rb, f)) > 0) {
					if (macos9_mp_append(&body, &len, &cap,
							rb, (long)got) != 0) {
						fclose(f);
						goto fail;
					}
				}
				fclose(f);
			}
			/* fopen failure -> an empty file part; server still gets the
			 * form, just with no file bytes. */
		} else if (n->value != NULL) {
			if (macos9_mp_append(&body, &len, &cap,
					n->value, (long)strlen(n->value)) != 0)
				goto fail;
		}
		if (macos9_mp_append(&body, &len, &cap, "\r\n", 2L) != 0)
			goto fail;
	}

	hl = sprintf(hdr, "--%s--\r\n", boundary);
	if (hl < 0 || macos9_mp_append(&body, &len, &cap, hdr, hl) != 0)
		goto fail;

	*out_len = len;
	return body;

fail:
	if (body != NULL) free(body);
	*out_len = 0L;
	return NULL;
}
