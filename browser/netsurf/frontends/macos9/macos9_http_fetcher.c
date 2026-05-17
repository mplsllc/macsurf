#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "utils/errors.h"
#include "utils/log.h"
#include "utils/nsurl.h"
#include "content/fetch.h"
#include "content/fetchers.h"
#include "macsurf_debug.h"
#ifdef __MACOS9__
#include <Files.h>
#include <OpenTransport.h>
#include <OpenTptInternet.h>
#include <Threads.h>
extern OTClientContextPtr macos9_ot_context;
#endif

#define PROXY_H "116.202.231.103"
#define PROXY_P 8765
#define MAX_F 64
/* fixes92 — bigger OTRcv buffer cuts the syscall count for large bodies.
 * 32 KB picks ~1 OTRcv per typical sub-resource response and ~3-4 per
 * the main HTML on MacTrove. Mac OS 9 stack frames are fine with this. */
#define RECV_B 32768
/* fixes92 — pool 16 simultaneous keep-alive sockets to the proxy.
 * NetSurf may have up to 16 fetches per host in flight after fixes91's
 * max_fetchers_per_host bump; 16 here matches. */
#define POOL_SIZE 16

/* fixes91 — new state MFS_QUEUED:
 * NetSurf core's fetch_dispatch_jobs() gates on max_fetchers / per-host
 * limits before calling ops.start. The HTTP fetcher previously sat in
 * MFS_INIT from setup-time and mfs_poll_one would run the OT state
 * machine regardless — so its slots stayed non-IDLE for fetches NetSurf
 * had never dispatched, eventually saturating the slot table. Slots
 * now start in MFS_QUEUED (no OT activity) and only transition to
 * MFS_INIT when ops.start is called, so slot accounting matches
 * NetSurf's fetch_ring/queue_ring view. */
enum mfs_state {
	MFS_IDLE,
	MFS_QUEUED,
	MFS_INIT,
	MFS_HEADERS,
	MFS_BODY,
	MFS_DONE,
	MFS_FAIL,
	MFS_NOTIFIED
};

/* fixes98 — chunked transfer-encoding decoder states.
 *   CS_SIZE    — reading hex chunk-size line up to \n
 *   CS_DATA    — copying chunk_remaining bytes as FETCH_DATA
 *   CS_CRLF    — consuming the \r\n that terminates a chunk's data
 *   CS_TRAILER — after final 0-size chunk, eating trailer headers
 *                until an empty line
 *   CS_DONE    — full body received; mfs_poll_one will transition the
 *                fetch to MFS_DONE on the next pass
 */
enum chunk_state {
	CS_SIZE = 0,
	CS_DATA,
	CS_CRLF,
	CS_TRAILER,
	CS_DONE
};

/* fixes94 — pool_key remembers which (host, port) this slot's endpoint
 * was opened to. Returned to the matching bucket on close. Direct
 * origins ("frogfind.com:80") and the proxy ("PROXY") each get their
 * own pool entries so reuse only happens on identical targets. */
#define POOL_KEY_LEN 96
struct macos9_fetch_ctx {
	struct fetch *parent; struct nsurl *url; enum mfs_state state;
	int redirects; int aborted;
#ifdef __MACOS9__
	EndpointRef ep;
#endif
	char *h_buf; long h_len; long h_cap;
	int status; char mime[128]; const char *err;
	long body_bytes; /* fixes311: total bytes delivered as FETCH_DATA after headers */
	long content_length; /* fixes91: -1 = unknown */
	int keep_alive_ok;   /* fixes91: 1 if endpoint can be reused after response */
	int chunked;         /* fixes91: 1 if Transfer-Encoding: chunked */
	char pool_key[POOL_KEY_LEN]; /* fixes94: host:port of this fetch's endpoint */
	/* fixes98 — 3xx auto-follow. Location header captured during header
	 * parsing; if status is 3xx we emit FETCH_REDIRECT instead of body
	 * delivery and NetSurf's llcache opens a fresh fetch against this
	 * URL (relative resolution happens in llcache). */
	char redirect_url[512];
	/* fixes98 — chunked decoder. Active when chunked==1. */
	int chunk_state;            /* enum chunk_state */
	long chunk_remaining;       /* bytes left in current chunk's data */
	char chunk_size_buf[16];    /* hex line buffer */
	int chunk_size_len;
	int trailer_just_after_eol; /* tracks empty-line in CS_TRAILER */
};
static struct macos9_fetch_ctx f_slots[MAX_F];

