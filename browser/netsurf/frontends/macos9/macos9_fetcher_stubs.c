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
	/* fixes186 — collapse <details> by default. Specificity ordering:
	 *   details > *           (0,0,0,1) base hide
	 *   details > summary     (0,0,0,2) summary always visible
	 *   details[open] > *     (0,0,1,1) opens override base hide
	 * Without this, mactrove.com's sidebar / category / mobile-menu
	 * <details> widgets paint all 57+ categories instead of the
	 * intended 13 visible + collapsed "Show N more..." control. We do
	 * not currently dispatch click → toggle [open], so users can't
	 * expand; the trade-off is correct initial layout. */
	"details > *{display:none}"
	"details > summary{display:block}"
	"details[open] > *{display:block}"
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
	/* fixes140c: HTML 5 / CSS 2.1 default <q> rendering. quotes
	 * gives the open/close pair for nesting depth 0 and 1; ::before
	 * and ::after wrap the element content in typographic quotes
	 * via generated content. \\201C / \\201D = "" curly double,
	 * \\2018 / \\2019 = '' curly single, encoded in UTF-8 so libcss's
	 * string parser sees the right bytes. */
	"q{quotes:\"\xE2\x80\x9C\" \"\xE2\x80\x9D\" \"\xE2\x80\x98\" \"\xE2\x80\x99\"}"
	"q::before{content:open-quote}"
	"q::after{content:close-quote}"
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
	"br[clear=all]{clear:both}"
	/* fixes272 — HTML5 dir="rtl" / dir="ltr" attribute wiring.
	 * NetSurf core honours css_computed_direction(...) for inline
	 * text alignment (layout.c:4326) and RTL positioning resolution
	 * (layout.c:6340), but the HTML dir attribute doesn't auto-flow
	 * to CSS direction without these UA selectors. With these rules,
	 * <html dir="rtl">, <body dir="rtl">, or <div dir="rtl"> all
	 * trigger right-to-left inline layout. Closes #49. */
	"[dir=rtl]{direction:rtl;unicode-bidi:embed}"
	"[dir=ltr]{direction:ltr;unicode-bidi:embed}"
	"[dir=auto]{unicode-bidi:plaintext}"
	/* fixes272 — writing-mode parsed but no visual effect in V1
	 * (NetSurf's inline layout is horizontally hard-wired). Parser
	 * accepts the property cleanly; future layout-direction work
	 * will activate it. Documented in #35. */
	"bdi{unicode-bidi:isolate}"
	"bdo[dir=rtl]{direction:rtl;unicode-bidi:bidi-override}"
	"bdo[dir=ltr]{direction:ltr;unicode-bidi:bidi-override}";

