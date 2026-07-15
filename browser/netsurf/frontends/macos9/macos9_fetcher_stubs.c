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

#include "utils/ns_errors.h"
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
	/* fixes186 + fixes351c — collapse <details> by default.
	 *
	 * Previous rule used `details > *{display:none}` which is the
	 * standard browser approach, but in MacSurf box_construct.c
	 * excludes display:none elements from the box tree at construct
	 * time. After fixes329 added click → toggle [open], calling
	 * html_recascade_tree refreshed styles on EXISTING boxes — but
	 * the inner div never had a box to start with, so toggling [open]
	 * flipped the styled details container background to green but
	 * inner content never appeared.
	 *
	 * Fix: collapse via height:0 + overflow:hidden instead. The inner
	 * box stays in the tree; only its measured height changes when
	 * [open] flips. Same visual result (children hidden when closed),
	 * but now amenable to recascade-only refresh.
	 *
	 *   details > *           (0,0,0,1) base collapse to 0
	 *   details > summary     (0,0,0,2) summary always at natural height
	 *   details[open] > *     (0,0,1,1) opens restore natural height
	 */
	"details > *{height:0;overflow:hidden}"
	"details > summary{height:auto;overflow:visible}"
	"details[open] > *{height:auto;overflow:visible}"
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
	/* fixes822: border="0" means NO border (HTML presentational hint).
	 * The [border] rule above matches any table POSSESSING the attribute
	 * regardless of value, so border="0" tables (Hacker News writes it on
	 * every table) grew phantom 1px cell borders. Same specificity, later
	 * in the sheet -> wins. */
	"table[border='0'],table[border='0'] td,table[border='0'] tr{"
	"border-style:none;border-width:0}"
	/* Body + headings */
	/* fixes629: THE white-background-on-dark-sites fix. The old UA
	 * body{background:#fff;color:#000} painted the body box WHITE over a
	 * dark author html{background:#2d3238} canvas (box_html captured the
	 * correct #2d3238 base, but the body box then covered it), and forced
	 * black body text over the author's light color -- so dark-themed
	 * sites (tinkerdifferent, any XenForo dark theme) rendered white with
	 * black hero text. Fix: put the default text colour on the ROOT so a
	 * dark-theme author html{color:...} is inherited by body, and set NO
	 * background here. We do NOT add html{background} -- that would defeat
	 * the CSS body-background->canvas propagation used by sites that set
	 * their page colour on <body>. Plain pages still render black-on-
	 * white: the canvas defaults to white (data->background_colour) and
	 * html{color:#000} makes text black. */
	"html{color:#000}"
	/* fixes830 (#244): default body font 13px -> 16px to match every
	 * mainstream browser's default (Chrome/Firefox/Safari all render an
	 * unstyled page at 16px). 13px made every un-styled page read ~20%
	 * too small -- the top v2.0 legibility complaint. em-based headings
	 * (h1=2em etc.) scale proportionally; pages that set their own
	 * font-size are unaffected (they override this). line-height stays
	 * 1.33 (comfortable; mainstream 'normal' is ~1.2 but tighter reads
	 * worse at OS-9 AA quality). */
	"body{margin:8px;line-height:1.33;"
	"font-family:sans-serif;font-size:16px}"
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
	/* fixes497: XenForo editor textarea reveal is done in JS (the editor
	 * stub strips `u-jsOnly` and sets an inline !important style on the
	 * real <textarea class="js-editor">). The earlier UA-stylesheet
	 * reveals (fixes479/492/496b: editorPlaceholder un-hide, textarea
	 * force-show, container min-heights) were REMOVED — a UA !important
	 * rule cannot override an author !important (`.u-jsOnly{display:none
	 * !important}`), so they never took effect AND the blanket
	 * `.formRow,.formRow>*{min-height:200px!important}` risked distorting
	 * unrelated form rows. Inline style from JS is the only layer that
	 * wins, so the reveal lives there now. */
	/* fixes469: form inputs lack intrinsic width in NetSurf's minmax pass
	 * (b->max_width=0 for replaced form elements with no text content).
	 * In a flex container with flex-basis:auto, base_size=0, so the input
	 * collapses. Give text/password/email inputs a UA default width matching
	 * the HTML spec's size=20 default (~20em). Author width:100% overrides
	 * this, but flex base_size can then use b->width instead of falling to 0. */
	"input[type=text],input[type=password],input[type=email],"
	"input[type=search],input[type=url],input[type=tel]{"
	"width:20em;min-width:4em}"
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
	/* fixes743 (#114) — HTML5 hidden attribute -> display:none generally.
	 * Non-!important so the content-container rescue rules below re-show
	 * article/main/section[hidden] (the JS-hydration pattern) via !important;
	 * a bare <div>/<span>/<button hidden> that JS won't reveal stays hidden. */
	"[hidden]{display:none}"
	/* fixes746 (#204) — XenForo header account/search group renders grey. The
	 * container .p-navgroup has TWO rules: an early background:rgba(20,20,20,
	 * 0.15) overlay and a later background:none reset. MacSurf drops the `none`
	 * reset AND composites the rgba against the page's #ebebeb (not the blue
	 * behind it), so rgba(20,20,20,.15) over #ebebeb == #cacaca — exactly the
	 * grey seen on the blue bar. Force the group + its iconic links transparent
	 * so the blue .p-header/.p-nav shows through. */
	".p-navgroup,.p-navgroup-link--iconic,.p-navgroup-link--search,"
	".p-navgroup-link--user{background-color:transparent !important;border:0 !important}"
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
	".no-js,html.no-js{display:block !important}"
	/* fixes329 (#110) — HTML5 <details>/<summary> disclosure widget.
	 * Closed details collapses everything but its summary. summary
	 * indicates clickability. Click → toggle handled in
	 * macos9_js_click.c by toggling the `open` attribute. */
	" details>:not(summary){display:none}"
	" details[open]>*{display:block}"
	" summary{display:block;cursor:pointer;font-weight:bold;"
	"padding:2px 0}"
	/* fixes329 (#114) — HTML5 hidden attribute defaults. Specific
	 * articles/main/section [hidden] rescue rules later in this
	 * sheet still override. */
	" [hidden]{display:none}"
	/* fixes499c — modest post spacing on XenForo message blocks. The old
	 * patches forced large min-heights (distorting layout); this is gentle,
	 * NON-!important breathing room only — author CSS still wins where it
	 * sets its own. Just enough to separate stacked posts/articles. */
	" .message,.block-row,article.message{margin-bottom:10px}"
	" .message-inner,.message-cell--main{padding:6px 0}"
	/* fixes499d — give the post editor a little vertical breathing room so
	 * the "Attach files" button row below the textarea isn't clipped where
	 * the editor wrapper meets the Options row. The editor textarea has an
	 * inline fixed height (set by the reveal), so the attach/footer row was
	 * overflowing the container; a small bottom margin clears it. Gentle,
	 * non-!important. */
	" .formRow--editor,.bbCodeEditorContainer,.fr-box,.editorPlaceholder{"
	"margin-bottom:18px}"
	" .formButtonGroup,.js-attachmentUpload,.formSubmitRow{margin-top:8px}";
	/* fixes497: all XenForo-specific UA CSS overrides removed (clean
	 * slate). These were .p-header / .js-bbCodeInput / textarea.js-editor /
	 * .node-main / .node-extra-icon force rules plus the editorPlaceholder
	 * reveals. Several couldn't take effect (UA !important loses to author
	 * !important) and the blanket min-width/min-height/float overrides risk
	 * distorting unrelated layout. We add back only what a specific,
	 * confirmed bug needs. The editor textarea reveal now lives in the JS
	 * editor stub (inline style, which DOES outrank author !important). */

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
	/* fixes352a (#96) — heap-allocated body for data: URLs whose
	 * decoded content can be much larger than body_buf. Owned by ctx
	 * and freed in stub_free. Only used by SCH_DATA. */
	unsigned char *dyn_body;
	size_t dyn_body_len;
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
		/* fixes352a (#96) — serve the decoded body the setup path
		 * stashed in ctx->dyn_body. MIME comes out of ctx->path's
		 * before-comma section (e.g. "text/plain;charset=utf-8" →
		 * "text/plain"; default text/plain when none specified or
		 * decode failed). */
		if (ctx->dyn_body != NULL) {
			*body_out = (const char *)ctx->dyn_body;
			*len_out = ctx->dyn_body_len;
			if (strncmp(ctx->path, "text/html", 9) == 0) {
				*mime_out = "text/html";
			} else if (strncmp(ctx->path, "text/css", 8) == 0) {
				*mime_out = "text/css";
			} else if (strncmp(ctx->path, "text/", 5) == 0) {
				*mime_out = "text/plain";
			} else if (strncmp(ctx->path, "image/png", 9) == 0) {
				*mime_out = "image/png";
			} else if (strncmp(ctx->path, "image/jpeg", 10) == 0) {
				*mime_out = "image/jpeg";
			} else if (strncmp(ctx->path, "image/gif", 9) == 0) {
				*mime_out = "image/gif";
			} else {
				*mime_out = "text/plain";
			}
			return;
		}
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

	/* fixes335 (#99 #107) — diagnostic about: pages. */
	if (scheme == SCH_ABOUT) {
		int rn = 0;
		if (strncmp(ctx->path, "cache", 5) == 0 &&
		    (ctx->path[5] == '\0' || ctx->path[5] == '/')) {
			extern long macsurf__site_css_skip;
			extern unsigned long macsurf__site_css_total_bytes;
			rn = sprintf(ctx->body_buf,
				"<html><head><title>about:cache</title>"
				"<style>body{font-family:Geneva,sans-serif;"
				"background:#FFF4D0;color:#002030;padding:24px;}"
				"h1{color:#002030;font-size:20pt;}"
				"table{border-collapse:collapse;}"
				"td,th{border:1px solid #B05030;padding:4px 10px;"
				"text-align:left;}"
				"th{background:#E07010;color:#fff;}"
				"</style></head><body>"
				"<h1>about:cache</h1>"
				"<table>"
				"<tr><th>Metric</th><th>Value</th></tr>"
				"<tr><td>CSS bytes this page</td><td>%lu</td></tr>"
				"<tr><td>CSS sheets skipped</td><td>%ld</td></tr>"
				"</table>"
				"<p>HTTP cache is on-disk; entries persist across "
				"sessions. Press Reload (\xE2\x8C\x98R) to force "
				"a fresh fetch bypassing the cache.</p>"
				"</body></html>",
				(unsigned long)macsurf__site_css_total_bytes,
				(long)macsurf__site_css_skip);
		} else if (strncmp(ctx->path, "memory", 6) == 0 &&
			   (ctx->path[6] == '\0' || ctx->path[6] == '/')) {
			rn = sprintf(ctx->body_buf,
				"<html><head><title>about:memory</title>"
				"<style>body{font-family:Geneva,sans-serif;"
				"background:#FFF4D0;color:#002030;padding:24px;}"
				"h1{color:#002030;}p{line-height:1.5;}"
				"</style></head><body>"
				"<h1>about:memory</h1>"
				"<p>MacSurf is a Carbon CFM application with a "
				"16 MB preferred / 8 MB minimum partition. "
				"libcss / libdom / libhubbub each maintain their "
				"own arenas; memory usage scales with page DOM "
				"and CSS size.</p>"
				"<p>Bumping the Carbon partition in Get Info "
				"helps with large pages.</p>"
				"</body></html>");
		} else if (strncmp(ctx->path, "config", 6) == 0 &&
			   (ctx->path[6] == '\0' || ctx->path[6] == '/')) {
			rn = sprintf(ctx->body_buf,
				"<html><head><title>about:config</title>"
				"<style>body{font-family:Geneva,sans-serif;"
				"background:#FFF4D0;color:#002030;padding:24px;}"
				"h1{color:#002030;}table{border-collapse:collapse;}"
				"td,th{border:1px solid #B05030;padding:4px 10px;"
				"text-align:left;}th{background:#E07010;color:#fff;}"
				"</style></head><body>"
				"<h1>about:config</h1>"
				"<table>"
				"<tr><th>Option</th><th>Value</th></tr>"
				"<tr><td>enable_javascript</td><td>true</td></tr>"
				"<tr><td>foreground_images</td><td>true</td></tr>"
				"<tr><td>background_images</td><td>true</td></tr>"
				"<tr><td>author_level_css</td><td>true</td></tr>"
				"<tr><td>max_fetchers</td><td>128</td></tr>"
				"<tr><td>memory_cache_size</td><td>32 MB</td></tr>"
				"</table>"
				"<p>V1 is read-only. Future round will expose "
				"editable preferences.</p>"
				"</body></html>");
		} else if (strncmp(ctx->path, "perf", 4) == 0 &&
			   (ctx->path[4] == '\0' || ctx->path[4] == '/')) {
			extern long macsurf__site_reformat_ms;
			extern long macsurf__site_box_total;
			extern long macsurf__site_box_blk;
			extern long macsurf__site_box_inlinec;
			extern long macsurf__site_box_inline;
			extern long macsurf__site_box_text;
			extern long macsurf__site_box_other;
			extern long macsurf__site_img_ok;
			extern long macsurf__site_img_fail;
			extern long macsurf__site_css_ok;
			extern long macsurf__site_css_skip;
			extern unsigned long macsurf__site_css_total_bytes;
			rn = sprintf(ctx->body_buf,
				"<html><head><title>about:perf</title>"
				"<style>body{font-family:Geneva,sans-serif;"
				"background:#FFF4D0;color:#002030;padding:24px;}"
				"h1{color:#002030;}"
				"table{border-collapse:collapse;margin:12px 0;}"
				"th,td{border:1px solid #002030;padding:4px 10px;"
				"text-align:left;font-family:Monaco,monospace;"
				"font-size:11px;}"
				"th{background:#E07010;color:#fff;}"
				"</style></head>"
				"<body><h1>about:perf</h1>"
				"<p>Live counters from the most recently loaded "
				"page. Reformat_ms = wall-clock duration of the "
				"layout pass.</p>"
				"<table>"
				"<tr><th>Metric</th><th>Value</th></tr>"
				"<tr><td>reformat_ms</td><td>%ld</td></tr>"
				"<tr><td>box_total</td><td>%ld</td></tr>"
				"<tr><td>box_block</td><td>%ld</td></tr>"
				"<tr><td>box_inline_container</td><td>%ld</td></tr>"
				"<tr><td>box_inline</td><td>%ld</td></tr>"
				"<tr><td>box_text</td><td>%ld</td></tr>"
				"<tr><td>box_other</td><td>%ld</td></tr>"
				"<tr><td>img_ok</td><td>%ld</td></tr>"
				"<tr><td>img_fail</td><td>%ld</td></tr>"
				"<tr><td>css_ok</td><td>%ld</td></tr>"
				"<tr><td>css_skip</td><td>%ld</td></tr>"
				"<tr><td>css_total_bytes</td><td>%lu</td></tr>"
				"</table>"
				"<p>SITE log lines in MacSurf Debug.log (Desktop) "
				"carry the full per-fetch timing.</p>"
				"</body></html>",
				macsurf__site_reformat_ms,
				macsurf__site_box_total,
				macsurf__site_box_blk,
				macsurf__site_box_inlinec,
				macsurf__site_box_inline,
				macsurf__site_box_text,
				macsurf__site_box_other,
				macsurf__site_img_ok,
				macsurf__site_img_fail,
				macsurf__site_css_ok,
				macsurf__site_css_skip,
				macsurf__site_css_total_bytes);
		}
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

/* fixes352a (#96) — data: URL fetcher. Pre-fix this returned an empty
 * body, so any data: navigation (including View Source) silently went
 * nowhere. Now parses the URL per RFC 2397 (subset):
 *
 *   data:[<mediatype>][;base64],<data>
 *
 * V1 supports percent-decoding of the data segment only (no base64).
 * The MIME type is read out of the before-comma section and stored in
 * ctx->path's prefix; stub_body_for case SCH_DATA dispatches on it.
 * The decoded body is heap-allocated into ctx->dyn_body so very large
 * data: URLs (e.g. a 32 KB View Source dump) survive without inflating
 * the inline body_buf. */
static int hex_digit(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	return -1;
}

static void *
stub_setup_data(struct fetch *parent_fetch, struct nsurl *url,
		bool only_2xx, bool downgrade_tls,
		const char *post_urlenc,
		const struct fetch_multipart_data *post_multipart,
		const char **headers)
{
	struct stub_fetch_ctx *ctx;
	const char *url_str;
	const char *after_scheme;
	const char *comma;
	const char *encoded;
	size_t enc_len;
	size_t i, j;
	size_t mime_n;
	unsigned char *body;

	(void)only_2xx; (void)downgrade_tls;
	(void)post_urlenc; (void)post_multipart; (void)headers;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) return NULL;
	ctx->parent = parent_fetch;
	ctx->scheme = SCH_DATA;

	url_str = nsurl_access(url);
	if (url_str == NULL) goto bail_min;

	/* Skip "data:" */
	after_scheme = strchr(url_str, ':');
	if (after_scheme == NULL) goto bail_min;
	after_scheme++;

	/* Find the comma separating mediatype from data. Do this on the
	 * RAW url_str (could be far past ctx->path's 1024-byte truncation). */
	comma = strchr(after_scheme, ',');
	if (comma == NULL) goto bail_min;

	/* Store the mediatype portion (before the comma) in ctx->path so
	 * stub_body_for can sniff it. Path is bounded at 1024 bytes; MIME
	 * types are well under that. */
	mime_n = (size_t)(comma - after_scheme);
	if (mime_n >= sizeof(ctx->path)) mime_n = sizeof(ctx->path) - 1;
	memcpy(ctx->path, after_scheme, mime_n);
	ctx->path[mime_n] = '\0';

	encoded = comma + 1;
	enc_len = strlen(encoded);

	body = (unsigned char *)malloc(enc_len + 1);
	if (body == NULL) goto bail_min;

	j = 0;
	for (i = 0; i < enc_len; i++) {
		if (encoded[i] == '%' && i + 2 < enc_len) {
			int hi = hex_digit(encoded[i+1]);
			int lo = hex_digit(encoded[i+2]);
			if (hi >= 0 && lo >= 0) {
				body[j++] = (unsigned char)((hi << 4) | lo);
				i += 2;
				continue;
			}
			body[j++] = (unsigned char)encoded[i];
		} else if (encoded[i] == '+') {
			body[j++] = ' ';
		} else {
			body[j++] = (unsigned char)encoded[i];
		}
	}
	body[j] = '\0';

	ctx->dyn_body = body;
	ctx->dyn_body_len = j;

bail_min:
	stub_ring_insert(ctx);
	MS_LOG("stub setup");
	return ctx;
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
	/* fixes352a (#96) — free the heap-allocated data: body if any. */
	if (ctx->dyn_body != NULL) {
		free(ctx->dyn_body);
		ctx->dyn_body = NULL;
		ctx->dyn_body_len = 0;
	}
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
