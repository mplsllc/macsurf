/*
 * MacSurf - macos9_fetcher_stubs.c
 *
 * Per-scheme fetcher implementations for resource:, about:, file:,
 * data:, and javascript: URL schemes.
 *
 * resource: serves real CSS bodies for default.css, internal.css,
 * quirks.css, and a minimal favicon.ico. about:, file:, data:,
 * javascript: return empty bodies with an appropriate MIME type so
 * the HTML content handler completes its child-fetch dependencies
 * instead of hanging with base.active > 0.
 *
 * Each ctx remembers its scheme + path and dispatches in poll().
 */

#include <stdlib.h>
#include <string.h>

#include "utils/errors.h"
#include "utils/nsurl.h"
#include "utils/log.h"
#include "utils/utils.h"
#include "content/fetch.h"
#include "content/fetchers.h"

#include "macsurf_debug.h"

/* ---- Embedded CSS resources ---- *
 *
 * These are the Mac-OS-9 distilled versions of NetSurf's default /
 * internal / quirks stylesheets. They are intentionally small so
 * they fit in data segment on a 64MB G3 without bloating the
 * built binary. The selectors and properties here are the ones
 * the HTML handler actually consults during layout.
 */

/* Each adjacent literal must stay under the 509-char C89 limit; the
 * compiler concatenates them into a single array at build time.
 *
 * Modelled on NetSurf's resources/default.css but condensed and
 * extended with HTML5 element coverage. */
static const char css_default[] =
	/* Block / flow display */
	"html,address,blockquote,body,dd,div,dl,dt,fieldset,form,"
	"frame,frameset,h1,h2,h3,h4,h5,h6,noframes,ol,p,ul,center,"
	"dir,hr,menu,pre{display:block}"
	/* HTML5 sectioning */
	"header,footer,nav,section,article,aside,main,figure,"
	"figcaption,details,summary,hgroup,dialog,picture{display:block}"
	"li{display:list-item}"
	"head,style,script,title,meta,link,base,template,"
	"noembed,noscript,source,track{display:none}"
	/* Tables */
	"table{display:table;border-spacing:2px;border-collapse:separate}"
	"tr{display:table-row;vertical-align:inherit}"
	"thead{display:table-header-group;vertical-align:middle}"
	"tbody{display:table-row-group;vertical-align:middle}"
	"tfoot{display:table-footer-group;vertical-align:middle}"
	"col{display:table-column}"
	"colgroup{display:table-column-group}"
	"td,th{display:table-cell;vertical-align:inherit;padding:1px}"
	"caption{display:table-caption;text-align:center}"
	"th{font-weight:bold;text-align:center}"
	"table[border],table[border] td,table[border] tr{"
	"border-color:#888;border-style:solid;border-width:1px}"
	/* Body + headings */
	"body{margin:8px;line-height:1.33;color:#000;background:#fff;"
	"font-family:sans-serif;font-size:13px}"
	"h1{font-size:2em;margin:.67em 0;font-weight:bold}"
	"h2{font-size:1.5em;margin:.83em 0;font-weight:bold}"
	"h3{font-size:1.17em;margin:1em 0;font-weight:bold}"
	"h4{margin:1.33em 0;font-weight:bold}"
	"h5{font-size:.83em;margin:1.67em 0;font-weight:bold}"
	"h6{font-size:.67em;margin:2.33em 0;font-weight:bold}"
	/* Paragraphs and block margins */
	"p{margin:1em 0}"
	"blockquote,figure{margin:1em 40px}"
	"hr{margin:.5em auto;border:1px inset #888;height:0}"
	"hr[noshade]{border-style:solid}"
	/* Lists */
	"ul{padding-left:40px;margin:1em 0;list-style-type:disc}"
	"ol{padding-left:40px;margin:1em 0;list-style-type:decimal}"
	"ul ul{list-style-type:circle}"
	"ul ul ul{list-style-type:square}"
	"ol ul,ul ol,ul ul,ol ol{margin-top:0;margin-bottom:0}"
	"dir,menu{padding-left:1.5em;margin:1em 0}"
	"dl{padding-left:1.5em;margin:1em 0}"
	"dt{font-weight:bold}"
	"dd{padding-left:1em;margin-bottom:.33em}"
	/* Inline styling */
	"u,ins{text-decoration:underline}"
	"ins{color:green}"
	"strike,s,del{text-decoration:line-through}"
	"del{color:#a00}"
	"b,strong{font-weight:bold}"
	"i,em,cite,var,dfn,q{font-style:italic}"
	"address{font-style:italic;display:block}"
	"abbr,acronym{font-variant:small-caps}"
	/* Monospace */
	"tt,code,kbd,samp,pre{font-family:monospace}"
	"kbd{font-weight:bold}"
	"pre{white-space:pre;margin:1em 0;font-family:monospace}"
	/* Sizes */
	"big{font-size:1.17em}"
	"small,sub,sup{font-size:.83em}"
	"sub{vertical-align:sub}"
	"sup{vertical-align:super}"
	/* Forms */
	"form{display:block;margin:0 0 1em}"
	"button,textarea,input,select{display:inline-block;"
	"background:#fff;color:#000;border:1px solid #777;padding:1px 3px;"
	"font-family:sans-serif;font-size:13px}"
	"input[type=submit],input[type=reset],input[type=button],"
	"button{background:#ddd;border:1px outset #ccc;padding:1px 6px;"
	"text-align:center}"
	"input[type=hidden]{display:none}"
	"input[type=checkbox],input[type=radio]{border:0;padding:0}"
	"textarea{font-family:monospace}"
	"fieldset{display:block;border:1px solid #888;margin:1em 0;padding:.5em}"
	"legend{padding:0 .5em}"
	"label{display:inline}"
	/* Images / replaced */
	"img{color:#888}"
	"iframe{width:19em;height:10em}"
	/* Anchors */
	"a:link{color:#00f;text-decoration:underline}"
	"a:visited{color:#609;text-decoration:underline}"
	"a:hover{color:#f00}"
	"a:active{color:#f80}"
	"a[href]{color:#00f;text-decoration:underline}"
	/* Misc */
	"center{display:block;text-align:center}"
	"mark{background:#ff0;color:#000}"
	"br[clear=left]{clear:left}"
	"br[clear=right]{clear:right}"
	"br[clear=all]{clear:both}";