#ifdef __MACOS9__
/* fixes94 — keyed endpoint pool. Previously all entries were assumed
 * to be proxy connections; now we route http:// directly to the
 * origin, so each pool entry needs to remember its (host, port). */
struct ep_pool_entry {
	EndpointRef ep;
	char key[POOL_KEY_LEN];
};
static struct ep_pool_entry ep_pool[POOL_SIZE];
static int ep_pool_count = 0;

static EndpointRef
ep_pool_take(const char *key)
{
	int i;
	for (i = ep_pool_count - 1; i >= 0; i--) {
		OTResult look;
		EndpointRef ep;

		if (strcmp(ep_pool[i].key, key) != 0) continue;
		ep = ep_pool[i].ep;
		/* Compact: move tail entry into this slot. */
		ep_pool[i] = ep_pool[ep_pool_count - 1];
		ep_pool_count--;

		/* OTLook reports any pending event without consuming
		 * data. 0 means the endpoint is in T_DATAXFER with no
		 * pending events — healthy idle, safe to reuse. */
		look = OTLook(ep);
		if (look == 0) {
			macsurf_debug_log_writef(
				"ep_pool: REUSE key=%s remaining=%d",
				key, ep_pool_count);
			return ep;
		}
		macsurf_debug_log_writef(
			"ep_pool: discard key=%s look=%ld remaining=%d",
			key, (long)look, ep_pool_count);
		if (look == T_ORDREL) {
			OTRcvOrderlyDisconnect(ep);
		} else if (look == T_DISCONNECT) {
			OTRcvDisconnect(ep, NULL);
		}
		OTSndOrderlyDisconnect(ep);
		OTCloseProvider(ep);
		/* Restart scan from new top — index i may now refer to a
		 * different entry after compaction. */
		i = ep_pool_count;
	}
	return NULL;
}

static void
ep_pool_return(const char *key, EndpointRef ep)
{
	char drain[256];
	OTResult n;
	long drained = 0;

	if (ep == NULL) return;
	if (ep_pool_count >= POOL_SIZE) {
		macsurf_debug_log_writef(
			"ep_pool: full, closing (count=%d key=%s)",
			ep_pool_count, key);
		OTSndOrderlyDisconnect(ep);
		OTCloseProvider(ep);
		return;
	}

	/* Drain any trailing bytes so the next user of this endpoint
	 * sees a clean stream starting at the next response. */
	OTSetNonBlocking(ep);
	do {
		n = OTRcv(ep, drain, sizeof(drain), NULL);
		if (n > 0) drained += n;
	} while (n > 0);

	ep_pool[ep_pool_count].ep = ep;
	strncpy(ep_pool[ep_pool_count].key, key, POOL_KEY_LEN - 1);
	ep_pool[ep_pool_count].key[POOL_KEY_LEN - 1] = '\0';
	ep_pool_count++;
	macsurf_debug_log_writef(
		"ep_pool: STORED key=%s count=%d drained=%ld",
		key, ep_pool_count, drained);
}
#endif /* __MACOS9__ */

static char *mfs_find_line(char **buf, long *len) {
	char *start = *buf, *p = *buf, *end = *buf + *len;
	while(p < end-1) {
		if(p[0]=='\r' && p[1]=='\n') { *p = 0; *buf = p + 2; *len = (long)(end - *buf); return start; }
		p++;
	}
	return NULL;
}

static void mfs_close(struct macos9_fetch_ctx *c) {
#ifdef __MACOS9__
	if (c->ep) {
		/* fixes92 — fixes91's keep-alive was DOA. mfs_close runs from
		 * macos9_http_free, by which time macos9_http_poll has already
		 * transitioned state MFS_DONE → MFS_NOTIFIED, so the old check
		 * `state == MFS_DONE` always failed and every endpoint got
		 * closed regardless of keep_alive_ok. That means fixes91 paid
		 * 44 fresh OTConnects per MacTrove page (~the dial-up feel).
		 * The real eligibility test is just keep_alive_ok && !aborted. */
		if (c->keep_alive_ok && !c->aborted && c->pool_key[0] != '\0') {
			ep_pool_return(c->pool_key, c->ep);
		} else {
			OTSndOrderlyDisconnect(c->ep);
			OTCloseProvider(c->ep);
		}
		c->ep = NULL;
	}
#endif
}

