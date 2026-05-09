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
#define MAX_F 8
#define RECV_B 8192

enum mfs_state { MFS_IDLE, MFS_INIT, MFS_HEADERS, MFS_BODY, MFS_DONE, MFS_FAIL, MFS_NOTIFIED };
struct macos9_fetch_ctx {
	struct fetch *parent; struct nsurl *url; enum mfs_state state;
	int redirects; int aborted;
#ifdef __MACOS9__
	EndpointRef ep;
#endif
	char *h_buf; long h_len; long h_cap;
	int status; char mime[128]; const char *err;
};
static struct macos9_fetch_ctx f_slots[MAX_F];

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
	if (c->ep) { OTSndOrderlyDisconnect(c->ep); OTCloseProvider(c->ep); c->ep = NULL; }
#endif
}

static int mfs_open(struct macos9_fetch_ctx *c) {
#ifdef __MACOS9__
	OSStatus e; OTConfigurationRef cfg; DNSAddress dns; TCall call;
	char pstr[64], req[2048]; const char *u; OTResult r;
	cfg = OTCreateConfiguration("tcp"); if(!cfg) return 0;
	c->ep = OTOpenEndpointInContext(cfg, 0, NULL, &e, macos9_ot_context);
	if(e!=noErr||!c->ep) return 0;
	OTSetSynchronous(c->ep); OTSetBlocking(c->ep);
	if(OTBind(c->ep,NULL,NULL)!=noErr) return 0;
	sprintf(pstr,"%s:%d",PROXY_H,PROXY_P); OTMemzero(&call,sizeof(TCall));
	call.addr.buf=(UInt8*)&dns; call.addr.len = (short)OTInitDNSAddress(&dns,pstr);
	if(OTConnect(c->ep,&call,NULL)!=noErr) return 0;
	u=nsurl_access(c->url);
	sprintf(req,"GET %s HTTP/1.0\r\nUser-Agent: MacSurf/0.2\r\nAccept: */*\r\nConnection: close\r\n\r\n",u);
	r=OTSnd(c->ep,req,(long)strlen(req),0); if(r<0) return 0;
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
		if(strncasecmp(p,"Content-Type:",13)==0) { char *v=p+13; while(*v==' ')v++; strncpy(c->mime,v,127); c->mime[127]=0; }
		msg.type=FETCH_HEADER; msg.data.header_or_data.buf=(const uint8_t*)p;
		msg.data.header_or_data.len=strlen(p); fetch_send_callback(&msg,c->parent);
	}
	c->state=MFS_BODY;
	if(sep+4 < c->h_buf+c->h_len) {
		msg.type=FETCH_DATA; msg.data.header_or_data.buf=(const uint8_t*)(sep+4);
		msg.data.header_or_data.len=(size_t)((c->h_buf+c->h_len)-(sep+4));
		fetch_send_callback(&msg,c->parent);
	}
	free(c->h_buf); c->h_buf=NULL;
}

static void mfs_poll_one(struct macos9_fetch_ctx *c) {
#ifdef __MACOS9__
	char b[RECV_B]; OTResult n; fetch_msg m;
	if(c->state==MFS_IDLE || c->state==MFS_NOTIFIED) return;
	if(c->aborted) { c->state=MFS_DONE; return; }
	if(c->state==MFS_INIT) { if(mfs_open(c)) c->state=MFS_HEADERS; else c->state=MFS_FAIL; return; }
	n=OTRcv(c->ep,b,sizeof(b),NULL);
	if(n==kOTNoDataErr) return;
	if(n<0) {
		if(n==kOTLookErr) { OTResult l=OTLook(c->ep); if(l==T_ORDREL||l==T_DISCONNECT) { c->state=MFS_DONE; return; } }
		c->err="OT error"; c->state=MFS_FAIL; return;
	}
	if(n==0) { c->state=MFS_DONE; return; }
	if(c->state==MFS_HEADERS) {
		long nl=c->h_len+n;
		if(nl>c->h_cap) { long nc=c->h_cap==0?4096:c->h_cap*2; while(nc<nl)nc*=2; c->h_buf=realloc(c->h_buf,nc); if(c->h_buf) c->h_cap=nc; }
		if(!c->h_buf) { c->err="OOM"; c->state=MFS_FAIL; return; }
		memcpy(c->h_buf+c->h_len,b,(size_t)n); c->h_len=nl;
		if(strstr(c->h_buf,"\r\n\r\n")) mfs_parse_headers(c);
	} else {
		m.type=FETCH_DATA; m.data.header_or_data.buf=(const uint8_t*)b;
		m.data.header_or_data.len=(size_t)n; fetch_send_callback(&m,c->parent);
	}
#endif
}

static void macos9_http_poll(lwc_string *s) {
	int i; (void)s;
	for(i=0;i<MAX_F;i++) {
		struct macos9_fetch_ctx *c = &f_slots[i]; if(c->state==MFS_IDLE || c->state==MFS_NOTIFIED) continue;
		mfs_poll_one(c);
		if(c->state==MFS_FAIL) { fetch_msg m; m.type=FETCH_ERROR; m.data.error=c->err; c->state=MFS_NOTIFIED; fetch_send_callback(&m,c->parent); }
		else if(c->state==MFS_DONE) { fetch_msg m; m.type=FETCH_FINISHED; c->state=MFS_NOTIFIED; fetch_send_callback(&m,c->parent); }
	}
}

static bool macos9_http_initialise(lwc_string *s) { (void)s; return true; }
static void macos9_http_finalise(lwc_string *s) { (void)s; }
static bool macos9_http_acceptable(const struct nsurl *u) { (void)u; return true; }
static void *macos9_http_setup(struct fetch *p, struct nsurl *u, bool o, bool d, const char *pu, const struct fetch_multipart_data *pm, const char **h) {
	int i; (void)o;(void)d;(void)pu;(void)pm;(void)h;
	for(i=0;i<MAX_F;i++) if(f_slots[i].state==MFS_IDLE) { memset(&f_slots[i],0,sizeof(f_slots[0])); f_slots[i].parent=p; f_slots[i].url=nsurl_ref(u); f_slots[i].state=MFS_INIT; return &f_slots[i]; }
	return NULL;
}
static bool macos9_http_start(void *ctx) { return true; }
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