static const char css_internal[] =
	"input,textarea,button,select{background:#fff;color:#000;"
	"border:1px solid #777;padding:1px 2px;font:inherit}"
	"input[type=submit],input[type=reset],input[type=button],"
	"button{background:#ddd;border:1px outset #ccc;padding:1px 6px}"
	"input[type=hidden]{display:none}"
	"input[type=checkbox],input[type=radio]{border:0;padding:0}"
	"progress,meter{display:inline-block;width:10em;height:1em;"
	"background:#ccc;border:1px inset #999}"
	"noscript{display:block}";

static const char css_quirks[] =
	"table{font-size:inherit;font-weight:inherit;text-align:start;"
	"border-collapse:separate}"
	"img{border:0}"
	"form{margin:0}"
	"body{margin:8px}";

static const unsigned char favicon_ico[6] = { 0,0,0,0,0,0 };

/* ---- Scheme + dispatch table ---- */

typedef enum {
	SCH_RESOURCE = 0,
	SCH_ABOUT,
	SCH_FILE,
	SCH_DATA,
	SCH_JAVASCRIPT
} stub_scheme;

struct stub_fetch_ctx {
	struct fetch *parent;
	stub_scheme scheme;
	char path[128];
	bool started;
	bool aborted;
	bool done;
	struct stub_fetch_ctx *r_next;
	struct stub_fetch_ctx *r_prev;
};

static struct stub_fetch_ctx *stub_ring = NULL;

bool
macos9_stub_fetcher_active(void)
{
	return stub_ring != NULL;
}

/* ---- Ring helpers ---- */

static void
stub_ring_insert(struct stub_fetch_ctx *ctx)
{
	if (stub_ring == NULL) {
		stub_ring = ctx;
		ctx->r_next = ctx;
		ctx->r_prev = ctx;
	} else {
		ctx->r_next = stub_ring;
		ctx->r_prev = stub_ring->r_prev;
		stub_ring->r_prev->r_next = ctx;
		stub_ring->r_prev = ctx;
	}
}

