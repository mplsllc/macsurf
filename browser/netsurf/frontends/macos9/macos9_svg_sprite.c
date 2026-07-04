/*
 * MacSurf - macos9_svg_sprite.c
 *
 * External SVG sprite support (FontAwesome <use href="file.svg#id">).
 * See macos9_svg_sprite.h for the rationale and the two infra gaps this
 * works around (no XML->DOM parser, no live frontend fetch primitive).
 *
 * This is part of MacSurf, built on the NetSurf engine. GPL v2.
 */

#include "macos9.h"

#ifdef __MACOS9__

#include <string.h>
#include <stdlib.h>

#include "utils/errors.h"
#include "utils/nsurl.h"
#include "netsurf/content_type.h"
#include "content/hlcache.h"
#include "netsurf/content.h"

#include "macos9_disk_cache.h"
#include "macos9_svg_sprite.h"
#include "macsurf_debug.h"

extern struct gui_window *macos9_window_list_head(void);

/* One-sprite in-memory cache. Every 68kmla icon shares the same sprite file
 * (regular.svg), so a single held buffer covers a whole page. */
static char *g_sprite_url = NULL;	/* NUL-terminated URL of held sprite */
static char *g_sprite_data = NULL;	/* NUL-terminated sprite bytes      */
static long  g_sprite_len = 0;
static hlcache_handle *g_sprite_fetch = NULL;	/* in-flight fetch, or NULL */
static char *g_fetching_url = NULL;		/* URL currently being fetched */

/* Fetch completion callback. By the time DONE/ERROR fires the fetcher has
 * already stored the body to the disk cache, so we just drop the handle and
 * repaint — the next paint reads the bytes back via macos9_cache_lookup.
 * (CONTENT_ANY has no handler for image/svg+xml, so ERROR is the normal
 * outcome; the bytes are cached regardless.) */
static nserror
sprite_fetch_cb(hlcache_handle *h, const hlcache_event *event, void *pw)
{
	struct gui_window *gw;

	(void) pw;

	switch (event->type) {
	case CONTENT_MSG_DONE:
		{
			size_t sz = 0;
			const uint8_t *src = content_get_source_data(h, &sz);
			if (src != NULL && sz > 0 && g_fetching_url != NULL) {
				if (g_sprite_data != NULL) {
					free(g_sprite_data);
					g_sprite_data = NULL;
				}
				if (g_sprite_url != NULL) {
					free(g_sprite_url);
					g_sprite_url = NULL;
				}
				g_sprite_data = (char *) malloc((size_t) sz + 1);
				if (g_sprite_data != NULL) {
					memcpy(g_sprite_data, src, sz);
					g_sprite_data[sz] = '\0';
					g_sprite_len = (long) sz;
					g_sprite_url = (char *) malloc(
						strlen(g_fetching_url) + 1);
					if (g_sprite_url != NULL)
						strcpy(g_sprite_url,
							g_fetching_url);
				}
				macsurf_debug_log_writef(
					"svg_sprite: loaded %ld bytes",
					(long) sz);
			}
		}
		/* fall through: release the handle and repaint */
	case CONTENT_MSG_ERROR:
		if (g_sprite_fetch == h) {
			hlcache_handle_release(g_sprite_fetch);
			g_sprite_fetch = NULL;
		}
		if (g_fetching_url != NULL) {
			free(g_fetching_url);
			g_fetching_url = NULL;
		}
		gw = macos9_window_list_head();
		if (gw != NULL) {
			macos9_window_invalidate_content(gw);
		}
		break;
	default:
		break;
	}

	return NSERROR_OK;
}

/* Parse a float from **pp, skipping leading whitespace/commas, and advance. */
static float
sprite_atof_adv(const char **pp)
{
	const char *q = *pp;
	char *end = NULL;
	double v;

	while (*q == ' ' || *q == ',' || *q == '\t' || *q == '\n' ||
			*q == '\r')
		q++;
	v = strtod(q, &end);
	if (end == q) {
		*pp = q;
		return 0.0f;
	}
	*pp = end;
	return (float) v;
}

/* Locate <symbol id="frag" viewBox="…"> … </symbol> in a NUL-terminated
 * sprite. On success sets *start_out/*end_out to bound the symbol content
 * (everything between the opening tag's '>' and </symbol>) and vb[0..3] to the
 * symbol viewBox; returns 1. The caller walks that range for one or more
 * <path>. Returns 0 if the id is not present in the sprite. */
