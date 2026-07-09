/*
 * ostls_async.h -- macTLS v0.2 non-blocking TLS stream API
 *
 * Socket-like async TLS transport for classic Mac OS 9. Designed so a
 * host app with its own event loop / fetcher state machine can drive
 * a TLS connection without freezing the UI:
 *
 *   conn = OSTLS_New(&cfg);
 *   OSTLS_Start(conn);
 *   while (!done) {
 *       OSTLSEvent ev;
 *       OSTLS_Pump(conn, 4, &ev);
 *       if (state == kOSTLSStateOpen && !request_sent) {
 *           OSTLS_Write(conn, req_buf, req_len, &written);
 *       }
 *       if (ev == kOSTLSEventReadable) {
 *           OSTLS_Read(conn, buf, sizeof buf, &n);
 *           // hand n bytes to the consumer
 *       }
 *       if (ev == kOSTLSEventClosed) done = true;
 *       if (ev == kOSTLSEventFailed) return error;
 *       WaitNextEvent(...);
 *   }
 *   OSTLS_Close(conn);
 *   OSTLS_Dispose(conn);
 *
 * HTTP semantics (request formatting, redirect handling, chunked
 * decoding) stay above macTLS. macTLS only owns the TLS bytes
 * between OT and the caller.
 *
 * Design doc: docs/mactls-v0.2-async-design.md
 * Library API doc: docs/mactls-library-api.md
 */

#ifndef OSTLS_ASYNC_H
#define OSTLS_ASYNC_H

#ifdef __MWERKS__
#include <MacTypes.h>      /* OSErr, OSStatus, UInt16, UInt32 */
#else
#ifndef OSTLS_OSERR_DEFINED
#define OSTLS_OSERR_DEFINED
typedef short OSErr;
typedef long  OSStatus;
#endif
#ifndef OSTLS_UINT16_DEFINED
#define OSTLS_UINT16_DEFINED
typedef unsigned short UInt16;
#endif
#ifndef OSTLS_UINT32_DEFINED
#define OSTLS_UINT32_DEFINED
typedef unsigned long  UInt32;
#endif
#endif

/* Opaque connection handle. */
typedef struct OSTLSConnection OSTLSConnection;

/*
 * Connection state machine. See design doc §"State machine" for
 * transitions. DNS resolution is folded into kOSTLSStateConnecting
 * because OT's OTInitDNSAddress + OTConnect treats them as one step.
 */
typedef enum {
    kOSTLSStateIdle = 0,       /* New() called, Start() not yet           */
    kOSTLSStateConnecting,     /* OTOpen + OTBind + OTConnect in flight   */
    kOSTLSStateHandshaking,    /* TCP up; BearSSL handshake in progress   */
    kOSTLSStateOpen,           /* app-data flowing both ways              */
    kOSTLSStateClosing,        /* close_notify sent, draining             */
    kOSTLSStateClosed,         /* peer closed cleanly; readable until 0   */
    kOSTLSStateFailed          /* terminal failure; see OSTLS_GetLastError*/
} OSTLSState;

/*
 * Events surfaced by OSTLS_Pump. Pump returns AT MOST ONE event per
 * call, the most-interesting one observed during the pump slice.
 * Precedence: Failed > Closed > HandshakeDone > Connected > Readable
 * > Writable > None.
 */
typedef enum {
    kOSTLSEventNone = 0,       /* nothing caller-actionable this slice    */
    kOSTLSEventConnected,      /* TCP three-way handshake completed       */
    kOSTLSEventHandshakeDone,  /* TLS handshake completed; cipher set     */
    kOSTLSEventReadable,       /* OSTLS_Read may return decrypted bytes   */
    kOSTLSEventWritable,       /* OSTLS_Write will accept more bytes      */
    kOSTLSEventClosed,         /* peer FIN received; drain then close     */
    kOSTLSEventFailed          /* state advanced to kOSTLSStateFailed     */
} OSTLSEvent;

/*
 * Per-connection configuration. host/server_name strings are copied
 * into the connection at OSTLS_New time; the caller does not need to
 * keep them live afterwards.
 */
typedef struct OSTLSConfig {
    const char *host;                   /* "google.com" or "10.0.0.1"     */
    UInt16      port;                   /* 443                            */
    const char *server_name;            /* SNI + cert hostname match      */

    /* 0 = use defaults (30 s at 60 Hz = 1800 ticks). */
    UInt32 connect_timeout_ticks;
    UInt32 handshake_timeout_ticks;

    /* Opaque to macTLS; retrieved via OSTLS_GetUserRefcon. */
    void *user_refcon;
} OSTLSConfig;