static void
stub_ring_remove(struct stub_fetch_ctx *ctx)
{
	if (ctx->r_next == ctx) {
		stub_ring = NULL;
	} else {
		ctx->r_prev->r_next = ctx->r_next;
		ctx->r_next->r_prev = ctx->r_prev;
		if (stub_ring == ctx)
			stub_ring = ctx->r_next;
	}
	ctx->r_next = NULL;
	ctx->r_prev = NULL;
}

/* ---- URL-aware body selection ---- */

static const char *
resource_tail(const char *path)
{
	const char *p;
	const char *tail;

	if (path == NULL) return "";
	tail = path;
	for (p = path; *p != '\0'; p++) {
		if (*p == '/')
			tail = p + 1;
	}
	return tail;
}

static void
stub_body_for(const struct stub_fetch_ctx *ctx,
	      const char **body_out, size_t *len_out,
	      const char **mime_out)
{
	const char *tail;

	switch (ctx->scheme) {
	case SCH_RESOURCE:
		tail = resource_tail(ctx->path);
		if (strcmp(tail, "default.css") == 0) {
			*body_out = css_default;
			*len_out = sizeof(css_default) - 1;
			*mime_out = "text/css";
			return;
		}
		if (strcmp(tail, "internal.css") == 0) {
			*body_out = css_internal;
			*len_out = sizeof(css_internal) - 1;
			*mime_out = "text/css";
			return;
		}
		if (strcmp(tail, "quirks.css") == 0) {
			*body_out = css_quirks;
			*len_out = sizeof(css_quirks) - 1;
			*mime_out = "text/css";
			return;
		}
		if (strcmp(tail, "favicon.ico") == 0) {
			*body_out = (const char *)favicon_ico;
			*len_out = sizeof(favicon_ico);
			*mime_out = "image/x-icon";
			return;
		}
		*body_out = "";
		*len_out = 0;
		*mime_out = "application/octet-stream";
		return;

	case SCH_ABOUT:
		*body_out = "<html><head><title>about:</title></head>"
			    "<body><h1>MacSurf</h1></body></html>";
		*len_out = strlen(*body_out);
		*mime_out = "text/html";
		return;

	case SCH_FILE:
		*body_out = "";
		*len_out = 0;
		*mime_out = "application/octet-stream";
		return;

	case SCH_DATA:
		*body_out = "";
		*len_out = 0;
		*mime_out = "application/octet-stream";
		return;

	case SCH_JAVASCRIPT:
		*body_out = "";
		*len_out = 0;
		*mime_out = "text/javascript";
		return;
	}

	*body_out = "";
	*len_out = 0;
	*mime_out = "application/octet-stream";
}

/* ---- fetcher_operation_table callbacks ---- */

static bool
stub_initialise(lwc_string *scheme)
{
	(void)scheme;
	return true;
}

static void
stub_finalise(lwc_string *scheme)
{
	(void)scheme;
}

static bool
stub_can_fetch(const struct nsurl *url)
{
	(void)url;
	return true;
}

static void *
stub_setup_scheme(struct fetch *parent_fetch, struct nsurl *url,
		  stub_scheme scheme)
{
	struct stub_fetch_ctx *ctx;
	const char *url_str;
	size_t url_len;
	size_t copy;
	const char *colon;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) return NULL;

	ctx->parent = parent_fetch;
	ctx->scheme = scheme;

	url_str = nsurl_access(url);
	url_len = strlen(url_str);

	/* Skip past "scheme:" to capture the path for dispatch. */
	colon = strchr(url_str, ':');
	if (colon != NULL) {
		url_str = colon + 1;
		while (*url_str == '/') url_str++;
		url_len = strlen(url_str);
	}
	copy = url_len;
	if (copy >= sizeof(ctx->path))
		copy = sizeof(ctx->path) - 1;
	memcpy(ctx->path, url_str, copy);
	ctx->path[copy] = '\0';

	stub_ring_insert(ctx);
	MS_LOG("stub setup");
	return ctx;
}