static int mfs_open(struct macos9_fetch_ctx *c) {
#ifdef __MACOS9__
	OSStatus e; OTConfigurationRef cfg; DNSAddress dns; TCall call;
	char target[96];        /* host:port for OTInitDNSAddress + pool key */
	char path_buf[1024];    /* path?query for direct-mode request line */
	char req[2048];
	OTResult r;
	lwc_string *scheme_lwc;
	lwc_string *host_lwc;
	lwc_string *port_lwc;
	lwc_string *path_lwc;
	lwc_string *query_lwc;
	const char *scheme_str;
	const char *host_str;
	size_t host_len;
	size_t scheme_len;
	int port_num;
	int use_proxy;
	EndpointRef pooled;
	const char *u_full;

	scheme_lwc = NULL;
	host_lwc = NULL;
	port_lwc = NULL;
	path_lwc = NULL;
	query_lwc = NULL;

	scheme_lwc = nsurl_get_component(c->url, NSURL_SCHEME);
	host_lwc = nsurl_get_component(c->url, NSURL_HOST);
	if (scheme_lwc == NULL || host_lwc == NULL) {
		MS_LOG("mfs_open: missing scheme/host");
		goto fail_unref;
	}

	scheme_str = lwc_string_data(scheme_lwc);
	scheme_len = lwc_string_length(scheme_lwc);
	host_str = lwc_string_data(host_lwc);
	host_len = lwc_string_length(host_lwc);

	/* fixes94 — only HTTPS routes through the proxy (we have no TLS).
	 * Plain HTTP goes direct to the origin, saving the round-trip to
	 * the Hetzner box for ~every fetch on http-only sites. */
	use_proxy = (scheme_len == 5 && strncmp(scheme_str, "https", 5) == 0);

	if (use_proxy) {
		sprintf(target, "%s:%d", PROXY_H, PROXY_P);
		strcpy(c->pool_key, "PROXY");
	} else {
		port_lwc = nsurl_get_component(c->url, NSURL_PORT);
		if (port_lwc != NULL && lwc_string_length(port_lwc) > 0) {
			port_num = atoi(lwc_string_data(port_lwc));
			if (port_num <= 0 || port_num > 65535) port_num = 80;
		} else {
			port_num = 80;
		}
		if (host_len + 8 >= sizeof(target)) {
			MS_LOG("mfs_open: host too long");
			goto fail_unref;
		}
		sprintf(target, "%.*s:%d", (int)host_len, host_str, port_num);
		strncpy(c->pool_key, target, POOL_KEY_LEN - 1);
		c->pool_key[POOL_KEY_LEN - 1] = '\0';
	}

	MS_LOG("mfs_open: enter");
	pooled = ep_pool_take(c->pool_key);
	if (pooled != NULL) {
		c->ep = pooled;
		OTSetSynchronous(c->ep);
		OTSetBlocking(c->ep);
	} else {
		cfg = OTCreateConfiguration("tcp");
		if (!cfg) {
			macsurf_debug_log_writef(
				"mfs_open: OTCreateConfig FAIL target=%s", target);
			goto fail_unref;
		}
		c->ep = OTOpenEndpointInContext(cfg, 0, NULL, &e, macos9_ot_context);
		if (e != noErr || !c->ep) {
			macsurf_debug_log_writef(
				"mfs_open: OTOpenEndpoint err=%d target=%s",
				(int)e, target);
			goto fail_unref;
		}
		OTSetSynchronous(c->ep);
		OTSetBlocking(c->ep);
		if (OTBind(c->ep, NULL, NULL) != noErr) {
			MS_LOG("mfs_open: OTBind FAIL");
			goto fail_unref;
		}
		OTMemzero(&call, sizeof(TCall));
		call.addr.buf = (UInt8 *)&dns;
		call.addr.len = (short)OTInitDNSAddress(&dns, target);
		macsurf_debug_log_writef("mfs_open: OTConnect %s", target);
		if (OTConnect(c->ep, &call, NULL) != noErr) {
			macsurf_debug_log_writef(
				"mfs_open: OTConnect FAIL target=%s", target);
			goto fail_unref;
		}
	}

	/* Build the request line.
	 *   proxy: absolute-form  (GET http://example.com/foo HTTP/1.1)
	 *   direct: origin-form   (GET /foo HTTP/1.1)
	 * In both cases Host: holds the origin hostname.
	 *
	 * fixes95: direct fetches send `Connection: close` and skip the
	 * pool. The proxy buffers responses and emits Content-Length so
	 * we can frame them and keep-alive cleanly. Origin servers
	 * (e.g. nginx) reply with Transfer-Encoding: chunked + keep-alive
	 * for HTML; we don't have a chunked decoder yet, so without a
	 * Content-Length we can't tell where one response ends on a
	 * reused socket — the fetch just hangs. Connection: close makes
	 * the origin close after the response, which OTRcv reports as
	 * n==0 and we transition to MFS_DONE. Cost: one TCP setup per
	 * direct fetch. Still saves the Hetzner round-trip vs proxy. */
	if (use_proxy) {
		u_full = nsurl_access(c->url);
		macsurf_debug_log_writef("mfs_open: GET (proxy) %s",
				u_full ? u_full : "(null)");
		sprintf(req,
			"GET %s HTTP/1.1\r\n"
			"Host: %.*s\r\n"
			"User-Agent: MacSurf/0.2\r\n"
			"Accept: */*\r\n"
			"Connection: keep-alive\r\n\r\n",
			u_full, (int)host_len, host_str);
	} else {
		path_lwc = nsurl_get_component(c->url, NSURL_PATH);
		query_lwc = nsurl_get_component(c->url, NSURL_QUERY);
		{
			const char *p_str = NULL;
			size_t p_len = 0;
			const char *q_str = NULL;
			size_t q_len = 0;
			if (path_lwc != NULL && lwc_string_length(path_lwc) > 0) {
				p_str = lwc_string_data(path_lwc);
				p_len = lwc_string_length(path_lwc);
			}
			if (query_lwc != NULL && lwc_string_length(query_lwc) > 0) {
				q_str = lwc_string_data(query_lwc);
				q_len = lwc_string_length(query_lwc);
			}
			if (p_str == NULL) {
				strcpy(path_buf, "/");
			} else if (q_str == NULL) {
				if (p_len >= sizeof(path_buf)) p_len = sizeof(path_buf) - 1;
				memcpy(path_buf, p_str, p_len);
				path_buf[p_len] = '\0';
			} else {
				size_t total = p_len + 1 + q_len;
				if (total >= sizeof(path_buf)) total = sizeof(path_buf) - 1;
				sprintf(path_buf, "%.*s?%.*s",
					(int)p_len, p_str,
					(int)q_len, q_str);
				path_buf[sizeof(path_buf) - 1] = '\0';
				(void)total;
			}
		}
		macsurf_debug_log_writef("mfs_open: GET (direct) %s %s",
				target, path_buf);
		/* fixes98: direct requests now use keep-alive — the chunked
		 * decoder in mfs_poll_one / process_chunked_bytes frames
		 * Transfer-Encoding: chunked responses correctly so we don't
		 * need the server to close after each. Pool reuse on direct
		 * origins eliminates per-fetch TCP setup latency. */
		sprintf(req,
			"GET %s HTTP/1.1\r\n"
			"Host: %.*s\r\n"
			"User-Agent: MacSurf/0.2\r\n"
			"Accept: */*\r\n"
			"Connection: keep-alive\r\n\r\n",
			path_buf, (int)host_len, host_str);
	}

	r = OTSnd(c->ep, req, (long)strlen(req), 0);
	if (r < 0) {
		macsurf_debug_log_writef("mfs_open: OTSnd err=%ld", (long)r);
		goto fail_unref;
	}
	OTSetNonBlocking(c->ep);

	if (scheme_lwc != NULL) lwc_string_unref(scheme_lwc);
	if (host_lwc != NULL) lwc_string_unref(host_lwc);
	if (port_lwc != NULL) lwc_string_unref(port_lwc);
	if (path_lwc != NULL) lwc_string_unref(path_lwc);
	if (query_lwc != NULL) lwc_string_unref(query_lwc);
	return 1;

fail_unref:
	/* fixes94 — if we opened an endpoint but failed before OTSnd,
	 * close it immediately so mfs_close doesn't try to pool a
	 * half-built connection (pool_key may already be set). */
	if (c->ep != NULL) {
		OTSndOrderlyDisconnect(c->ep);
		OTCloseProvider(c->ep);
		c->ep = NULL;
	}
	c->keep_alive_ok = 0;
	if (scheme_lwc != NULL) lwc_string_unref(scheme_lwc);
	if (host_lwc != NULL) lwc_string_unref(host_lwc);
	if (port_lwc != NULL) lwc_string_unref(port_lwc);
	if (path_lwc != NULL) lwc_string_unref(path_lwc);
	if (query_lwc != NULL) lwc_string_unref(query_lwc);
	return 0;
#else
	return 0;
#endif
}