/*
 * Snapshot of the connection's error and identity state. Cheap to
 * call from any thread / event-loop tick.
 */
typedef struct OSTLSDiagnostics {
    OSErr      os_err;          /* OSTLS_* style result code           */
    OSStatus   ot_err;          /* underlying OT error if any          */
    int        br_err;          /* BR_ERR_* if TLS-side failure        */
    OSTLSState state;
    UInt16     cipher_suite;    /* 0 until handshake completes         */

    /* Counters for diagnosing stalls. Reset implicitly at OSTLS_New. */
    UInt32 ot_send_calls;
    UInt32 ot_send_bytes;
    UInt32 ot_send_zero;        /* OTSnd returned 0 (try again)        */
    UInt32 ot_send_flow;        /* OTSnd returned kOTFlowErr           */
    UInt32 ot_recv_calls;
    UInt32 ot_recv_bytes;
    UInt32 ot_recv_nodata;      /* OTRcv returned kOTNoDataErr         */
    UInt32 pump_calls;
    UInt32 br_state_last;       /* last br_ssl_engine_current_state    */
    int    hs_fail_site;        /* fixes701 (#206): TLS13_FAIL_* step   */
    int    hs_state;            /* fixes701 (#206): tls13 hs state      */
} OSTLSDiagnostics;

/*
 * v0.2 async-API result codes. Disjoint from the v0.1 OSTLS_Fetch
 * codes (1000..1014) so a host app can switch over both spaces
 * without collision. Range: 2000..2019.
 */
enum {
    kOSTLSAsync_OK              = 0,
    kOSTLSAsync_BadArgs         = 2000,
    kOSTLSAsync_NoMemory        = 2001,
    kOSTLSAsync_ClockBefore2000 = 2002,
    kOSTLSAsync_OTConfigFail    = 2003,
    kOSTLSAsync_OTOpenFail      = 2004,
    kOSTLSAsync_OTBindFail      = 2005,
    kOSTLSAsync_OTConnectFail   = 2006,
    kOSTLSAsync_OTNotifierFail  = 2007,
    kOSTLSAsync_OTSndFail       = 2008,
    kOSTLSAsync_OTRcvFail       = 2009,
    kOSTLSAsync_EntropyFail     = 2010,
    kOSTLSAsync_ClientResetFail = 2011,
    kOSTLSAsync_BearSSLError    = 2012,
    kOSTLSAsync_ConnectTimeout  = 2013,
    kOSTLSAsync_HandshakeTimeout= 2014,
    kOSTLSAsync_PeerClosed      = 2015,  /* peer closed before handshake */
    kOSTLSAsync_WrongState      = 2016,  /* called Read/Write in wrong state */
    kOSTLSAsync_Disposed        = 2017   /* operation on a disposed conn */
};

/* ----------------------------------------------------------------- */
/* Lifecycle                                                         */
/* ----------------------------------------------------------------- */

/*
 * Allocate a connection. Does NOT open any network resources or
 * issue any OT calls -- those happen at OSTLS_Start. Returns
 * kOSTLSAsync_OK and writes the handle to *out_conn, or a non-zero
 * error code (in which case *out_conn is set to NULL).
 *
 * Caller still owns the pointer and must eventually OSTLS_Dispose.
 */
OSErr OSTLS_New(OSTLSConnection **out_conn, const OSTLSConfig *config);

/*
 * Begin the asynchronous connect. Transitions state Idle -> Connecting
 * and kicks off OTAsyncOpenEndpointInContext + OTBind + OTConnect.
 * Returns immediately; observe progress via OSTLS_Pump.
 *
 * Returns kOSTLSAsync_OK if the open call dispatched. A non-zero
 * return code means the connection couldn't even start (state moves
 * to Failed; consult diagnostics).
 */
OSErr OSTLS_Start(OSTLSConnection *conn);

/*
 * Cleanly close the connection: send TLS close_notify if open,
 * orderly-disconnect the OT endpoint, transition state to Closing
 * then Closed.
 *
 * Calling Close on a connection that's already Closed/Failed is a
 * no-op. Calling Close mid-handshake aborts the handshake with
 * OTSndDisconnect and transitions to Failed (handshake never
 * completed, so "Closed" would be misleading).
 */
void OSTLS_Close(OSTLSConnection *conn);