static void *
stub_setup_resource(struct fetch *parent_fetch, struct nsurl *url,
		    bool only_2xx, bool downgrade_tls,
		    const char *post_urlenc,
		    const struct fetch_multipart_data *post_multipart,
		    const char **headers)
{
	(void)only_2xx; (void)downgrade_tls;
	(void)post_urlenc; (void)post_multipart; (void)headers;
	return stub_setup_scheme(parent_fetch, url, SCH_RESOURCE);
}

static void *
stub_setup_about(struct fetch *parent_fetch, struct nsurl *url,
		 bool only_2xx, bool downgrade_tls,
		 const char *post_urlenc,
		 const struct fetch_multipart_data *post_multipart,
		 const char **headers)
{
	(void)only_2xx; (void)downgrade_tls;
	(void)post_urlenc; (void)post_multipart; (void)headers;
	return stub_setup_scheme(parent_fetch, url, SCH_ABOUT);
}

static void *
stub_setup_file(struct fetch *parent_fetch, struct nsurl *url,
		bool only_2xx, bool downgrade_tls,
		const char *post_urlenc,
		const struct fetch_multipart_data *post_multipart,
		const char **headers)
{
	(void)only_2xx; (void)downgrade_tls;
	(void)post_urlenc; (void)post_multipart; (void)headers;
	return stub_setup_scheme(parent_fetch, url, SCH_FILE);
}

static void *
stub_setup_data(struct fetch *parent_fetch, struct nsurl *url,
		bool only_2xx, bool downgrade_tls,
		const char *post_urlenc,
		const struct fetch_multipart_data *post_multipart,
		const char **headers)
{
	(void)only_2xx; (void)downgrade_tls;
	(void)post_urlenc; (void)post_multipart; (void)headers;
	return stub_setup_scheme(parent_fetch, url, SCH_DATA);
}

static void *
stub_setup_javascript(struct fetch *parent_fetch, struct nsurl *url,
		      bool only_2xx, bool downgrade_tls,
		      const char *post_urlenc,
		      const struct fetch_multipart_data *post_multipart,
		      const char **headers)
{
	(void)only_2xx; (void)downgrade_tls;
	(void)post_urlenc; (void)post_multipart; (void)headers;
	return stub_setup_scheme(parent_fetch, url, SCH_JAVASCRIPT);
}

static bool
stub_start(void *fetch)
{
	struct stub_fetch_ctx *ctx = fetch;
	if (ctx != NULL) {
		ctx->started = true;
		MS_LOG("stub start");
	}
	return true;
}

static void
stub_abort(void *fetch)
{
	struct stub_fetch_ctx *ctx = fetch;
	if (ctx != NULL) ctx->aborted = true;
}

static void
stub_free(void *fetch)
{
	struct stub_fetch_ctx *ctx = fetch;
	if (ctx == NULL) return;
	stub_ring_remove(ctx);
	free(ctx);
}

static void
stub_send_for(struct stub_fetch_ctx *ctx)
{
	fetch_msg msg;
	const char *body;
	size_t body_len;
	const char *mime;
	char header[96];
	int hlen;

	MS_LOG("stub snd");
	fetch_set_http_code(ctx->parent, 200);
	stub_body_for(ctx, &body, &body_len, &mime);

	hlen = 0;
	{
		const char prefix[] = "Content-Type: ";
		size_t plen = sizeof(prefix) - 1;
		size_t mlen = strlen(mime);
		if (plen + mlen >= sizeof(header))
			mlen = sizeof(header) - plen - 1;
		memcpy(header, prefix, plen);
		memcpy(header + plen, mime, mlen);
		hlen = (int)(plen + mlen);
		header[hlen] = '\0';
	}

	msg.type = FETCH_HEADER;
	msg.data.header_or_data.buf = (const uint8_t *)header;
	msg.data.header_or_data.len = (size_t)hlen;
	fetch_send_callback(&msg, ctx->parent);
	if (ctx->aborted) { ctx->done = true; return; }

	if (body_len > 0) {
		msg.type = FETCH_DATA;
		msg.data.header_or_data.buf = (const uint8_t *)body;
		msg.data.header_or_data.len = body_len;
		fetch_send_callback(&msg, ctx->parent);
		if (ctx->aborted) { ctx->done = true; return; }
	}

	msg.type = FETCH_FINISHED;
	ctx->done = true;
	fetch_send_callback(&msg, ctx->parent);
	MS_LOG("stub fin");
}