/* fixes98 — chunked decoder. Drives the chunk_state state machine over
 * a buffer of raw post-header bytes. Emits FETCH_DATA for the data
 * regions; consumes the size lines, CRLFs, and trailer headers without
 * forwarding them. Reentrant across calls — partial chunks are
 * remembered in c->chunk_state / chunk_remaining / chunk_size_buf, so
 * an OTRcv that splits mid-line works correctly on the next pass.
 * When the terminating empty trailer line is seen, sets chunk_state to
 * CS_DONE; mfs_poll_one then transitions state to MFS_DONE. */
static void
process_chunked_bytes(struct macos9_fetch_ctx *c, const char *b, long len)
{
	fetch_msg msg;
	long pos;
	char ch;

	pos = 0;
	while (pos < len && c->chunk_state != CS_DONE) {
		switch (c->chunk_state) {
		case CS_SIZE: {
			while (pos < len) {
				ch = b[pos++];
				if (ch == '\n') {
					long sz;
					c->chunk_size_buf[c->chunk_size_len] = '\0';
					sz = strtol(c->chunk_size_buf, NULL, 16);
					c->chunk_size_len = 0;
					c->chunk_remaining = sz;
					if (sz == 0) {
						c->chunk_state = CS_TRAILER;
						c->trailer_just_after_eol = 1;
					} else {
						c->chunk_state = CS_DATA;
					}
					break;
				}
				if (ch == '\r') continue;
				/* Drop chunk extensions after ';' — stay
				 * inside the buffer so strtol stops at the
				 * first non-hex char anyway, but avoid
				 * overflowing chunk_size_buf on a long ext. */
				if (c->chunk_size_len <
				    (int)sizeof(c->chunk_size_buf) - 1) {
					c->chunk_size_buf[c->chunk_size_len++] = ch;
				}
			}
			break;
		}
		case CS_DATA: {
			long avail;
			long deliver;
			avail = len - pos;
			deliver = (avail < c->chunk_remaining) ? avail : c->chunk_remaining;
			if (deliver > 0) {
				msg.type = FETCH_DATA;
				msg.data.header_or_data.buf = (const uint8_t *)(b + pos);
				msg.data.header_or_data.len = (size_t)deliver;
				fetch_send_callback(&msg, c->parent);
				c->body_bytes += deliver;
				pos += deliver;
				c->chunk_remaining -= deliver;
			}
			if (c->chunk_remaining == 0) {
				c->chunk_state = CS_CRLF;
			}
			break;
		}
		case CS_CRLF: {
			while (pos < len) {
				ch = b[pos++];
				if (ch == '\n') {
					c->chunk_state = CS_SIZE;
					break;
				}
				/* skip \r and any stray bytes */
			}
			break;
		}
		case CS_TRAILER: {
			while (pos < len) {
				ch = b[pos++];
				if (ch == '\r') continue;
				if (ch == '\n') {
					if (c->trailer_just_after_eol) {
						c->chunk_state = CS_DONE;
						break;
					}
					c->trailer_just_after_eol = 1;
				} else {
					c->trailer_just_after_eol = 0;
				}
			}
			break;
		}
		}
	}
}

