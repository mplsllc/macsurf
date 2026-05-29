/*
 * mlink_listener.c — Phase 0 loopback listener
 *
 * Validates that an FBA can:
 *   - InitOpenTransportInContext for application use
 *   - Open a TCP endpoint via OTOpenEndpointInContext
 *   - Bind to a loopback InetAddress and listen
 *   - Accept inbound connections cooperatively (via OT notifier +
 *     async OTLook from the event loop)
 *   - Tear down without leaking endpoints
 *
 * This is intentionally minimal. Real CONNECT proxying lands in
 * Phase 3 after macTLS server-side termination is wired (Phase 1)
 * and the CA can mint leaf certs (Phase 2). For now: accept,
 * log who connected, drop them.
 *
 * CW8 C89 — no inline, no //, no for-scope decls.
 */
#include <stddef.h>
#include <string.h>
#include <OpenTransport.h>
#include <OpenTptInternet.h>
#include <Threads.h>

#include "mlink_listener.h"
#include "mlink_log.h"

/* ------------------------------------------------------------------ */
/* Internal state                                                     */
/* ------------------------------------------------------------------ */

#define MLINK_MAX_LISTENERS  8
#define MLINK_MAX_CLIENTS   16

#define LSTATE_FREE     0
#define LSTATE_OPENING  1
#define LSTATE_BINDING  2
#define LSTATE_LISTENING 3
#define LSTATE_FAILED   4

struct mlink_listener_slot {
    int             state;
    int             port;
    EndpointRef     ep;
    int             pending_accept;
};

struct mlink_client_slot {
    int             active;
    int             port;            /* listener port that accepted */
    EndpointRef     ep;
    unsigned long   accepted_ticks;
};

static struct mlink_listener_slot g_lst[MLINK_MAX_LISTENERS];
static struct mlink_client_slot   g_cli[MLINK_MAX_CLIENTS];
static OTClientContextPtr         g_ot_ctx = NULL;
static int                        g_ot_initialised = 0;

/* ------------------------------------------------------------------ */
/* OT notifier — fires async with kOTSyncIdleEvent during blocking    */
/* OT calls. We yield to other cooperative threads here, matching     */
/* macTLS's pattern in ostls_async.c.                                 */
/* ------------------------------------------------------------------ */