static int
sprite_parse_symbol(const char *data, const char *frag,
		const char **start_out, const char **end_out, float *vb)
{
	char idpat[160];
	const char *p;
	const char *sstart;
	const char *send;
	const char *vbp;
	const char *gt;
	long back;

	if (data == NULL || frag == NULL || frag[0] == '\0')
		return 0;
	if (strlen(frag) + 6 >= sizeof(idpat))
		return 0;

	strcpy(idpat, "id=\"");
	strcat(idpat, frag);
	strcat(idpat, "\"");

	p = strstr(data, idpat);
	if (p == NULL) {
		macsurf_debug_log_writef("svg_sprite: id not in sprite #%s",
				frag);
		return 0;
	}

	/* Back up to the enclosing <symbol (bounded); FontAwesome puts the id
	 * immediately after the tag name so this is a very short scan. */
	sstart = p;
	back = 0;
	while (sstart > data && back < 400) {
		if (strncmp(sstart, "<symbol", 7) == 0)
			break;
		sstart--;
		back++;
	}

	send = strstr(p, "</symbol>");
	if (send == NULL)
		send = data + strlen(data);

	/* viewBox (in the symbol's opening tag). */
	vb[0] = 0.0f;
	vb[1] = 0.0f;
	vb[2] = 0.0f;
	vb[3] = 0.0f;
	vbp = strstr(sstart, "viewBox=\"");
	if (vbp != NULL && vbp < send) {
		const char *q = vbp + 9;
		vb[0] = sprite_atof_adv(&q);
		vb[1] = sprite_atof_adv(&q);
		vb[2] = sprite_atof_adv(&q);
		vb[3] = sprite_atof_adv(&q);
	}

	/* Symbol content starts after the opening tag's '>'. The painter walks
	 * [start_out, end_out) for every <path> so multi-path icons paint in
	 * full. */
	gt = strchr(p, '>');
	if (gt == NULL || gt >= send)
		return 0;
	*start_out = gt + 1;
	*end_out = send;
	return 1;
}

/* Ensure the sprite at sprite_nsurl is held in g_sprite_data. Returns 1 if
 * held, 0 if not yet (cache miss -> a fetch is kicked off). */
static int
sprite_ensure_loaded(nsurl *sprite_nsurl)
{
	const char *surl = nsurl_access(sprite_nsurl);
	char *body = NULL;
	long blen = 0;
	char mime[128];
	int status = 0;

	if (surl == NULL)
		return 0;

	if (g_sprite_url != NULL && g_sprite_data != NULL &&
			strcmp(g_sprite_url, surl) == 0)
		return 1;

	mime[0] = '\0';
	if (macos9_cache_lookup(surl, &body, &blen, mime, (int) sizeof(mime),
			&status) == 1 && body != NULL && blen > 0) {
		if (g_sprite_data != NULL) {
			free(g_sprite_data);
			g_sprite_data = NULL;
		}
		if (g_sprite_url != NULL) {
			free(g_sprite_url);
			g_sprite_url = NULL;
		}
		g_sprite_data = (char *) malloc((size_t) blen + 1);
		if (g_sprite_data != NULL) {
			memcpy(g_sprite_data, body, (size_t) blen);
			g_sprite_data[blen] = '\0';
			g_sprite_len = blen;
			g_sprite_url = (char *) malloc(strlen(surl) + 1);
			if (g_sprite_url != NULL)
				strcpy(g_sprite_url, surl);
		}
		free(body);
		macsurf_debug_log_writef("svg_sprite: loaded %ld bytes %s",
				blen, surl);
		return (g_sprite_data != NULL) ? 1 : 0;
	}

	/* Cache miss: kick a fetch once so the bytes are there next redraw. */
	if (g_fetching_url != NULL && strcmp(g_fetching_url, surl) == 0)
		return 0;
	if (g_sprite_fetch == NULL) {
		nserror e = hlcache_handle_retrieve(sprite_nsurl,
				HLCACHE_RETRIEVE_SNIFF_TYPE, NULL, NULL,
				sprite_fetch_cb, NULL, NULL, CONTENT_ANY,
				&g_sprite_fetch);
		if (e == NSERROR_OK) {
			if (g_fetching_url != NULL)
				free(g_fetching_url);
			g_fetching_url = (char *) malloc(strlen(surl) + 1);
			if (g_fetching_url != NULL)
				strcpy(g_fetching_url, surl);
			macsurf_debug_log_writef("svg_sprite: fetch start %s",
					surl);
		} else {
			g_sprite_fetch = NULL;
		}
	}
	return 0;
}

int
macos9_svg_sprite_symbol(nsurl *base, const char *href,
		const char **sym_start, const char **sym_end, float *vb)
{
	const char *hashp;
	char frag[128];
	size_t i;
	nsurl *joined = NULL;
	nsurl *sprite = NULL;
	int rc;

	if (base == NULL || href == NULL || sym_start == NULL ||
			sym_end == NULL || vb == NULL)
		return 0;

	/* Fragment (symbol id) is mandatory for an external <use>. */
	hashp = strchr(href, '#');
	if (hashp == NULL)
		return 0;
	hashp++;
	i = 0;
	while (hashp[i] != '\0' && hashp[i] != '"' && hashp[i] != ' ' &&
			i < sizeof(frag) - 1) {
		frag[i] = hashp[i];
		i++;
	}
	frag[i] = '\0';
	if (frag[0] == '\0')
		return 0;

	if (nsurl_join(base, href, &joined) != NSERROR_OK || joined == NULL)
		return 0;
	if (nsurl_defragment(joined, &sprite) != NSERROR_OK || sprite == NULL) {
		nsurl_unref(joined);
		return 0;
	}
	nsurl_unref(joined);

	if (sprite_ensure_loaded(sprite) == 0) {
		nsurl_unref(sprite);
		return 0;
	}
	nsurl_unref(sprite);

	rc = sprite_parse_symbol(g_sprite_data, frag, sym_start, sym_end, vb);
	return rc;
}

#endif /* __MACOS9__ */