static void mfs_parse_headers(struct macos9_fetch_ctx *c) {
	char *sep = strstr(c->h_buf, "\r\n\r\n"), *p, *cur; long cur_len; fetch_msg msg;
	if(!sep) return;
	*sep = 0; cur = c->h_buf; cur_len = (long)(sep - c->h_buf) + 2;
	p = mfs_find_line(&cur, &cur_len);
	if(p && strncmp(p,"HTTP/",5)==0) {
		char *sp=strchr(p,' '); if(sp) c->status=atoi(sp+1);
		/* UNLOCK HISTORY: NetSurf core needs the status line reported as a header */
		msg.type=FETCH_HEADER; msg.data.header_or_data.buf=(const uint8_t*)p;
		msg.data.header_or_data.len=strlen(p); fetch_send_callback(&msg,c->parent);
	}
	fetch_set_http_code(c->parent, c->status);
	while((p = mfs_find_line(&cur, &cur_len)) != NULL) {
		if(p[0]==0) break;
		if(strncasecmp(p,"Content-Type:",13)==0) {
			char *v=p+13; while(*v==' ')v++;
			strncpy(c->mime,v,127); c->mime[127]=0;
		}
		/* fixes91 — parse Content-Length so we know when the body
		 * ends without waiting for the server to close. */
		if(strncasecmp(p,"Content-Length:",15)==0) {
			char *v=p+15; while(*v==' ')v++;
			c->content_length = atol(v);
		}
		if(strncasecmp(p,"Transfer-Encoding:",18)==0) {
			char *v=p+18; while(*v==' ')v++;
			if(strncasecmp(v,"chunked",7)==0) c->chunked = 1;
		}
		if(strncasecmp(p,"Connection:",11)==0) {
			char *v=p+11; while(*v==' ')v++;
			if(strncasecmp(v,"close",5)==0) c->keep_alive_ok = 0;
		}
		/* fixes98 — capture Location: for 3xx auto-follow. */
		if(strncasecmp(p,"Location:",9)==0) {
			char *v=p+9; size_t lv;
			while(*v==' '||*v=='\t')v++;
			lv = strlen(v);
			if (lv >= sizeof(c->redirect_url)) lv = sizeof(c->redirect_url) - 1;
			memcpy(c->redirect_url, v, lv);
			c->redirect_url[lv] = '\0';
		}
		msg.type=FETCH_HEADER; msg.data.header_or_data.buf=(const uint8_t*)p;
		msg.data.header_or_data.len=strlen(p); fetch_send_callback(&msg,c->parent);
	}
	/* fixes98 — 3xx with Location: hand the redirect target to llcache
	 * via FETCH_REDIRECT and stop the body delivery. We don't bother
	 * draining the (usually-tiny "301 Moved Permanently") body because
	 * we're closing the socket — pool reuse on a 3xx isn't worth the
	 * decoder complexity. */
	if (c->status >= 300 && c->status < 400 && c->redirect_url[0] != '\0') {
		msg.type = FETCH_REDIRECT;
		msg.data.redirect = c->redirect_url;
		fetch_send_callback(&msg, c->parent);
		macsurf_debug_log_writef("http: redirect %d -> %s",
			c->status, c->redirect_url);
		c->state = MFS_NOTIFIED;
		c->keep_alive_ok = 0;
		free(c->h_buf); c->h_buf = NULL;
		return;
	}
	/* fixes98 — keep-alive is now compatible with chunked (the decoder
	 * tells us where the body ends). Only Content-Length-less *plain*
	 * responses force close. */
	if (c->content_length < 0 && !c->chunked) c->keep_alive_ok = 0;
	/* fixes98 — init chunked decoder state. */
	if (c->chunked) {
		c->chunk_state = CS_SIZE;
		c->chunk_size_len = 0;
		c->chunk_remaining = 0;
	}
	c->state=MFS_BODY;
	if(sep+4 < c->h_buf+c->h_len) {
		long initial_body = (long)((c->h_buf+c->h_len)-(sep+4));
		if (c->chunked) {
			process_chunked_bytes(c, sep+4, initial_body);
			if (c->chunk_state == CS_DONE) c->state = MFS_DONE;
		} else {
			long deliver = initial_body;
			if (c->content_length >= 0 && c->body_bytes + deliver > c->content_length)
				deliver = c->content_length - c->body_bytes;
			if (deliver > 0) {
				msg.type=FETCH_DATA; msg.data.header_or_data.buf=(const uint8_t*)(sep+4);
				msg.data.header_or_data.len=(size_t)deliver;
				fetch_send_callback(&msg,c->parent);
				c->body_bytes += deliver;
			}
			if (c->content_length >= 0 && c->body_bytes >= c->content_length) {
				c->state = MFS_DONE;
			}
		}
	}
	free(c->h_buf); c->h_buf=NULL;
}