static void
stub_poll(lwc_string *scheme)
{
	struct stub_fetch_ctx *ctx;
	struct stub_fetch_ctx *start;
	int safety;

	(void)scheme;
	if (stub_ring == NULL) return;

	/* Walk the ring once, dispatching every started-and-not-done
	 * context. fetch_send_callback may free ctx via stub_free
	 * (which mutates stub_ring), so we re-acquire the head
	 * each loop pass. */
	safety = 0;
	while (stub_ring != NULL && safety < 64) {
		ctx = stub_ring;
		start = ctx;
		safety++;

		do {
			struct stub_fetch_ctx *next = ctx->r_next;
			if (ctx->started && !ctx->done && !ctx->aborted) {
				stub_send_for(ctx);
				/* stub_ring may have mutated. Break
				 * inner loop so the outer while re-
				 * reads the head. */
				break;
			}
			ctx = next;
		} while (ctx != start);

		/* If we walked the full ring without dispatching
		 * anything, nothing more to do this pass. */
		if (ctx == start) {
			if (!ctx->started || ctx->done || ctx->aborted)
				break;
		}
	}
}

/* ---- fetcher_operation_table instances ---- */

/* Field order: initialise, can_fetch, setup, start, abort, free,
 * poll, fdset, finalise (see content/fetchers.h). */

static const struct fetcher_operation_table stub_ops_resource = {
	stub_initialise, stub_can_fetch, stub_setup_resource,
	stub_start, stub_abort, stub_free, stub_poll, NULL, stub_finalise
};

static const struct fetcher_operation_table stub_ops_about = {
	stub_initialise, stub_can_fetch, stub_setup_about,
	stub_start, stub_abort, stub_free, stub_poll, NULL, stub_finalise
};

static const struct fetcher_operation_table stub_ops_file = {
	stub_initialise, stub_can_fetch, stub_setup_file,
	stub_start, stub_abort, stub_free, stub_poll, NULL, stub_finalise
};

static const struct fetcher_operation_table stub_ops_data = {
	stub_initialise, stub_can_fetch, stub_setup_data,
	stub_start, stub_abort, stub_free, stub_poll, NULL, stub_finalise
};

static const struct fetcher_operation_table stub_ops_javascript = {
	stub_initialise, stub_can_fetch, stub_setup_javascript,
	stub_start, stub_abort, stub_free, stub_poll, NULL, stub_finalise
};

/* ---- Per-scheme registration ---- */

nserror fetch_resource_register(void)
{
	lwc_string *scheme;
	if (lwc_intern_string("resource", 8, &scheme) != lwc_error_ok)
		return NSERROR_NOMEM;
	return fetcher_add(scheme, &stub_ops_resource);
}

nserror fetch_about_register(void)
{
	lwc_string *scheme;
	if (lwc_intern_string("about", 5, &scheme) != lwc_error_ok)
		return NSERROR_NOMEM;
	return fetcher_add(scheme, &stub_ops_about);
}

nserror fetch_file_register(void)
{
	lwc_string *scheme;
	if (lwc_intern_string("file", 4, &scheme) != lwc_error_ok)
		return NSERROR_NOMEM;
	return fetcher_add(scheme, &stub_ops_file);
}

nserror fetch_data_register(void)
{
	lwc_string *scheme;
	if (lwc_intern_string("data", 4, &scheme) != lwc_error_ok)
		return NSERROR_NOMEM;
	return fetcher_add(scheme, &stub_ops_data);
}

nserror fetch_javascript_register(void)
{
	lwc_string *scheme;
	if (lwc_intern_string("javascript", 10, &scheme) != lwc_error_ok)
		return NSERROR_NOMEM;
	return fetcher_add(scheme, &stub_ops_javascript);
}