static const char css_internal[] =
	"input,textarea,button,select{background:#fff;color:#000;"
	"border:1px solid #777;padding:1px 2px;font:inherit}"
	"input[type=submit],input[type=reset],input[type=button],"
	"button{background:#ddd;border:1px outset #ccc;padding:1px 6px}"
	"input[type=hidden]{display:none}"
	"input[type=checkbox],input[type=radio]{border:0;padding:0}"
	"progress,meter{display:inline-block;width:10em;height:1em;"
	"background:#ccc;border:1px inset #999}"
	"noscript{display:block}"
	/* fixes168c — Modern-web rescue rules. Targeted at JS-required
	 * overlay patterns and content-hidden-until-hydration patterns.
	 * Selectors are narrow enough that legitimate uses are extremely
	 * rare; the rescue payload is unconditional. */
	/* Cookie / consent banner suppression. */
	"[class*=\"cookie-banner\"],[class*=\"cookie-consent\"],"
	"[class*=\"cookie-modal\"],[class*=\"cookie-overlay\"],"
	"[class*=\"cookie-wall\"],[class*=\"consent-banner\"],"
	"[class*=\"consent-modal\"],[class*=\"consent-overlay\"],"
	"[id*=\"cookie-banner\"],[id*=\"cookie-consent\"],"
	"[id*=\"cookie-wall\"],[id*=\"consent-banner\"],"
	"[id*=\"consent-modal\"]{display:none !important}"
	/* Newsletter / subscribe / paywall overlay suppression. */
	"[class*=\"newsletter-popup\"],[class*=\"newsletter-modal\"],"
	"[class*=\"subscribe-modal\"],[class*=\"signup-modal\"],"
	"[class*=\"paywall-modal\"],[class*=\"paywall-overlay\"],"
	"[id*=\"newsletter-popup\"],[id*=\"subscribe-modal\"],"
	"[id*=\"signup-modal\"]{display:none !important}"
	/* Unhide content containers that JS would normally hydrate. */
	"article[hidden],main[hidden],section[hidden]"
	"{display:block !important}"
	"[hidden][class*=\"article\"],[hidden][class*=\"content\"],"
	"[hidden][class*=\"story\"],[hidden][class*=\"post\"],"
	"[hidden][class*=\"entry\"]{display:block !important}"
	/* Visibility/aria rescue for content containers only. */
	"article[aria-hidden=\"true\"],main[aria-hidden=\"true\"],"
	"section[aria-hidden=\"true\"]{visibility:visible !important}"
	/* JS-state class rescues, scoped to content selectors only. */
	"[class*=\"js-hidden\"][class*=\"article\"],"
	"[class*=\"js-hidden\"][class*=\"content\"],"
	"[class*=\"js-hidden\"][class*=\"main\"]"
	"{display:block !important;visibility:visible !important}"
	/* "noscript" content survives. We already display:block above; this
	 * is the matching rescue for sites that gate real content behind
	 * a noscript fallback. */
	".no-js,html.no-js{display:block !important}";

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
	char path[1024];
	bool started;
	bool aborted;
	bool done;
	struct stub_fetch_ctx *r_next;
	struct stub_fetch_ctx *r_prev;
	/* fixes242 — dynamic body for about: pages. about:query/fetcherror
	 * encodes the failed URL in its query string; we generate a friendly
	 * error page with the URL embedded so users don't see the bare
	 * white "MacSurf" page on fetch failures. body_used is the length
	 * actually populated; 0 means use the static fallback in stub_body_for. */
	char body_buf[2048];
	size_t body_used;
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
		/* fixes242 — if setup populated a dynamic body (e.g. an error
		 * page for about:query/fetcherror), serve it. Otherwise fall
		 * back to the generic welcome page. */
		if (ctx->body_used > 0) {
			*body_out = ctx->body_buf;
			*len_out = ctx->body_used;
			*mime_out = "text/html";
			return;
		}
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

	/* fixes242 — about:query/fetcherror gets a friendly explanation
	 * page with the failed URL embedded, instead of the bare "MacSurf"
	 * placeholder that confuses users. The query string carries the
	 * failed URL as "url=..."; we extract it (URL-encoded, no decode
	 * to dodge HTML-injection concerns) and embed it twice: once as
	 * displayed text and once as a retry link. */
	if (scheme == SCH_ABOUT &&
	    strncmp(ctx->path, "query/fetcherror", 16) == 0) {
		const char *url_param;
		char failed_url[1024];
		int rn;

		failed_url[0] = '\0';
		url_param = strstr(ctx->path, "url=");
		if (url_param != NULL) {
			size_t i = 0;
			url_param += 4;   /* skip "url=" */
			while (url_param[i] != '\0' &&
			       url_param[i] != '&' &&
			       i < sizeof(failed_url) - 1) {
				failed_url[i] = url_param[i];
				i++;
			}
			failed_url[i] = '\0';
		}

		rn = sprintf(ctx->body_buf,
		    "<html><head><title>Couldn't load page</title>"
		    "<style>body{font-family:Geneva,sans-serif;"
		    "background:#dddddd;color:#222222;padding:48px;}"
		    "h1{color:#003366;margin-bottom:24px;font-size:24pt;}"
		    "p{font-size:14pt;line-height:1.5;}"
		    "tt{background:#ffffff;padding:6px 10px;"
		    "border:1px solid #999999;color:#003366;}"
		    "ul{font-size:12pt;color:#444444;}"
		    "a{color:#003366;}</style></head>"
		    "<body><h1>Couldn't load that page.</h1>"
		    "<p>MacSurf tried to reach:</p>"
		    "<p><tt>%.512s</tt></p>"
		    "<p>This usually means:</p>"
		    "<ul>"
		    "<li>The server didn't respond, or is offline.</li>"
		    "<li>The site's TLS handshake rejected the connection "
		    "(some sites block older clients).</li>"
		    "<li>The network connection timed out.</li>"
		    "</ul>"
		    "<p>You can <a href=\"%.512s\">retry the same URL</a>, "
		    "or pick another page from your bookmarks.</p>"
		    "</body></html>",
		    failed_url, failed_url);
		if (rn > 0 && (size_t)rn < sizeof(ctx->body_buf)) {
			ctx->body_used = (size_t)rn;
		}
	}

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
	struct stub_fetch_ctx *next;
	int safety;
	int did_work;

	(void)scheme;
	if (stub_ring == NULL) return;

	/* fixes105 — release every stub fetch through
	 * fetch_remove_from_queues + fetch_free, mirroring every
	 * reference fetcher in NetSurf core (curl.c, file.c, about.c,
	 * data.c, resource.c, css_fetcher.c, javascript/fetcher.c).
	 *
	 * Pre-fixes105 the stub fetcher dispatched FETCH_FINISHED and
	 * marked ctx->done=true but NEVER called fetch_remove_from_queues
	 * or fetch_free. Every resource:/about:/data:/javascript: fetch
	 * leaked into NetSurf's fetch_ring permanently. Visible in the
	 * log as "stub setup; stub start; stub snd; FETCH FINISHED;
	 * stub fin" pairs on every html_create — two leaks per page
	 * (resource:default.css + resource:internal.css). Combined with
	 * the aborted-while-queued stub leak (aborted contexts never get
	 * past the !ctx->aborted gate in the old dispatch condition) this
	 * was the second half of the "stops after about three pages"
	 * wall, paralleling the HTTP fetcher's pre-fixes102 behaviour.
	 *
	 * Two cleanup branches now:
	 * 1. ctx->aborted — caller (NetSurf) wants this fetch gone. No
	 *    callbacks (matches curl ops.abort), just remove + free.
	 * 2. ctx->started && !ctx->done — normal dispatch. stub_send_for
	 *    emits HEADER/DATA/FINISHED, then remove + free.
	 *
	 * After fetch_free returns, ctx is freed memory (fetch_free
	 * synchronously calls stub_free which removes from stub_ring
	 * and free()s ctx). Break the inner do-loop after any cleanup
	 * so the outer while re-reads stub_ring's head.
	 */
	safety = 0;
	while (stub_ring != NULL && safety < 64) {
		safety++;
		ctx = stub_ring;
		start = ctx;
		did_work = 0;

		do {
			next = ctx->r_next;

			if (ctx->aborted) {
				fetch_remove_from_queues(ctx->parent);
				fetch_free(ctx->parent);
				did_work = 1;
				break;
			}

			if (ctx->started && !ctx->done) {
				stub_send_for(ctx);
				fetch_remove_from_queues(ctx->parent);
				fetch_free(ctx->parent);
				did_work = 1;
				break;
			}

			ctx = next;
		} while (ctx != start);

		if (!did_work) break;
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