static void mfs_poll_one(struct macos9_fetch_ctx *c) {
#ifdef __MACOS9__
	/* fixes92 — static so we can afford a 32 KB buffer without stack
	 * pressure. macos9_http_poll calls mfs_poll_one sequentially in a
	 * single-threaded loop, and OTRcv in non-blocking mode never yields,
	 * so this is reentrancy-safe under MacSurf's cooperative model. */
	static char b[RECV_B];
	OTResult n; fetch_msg m;
	/* fixes91 — MFS_QUEUED means NetSurf hasn't dispatched yet (ops.start
	 * unfired); don't open OT until then. */
	if(c->state==MFS_IDLE || c->state==MFS_QUEUED || c->state==MFS_NOTIFIED) return;
	if(c->aborted) { c->state=MFS_DONE; c->keep_alive_ok=0; return; }
	if(c->state==MFS_INIT) { if(mfs_open(c)) c->state=MFS_HEADERS; else c->state=MFS_FAIL; return; }
	n=OTRcv(c->ep,b,sizeof(b),NULL);
	if(n==kOTNoDataErr) return;
	if(n<0) {
		if(n==kOTLookErr) {
			OTResult l=OTLook(c->ep);
			if(l==T_ORDREL||l==T_DISCONNECT) {
				/* Server closed before we hit Content-Length —
				 * treat as done but don't pool. */
				c->keep_alive_ok = 0;
				c->state=MFS_DONE; return;
			}
		}
		c->err="OT error"; c->state=MFS_FAIL; return;
	}
	if(n==0) { c->keep_alive_ok=0; c->state=MFS_DONE; return; }
	if(c->state==MFS_HEADERS) {
		long nl=c->h_len+n;
		if(nl>c->h_cap) { long nc=c->h_cap==0?4096:c->h_cap*2; while(nc<nl)nc*=2; c->h_buf=realloc(c->h_buf,nc); if(c->h_buf) c->h_cap=nc; }
		if(!c->h_buf) { c->err="OOM"; c->state=MFS_FAIL; return; }
		memcpy(c->h_buf+c->h_len,b,(size_t)n); c->h_len=nl;
		if(strstr(c->h_buf,"\r\n\r\n")) mfs_parse_headers(c);
	} else {
		if (c->chunked) {
			process_chunked_bytes(c, b, (long)n);
			if (c->chunk_state == CS_DONE) c->state = MFS_DONE;
		} else {
			long deliver = n;
			/* fixes91 — cap delivery at Content-Length so we don't bleed
			 * into the next pipelined response. */
			if (c->content_length >= 0 && c->body_bytes + deliver > c->content_length)
				deliver = c->content_length - c->body_bytes;
			if (deliver > 0) {
				m.type=FETCH_DATA; m.data.header_or_data.buf=(const uint8_t*)b;
				m.data.header_or_data.len=(size_t)deliver;
				fetch_send_callback(&m,c->parent);
				c->body_bytes += deliver;
			}
			if (c->content_length >= 0 && c->body_bytes >= c->content_length) {
				c->state = MFS_DONE;
			}
		}
	}
#endif
}