/*
 * Free all resources. Tears down any live OT endpoint first if
 * Close wasn't called. After OSTLS_Dispose returns, the handle is
 * invalid; further calls on it are UB. Safe to call on a NULL
 * handle.
 */
void OSTLS_Dispose(OSTLSConnection *conn);

/* ----------------------------------------------------------------- */
/* Pump                                                              */
/* ----------------------------------------------------------------- */

/*
 * Advance the connection at most `max_steps` atomic actions, then
 * return. A step is one of:
 *   - drain one notifier-set OT event flag and react
 *   - call OTSnd once with whatever BearSSL has queued
 *   - call OTRcv once and deliver bytes into BearSSL
 *   - advance BearSSL's engine state machine once
 *   - copy one chunk of plaintext into the read buffer
 *   - copy one chunk of caller-queued data into BearSSL's send-app
 *
 * NEVER blocks on the network. NEVER calls WaitNextEvent. NEVER
 * sleeps. The caller controls cadence by how often Pump is invoked.
 *
 * *out_event receives the most-interesting event seen during the
 * slice. If max_steps was used up mid-progress and no caller-
 * actionable event occurred, *out_event is kOSTLSEventNone. The
 * caller can OSTLS_GetState to confirm the connection is still
 * advancing.
 *
 * max_steps = 0 is legal and degenerate: returns immediately
 * without advancing (useful for "did we fail without polling?").
 */
OSErr OSTLS_Pump(OSTLSConnection *conn, UInt32 max_steps, OSTLSEvent *out_event);

/* ----------------------------------------------------------------- */
/* Streaming I/O                                                     */
/* ----------------------------------------------------------------- */

/*
 * Queue plaintext for transmission. Returns the number of bytes
 * actually accepted via *out_written (always >= 0, may be < len if
 * the internal write buffer is full). Caller should retry the
 * remainder on a later Pump tick once kOSTLSEventWritable fires.
 *
 * Only legal in state Open. Returns kOSTLSAsync_WrongState
 * otherwise.
 */
OSErr OSTLS_Write(OSTLSConnection *conn, const void *buf, UInt32 len, UInt32 *out_written);

/*
 * Read decrypted bytes into the caller buffer. Returns the number
 * of bytes copied via *out_read (0 if no data currently available;
 * this is NOT an error). Caller may call Read multiple times per
 * Pump tick to drain the internal plaintext buffer; when
 * out_read == 0 and err == noErr, go back to Pump.
 *
 * Legal in states Open, Closing, and Closed (the last two can still
 * have buffered plaintext to drain).
 */
OSErr OSTLS_Read(OSTLSConnection *conn, void *buf, UInt32 cap, UInt32 *out_read);

/* ----------------------------------------------------------------- */
/* Introspection                                                     */
/* ----------------------------------------------------------------- */

OSTLSState  OSTLS_GetState(OSTLSConnection *conn);
OSErr       OSTLS_GetLastError(OSTLSConnection *conn);
UInt16      OSTLS_GetCipherSuite(OSTLSConnection *conn);
int         OSTLS_GetResumed(OSTLSConnection *conn);
void        OSTLS_GetDiagnostics(OSTLSConnection *conn, OSTLSDiagnostics *out_diag);
void *      OSTLS_GetUserRefcon(OSTLSConnection *conn);
/* fixes686 (Path B) — returns 1 if the last OTSnd hit kOTFlowErr and the
 * pump is waiting for T_GODATA before it can send more TLS data.  The
 * fetcher can call this after OSTLS_Write to decide whether to re-invoke
 * OSTLS_Pump immediately or defer to the next poll tick. */
int         OSTLS_WantWrite(OSTLSConnection *conn);

/*
 * Process-wide switch for the TLS 1.3 path. Enabled by default: every
 * connection offers 1.3 and falls back to 1.2 automatically if the
 * server declines. Pass 0 to force 1.2-only (e.g. to isolate a problem
 * to one protocol). Affects connections started after the call.
 */
void        OSTLS_SetTryTLS13(int enabled);

/*
 * fixes413 -- SHA-384 known-answer self-test. Returns 0 if SHA-384("abc")
 * matches the NIST vector (the macTLS SHA-384 core is correct on this build),
 * nonzero if it is wrong. Lets the host print a one-line on-device proof at
 * startup that the CW8 SHA-384 fix (fixes411) is live and computing correctly.
 */
int         OSTLS_SHA384_KAT(void);

#endif /* OSTLS_ASYNC_H */
