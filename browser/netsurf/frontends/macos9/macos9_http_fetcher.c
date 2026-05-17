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
#define RECV_B 8192
#define POOL_SIZE 8

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
};
static struct macos9_fetch_ctx f_slots[MAX_F];

#ifdef __MACOS9__
/* fixes91 — endpoint pool for HTTP/1.1 keep-alive to the proxy.
 * Every HTTP fetch through MacSurf currently dials the same proxy
 * (PROXY_H:PROXY_P), so we don't key by host — the whole pool serves
 * one (host, port) pair. */
static EndpointRef ep_pool[POOL_SIZE];
static int ep_pool_count = 0;

static EndpointRef
ep_pool_take(void)
{
	while (ep_pool_count > 0) {
		EndpointRef ep = ep_pool[--ep_pool_count];
		OTResult look;

		/* OTLook reports any pending events without consuming
		 * data. 0 means the endpoint is in T_DATAXFER with no
		 * pending events — healthy idle, safe to reuse. */
		look = OTLook(ep);
		if (look == 0) {
			return ep;
		}
		/* Peer signalled shutdown / leftover bytes / etc.
		 * Close cleanly and try the next pooled endpoint. */
		if (look == T_ORDREL) {
			OTRcvOrderlyDisconnect(ep);
		} else if (look == T_DISCONNECT) {
			OTRcvDisconnect(ep, NULL);
		}
		OTSndOrderlyDisconnect(ep);
		OTCloseProvider(ep);
	}
	return NULL;
}

static void
ep_pool_return(EndpointRef ep)
{
	char drain[256];
	OTResult n;

	if (ep == NULL) return;
	if (ep_pool_count >= POOL_SIZE) {
		OTSndOrderlyDisconnect(ep);
		OTCloseProvider(ep);
		return;
	}

	/* Drain any trailing bytes so the next user of this endpoint
	 * sees a clean stream starting at the next response. */
	OTSetNonBlocking(ep);
	do {
		n = OTRcv(ep, drain, sizeof(drain), NULL);
	} while (n > 0);

	ep_pool[ep_pool_count++] = ep;
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
		if (c->state == MFS_DONE && c->keep_alive_ok) {
			ep_pool_return(c->ep);
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
	char pstr[64], req[2048]; const char *u; OTResult r;
	lwc_string *host_lwc;
	const char *host_str;
	size_t host_len;
	EndpointRef pooled;

	MS_LOG("mfs_open: enter");
	pooled = ep_pool_take();
	if (pooled != NULL) {
		c->ep = pooled;
		OTSetSynchronous(c->ep);
		OTSetBlocking(c->ep);
		MS_LOG("mfs_open: reused pooled endpoint");
	} else {
		cfg = OTCreateConfiguration("tcp");
		if(!cfg) { MS_LOG("mfs_open: OTCreateConfig FAIL"); return 0; }
		c->ep = OTOpenEndpointInContext(cfg, 0, NULL, &e, macos9_ot_context);
		if(e!=noErr||!c->ep) {
			macsurf_debug_log_writef("mfs_open: OTOpenEndpoint err=%d",
					(int)e);
			return 0;
		}
		OTSetSynchronous(c->ep); OTSetBlocking(c->ep);
		if(OTBind(c->ep,NULL,NULL)!=noErr) {
			MS_LOG("mfs_open: OTBind FAIL");
			return 0;
		}
		sprintf(pstr,"%s:%d",PROXY_H,PROXY_P); OTMemzero(&call,sizeof(TCall));
		call.addr.buf=(UInt8*)&dns; call.addr.len = (short)OTInitDNSAddress(&dns,pstr);
		MS_LOG("mfs_open: OTConnect start");
		if(OTConnect(c->ep,&call,NULL)!=noErr) {
			MS_LOG("mfs_open: OTConnect FAIL");
			return 0;
		}
		MS_LOG("mfs_open: OTConnect OK");
	}

	u=nsurl_access(c->url);
	macsurf_debug_log_writef("mfs_open: GET %s", u ? u : "(null)");

	/* Extract Host: header value from the URL. Required by HTTP/1.1
	 * origin servers and increasingly by HTTP/1.0 ones too -- many
	 * proxies forward without injecting it, so the absence used to
	 * silently kill requests to virtual-hosted sites. */
	host_str = "";
	host_len = 0;
	host_lwc = nsurl_get_component(c->url, NSURL_HOST);
	if (host_lwc != NULL) {
		host_str = lwc_string_data(host_lwc);
		host_len = lwc_string_length(host_lwc);
	}

	/* fixes91 — HTTP/1.1 with explicit keep-alive. The Go proxy
	 * (proxy/proxy.go) is net/http-based, which honours keep-alive
	 * by default. Reusing the front-side TCP connection collapses
	 * ~44 OTConnect handshakes per page load to ~5–8. */
	sprintf(req,
		"GET %s HTTP/1.1\r\n"
		"Host: %.*s\r\n"
		"User-Agent: MacSurf/0.2\r\n"
		"Accept: */*\r\n"
		"Connection: keep-alive\r\n\r\n",
		u,
		(int)host_len, host_str);

	if (host_lwc != NULL) lwc_string_unref(host_lwc);

	MS_LOG("mfs_open: OTSnd start");
	r=OTSnd(c->ep,req,(long)strlen(req),0);
	if(r<0) {
		macsurf_debug_log_writef("mfs_open: OTSnd err=%ld",
				(long)r);
		return 0;
	}
	macsurf_debug_log_writef("mfs_open: OTSnd OK (%ld bytes)",
			(long)r);
	OTSetNonBlocking(c->ep); return 1;
#else
	return 0;
#endif
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
		msg.type=FETCH_HEADER; msg.data.header_or_data.buf=(const uint8_t*)p;
		msg.data.header_or_data.len=strlen(p); fetch_send_callback(&msg,c->parent);
	}
	/* fixes91 — keep-alive only if we know the body length and it isn't
	 * chunked. Without Content-Length we can't tell where one response
	 * ends and the next begins on a reused socket. */
	if (c->content_length < 0 || c->chunked) c->keep_alive_ok = 0;
	c->state=MFS_BODY;
	if(sep+4 < c->h_buf+c->h_len) {
		long initial_body = (long)((c->h_buf+c->h_len)-(sep+4));
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
	free(c->h_buf); c->h_buf=NULL;
}

static void mfs_poll_one(struct macos9_fetch_ctx *c) {
#ifdef __MACOS9__
	char b[RECV_B]; OTResult n; fetch_msg m;
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