static void macos9_http_poll(lwc_string *s) {
	int i; (void)s;
	for(i=0;i<MAX_F;i++) {
		struct macos9_fetch_ctx *c = &f_slots[i];
		if(c->state==MFS_IDLE || c->state==MFS_QUEUED || c->state==MFS_NOTIFIED) continue;
		mfs_poll_one(c);
		if(c->state==MFS_FAIL) {
			fetch_msg m; m.type=FETCH_ERROR; m.data.error=c->err;
			c->state=MFS_NOTIFIED; fetch_send_callback(&m,c->parent);
			macsurf_debug_log_writef("http: fail body_bytes=%ld status=%d", c->body_bytes, c->status);
		}
		else if(c->state==MFS_DONE) {
			fetch_msg m; m.type=FETCH_FINISHED;
			c->state=MFS_NOTIFIED; fetch_send_callback(&m,c->parent);
			macsurf_debug_log_writef("http: done body=%ld len=%ld status=%d ka=%d", c->body_bytes, c->content_length, c->status, c->keep_alive_ok);
			/* fixes99 — pool the endpoint right now, while it is
			 * known idle and the next fetch may already be queued.
			 * Waiting for macos9_http_free (which NetSurf calls
			 * lazily — slots routinely linger in MFS_NOTIFIED
			 * across several subsequent http_setup calls) means
			 * the pool is always empty when a new fetch wants it
			 * and every sub-resource pays a fresh TCP setup. By
			 * returning here, the very next mfs_open against the
			 * same host can REUSE. mfs_close still runs at free
			 * time but sees c->ep==NULL and skips the close. */
#ifdef __MACOS9__
			if (c->keep_alive_ok && !c->aborted &&
			    c->ep != NULL && c->pool_key[0] != '\0') {
				ep_pool_return(c->pool_key, c->ep);
				c->ep = NULL;
			}
#endif
		}
	}
}