static pascal void mlink__notifier(void *contextPtr, OTEventCode code,
                                   OTResult result, void *cookie)
{
    (void)contextPtr;
    (void)result;
    (void)cookie;
    if (code == kOTSyncIdleEvent) {
        YieldToAnyThread();
    } else if (code == T_LISTEN) {
        /* Mark the corresponding listener slot. Real dispatch happens
         * in mlink_listener_pump on the main thread; we just flag it. */
        int i;
        for (i = 0; i < MLINK_MAX_LISTENERS; i++) {
            /* contextPtr is the slot; cast from refcon */
            if ((void *)&g_lst[i] == contextPtr) {
                g_lst[i].pending_accept = 1;
                break;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* OT init — lazy, one-shot                                           */
/* ------------------------------------------------------------------ */

static int mlink__ot_init_once(void)
{
    OSStatus err;
    if (g_ot_initialised) return 0;
    err = InitOpenTransportInContext(kInitOTForApplicationMask, &g_ot_ctx);
    if (err != noErr) {
        mlink_logf("listener: InitOpenTransport FAIL err=%d", (int)err);
        return -1;
    }
    g_ot_initialised = 1;
    mlink_log("listener: Open Transport initialised");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Slot allocation                                                    */
/* ------------------------------------------------------------------ */

static struct mlink_listener_slot *mlink__listener_alloc(void)
{
    int i;
    for (i = 0; i < MLINK_MAX_LISTENERS; i++) {
        if (g_lst[i].state == LSTATE_FREE) {
            memset(&g_lst[i], 0, sizeof g_lst[i]);
            return &g_lst[i];
        }
    }
    return NULL;
}

static struct mlink_client_slot *mlink__client_alloc(void)
{
    int i;
    for (i = 0; i < MLINK_MAX_CLIENTS; i++) {
        if (!g_cli[i].active) {
            memset(&g_cli[i], 0, sizeof g_cli[i]);
            return &g_cli[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

int mlink_listener_start(int port)
{
    struct mlink_listener_slot *s;
    OSStatus err;
    InetAddress addr;
    TBind bind_req;
    OTConfigurationRef cfg;

    if (mlink__ot_init_once() != 0) return -1;

    s = mlink__listener_alloc();
    if (s == NULL) {
        mlink_log("listener: no free slot");
        return -1;
    }
    s->port = port;
    s->state = LSTATE_OPENING;

    cfg = OTCreateConfiguration("tcp");
    if (cfg == NULL || cfg == (OTConfigurationRef)-1L) {
        mlink_log("listener: OTCreateConfiguration FAIL");
        s->state = LSTATE_FAILED;
        return -1;
    }

    s->ep = OTOpenEndpointInContext(cfg, 0, NULL, &err, g_ot_ctx);
    if (err != noErr || s->ep == kOTInvalidEndpointRef) {
        mlink_logf("listener: OTOpenEndpoint FAIL err=%d", (int)err);
        s->state = LSTATE_FAILED;
        return -1;
    }

    err = OTInstallNotifier(s->ep, mlink__notifier, s);
    if (err != noErr) {
        mlink_logf("listener: OTInstallNotifier FAIL err=%d", (int)err);
        OTCloseProvider(s->ep);
        s->ep = kOTInvalidEndpointRef;
        s->state = LSTATE_FAILED;
        return -1;
    }

    (void)OTUseSyncIdleEvents(s->ep, true);

    /* Bind to loopback:port. qlen = 4 so we accept a small backlog. */
    s->state = LSTATE_BINDING;
    OTInitInetAddress(&addr, (UInt16)port, 0x7F000001UL);  /* 127.0.0.1 */
    bind_req.addr.maxlen = (UInt32)sizeof addr;
    bind_req.addr.len    = (UInt32)sizeof addr;
    bind_req.addr.buf    = (UInt8 *)&addr;
    bind_req.qlen        = 4;

    err = OTBind(s->ep, &bind_req, NULL);
    if (err != noErr) {
        mlink_logf("listener: OTBind FAIL port=%d err=%d", port, (int)err);
        OTCloseProvider(s->ep);
        s->ep = kOTInvalidEndpointRef;
        s->state = LSTATE_FAILED;
        return -1;
    }

    s->state = LSTATE_LISTENING;
    mlink_logf("listener: listening on 127.0.0.1:%d", port);
    return 0;
}

static void mlink__accept_pending(struct mlink_listener_slot *s)
{
    struct mlink_client_slot *c;
    EndpointRef new_ep;
    InetAddress peer;
    TCall call;
    OSStatus err;
    OTConfigurationRef cfg;

    if (s == NULL || s->ep == kOTInvalidEndpointRef) return;
    if (!s->pending_accept) return;
    s->pending_accept = 0;

    memset(&peer, 0, sizeof peer);
    memset(&call, 0, sizeof call);
    call.addr.maxlen = (UInt32)sizeof peer;
    call.addr.len    = 0;
    call.addr.buf    = (UInt8 *)&peer;

    err = OTListen(s->ep, &call);
    if (err == kOTNoDataErr) {
        /* False alarm — no actual call queued. */
        return;
    }
    if (err != noErr) {
        mlink_logf("listener: OTListen err=%d", (int)err);
        return;
    }

    c = mlink__client_alloc();
    if (c == NULL) {
        mlink_log("listener: client table full, rejecting");
        (void)OTSndDisconnect(s->ep, &call);
        return;
    }

    /* Open a fresh endpoint to receive the accepted call. */
    cfg = OTCreateConfiguration("tcp");
    new_ep = OTOpenEndpointInContext(cfg, 0, NULL, &err, g_ot_ctx);
    if (err != noErr || new_ep == kOTInvalidEndpointRef) {
        mlink_logf("listener: OTOpenEndpoint(accept) err=%d", (int)err);
        (void)OTSndDisconnect(s->ep, &call);
        c->active = 0;
        return;
    }

    err = OTAccept(s->ep, new_ep, &call);
    if (err != noErr) {
        mlink_logf("listener: OTAccept err=%d", (int)err);
        OTCloseProvider(new_ep);
        c->active = 0;
        return;
    }

    c->active         = 1;
    c->port           = s->port;
    c->ep             = new_ep;
    c->accepted_ticks = (unsigned long)TickCount();

    mlink_logf("listener: ACCEPT port=%d peer=%ld.%ld.%ld.%ld:%d",
        s->port,
        (long)((peer.fHost >> 24) & 0xFF),
        (long)((peer.fHost >> 16) & 0xFF),
        (long)((peer.fHost >>  8) & 0xFF),
        (long)( peer.fHost        & 0xFF),
        (int)peer.fPort);

    /* PHASE 0: close immediately. Real proxy dispatch in later phases. */
    (void)OTSndOrderlyDisconnect(c->ep);
    OTCloseProvider(c->ep);
    c->active = 0;
}

void mlink_listener_pump(void)
{
    int i;
    for (i = 0; i < MLINK_MAX_LISTENERS; i++) {
        if (g_lst[i].state == LSTATE_LISTENING) {
            mlink__accept_pending(&g_lst[i]);
        }
    }
}

void mlink_listener_stop_all(void)
{
    int i;
    for (i = 0; i < MLINK_MAX_LISTENERS; i++) {
        if (g_lst[i].state != LSTATE_FREE && g_lst[i].state != LSTATE_FAILED) {
            if (g_lst[i].ep != kOTInvalidEndpointRef) {
                OTCloseProvider(g_lst[i].ep);
            }
            g_lst[i].state = LSTATE_FREE;
            g_lst[i].ep = kOTInvalidEndpointRef;
        }
    }
    for (i = 0; i < MLINK_MAX_CLIENTS; i++) {
        if (g_cli[i].active && g_cli[i].ep != kOTInvalidEndpointRef) {
            OTCloseProvider(g_cli[i].ep);
            g_cli[i].active = 0;
            g_cli[i].ep = kOTInvalidEndpointRef;
        }
    }
}

int mlink_listener_active_count(void)
{
    int i, n;
    n = 0;
    for (i = 0; i < MLINK_MAX_CLIENTS; i++) {
        if (g_cli[i].active) n++;
    }
    return n;
}