static bool macos9_http_initialise(lwc_string *s) { (void)s; return true; }
static void macos9_http_finalise(lwc_string *s) { (void)s; }
static bool macos9_http_acceptable(const struct nsurl *u) { (void)u; return true; }
static void *macos9_http_setup(struct fetch *p, struct nsurl *u, bool o, bool d, const char *pu, const struct fetch_multipart_data *pm, const char **h) {
	int i; (void)o;(void)d;(void)pu;(void)pm;(void)h;
	for(i=0;i<MAX_F;i++) if(f_slots[i].state==MFS_IDLE) {
		memset(&f_slots[i],0,sizeof(f_slots[0]));
		f_slots[i].parent=p; f_slots[i].url=nsurl_ref(u);
		/* fixes91 — was MFS_INIT; defer OT until ops.start. */
		f_slots[i].state=MFS_QUEUED;
		f_slots[i].content_length=-1;
		f_slots[i].keep_alive_ok=1;
		macsurf_debug_log_writef("http_setup: slot=%d/%d", i, MAX_F);
		return &f_slots[i];
	}
	macsurf_debug_log_writef("http_setup: NO FREE SLOTS (MAX_F=%d) - fetch will FAIL", MAX_F);
	return NULL;
}
static bool macos9_http_start(void *ctx) {
	struct macos9_fetch_ctx *c = (struct macos9_fetch_ctx*)ctx;
	/* fixes91 — transition QUEUED→INIT so mfs_poll_one will open OT. */
	if (c != NULL && c->state == MFS_QUEUED) c->state = MFS_INIT;
	return true;
}
static void macos9_http_abort(void *ctx) { ((struct macos9_fetch_ctx*)ctx)->aborted=1; }
static void macos9_http_free(void *ctx) {
	struct macos9_fetch_ctx *c = (struct macos9_fetch_ctx*)ctx;
	mfs_close(c); if(c->h_buf) free(c->h_buf); if(c->url) nsurl_unref(c->url); c->state=MFS_IDLE;
}

nserror macos9_http_fetcher_register(void) {
	struct fetcher_operation_table ops; lwc_string *sh, *ss;
	ops.initialise=macos9_http_initialise; ops.acceptable=macos9_http_acceptable;
	ops.setup=macos9_http_setup; ops.start=macos9_http_start; ops.abort=macos9_http_abort;
	ops.free=macos9_http_free; ops.poll=macos9_http_poll; ops.fdset=NULL; ops.finalise=macos9_http_finalise;
	lwc_intern_string("http",4,&sh); lwc_intern_string("https",5,&ss);
	fetcher_add(sh,&ops); fetcher_add(ss,&ops); return NSERROR_OK;
}
