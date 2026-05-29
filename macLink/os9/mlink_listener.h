/*
 * mlink_listener.h — Open Transport loopback listener for macLink
 *
 * Phase 0 deliverable. Opens a single TCP endpoint on 127.0.0.1:8765
 * (the default HTTPS-CONNECT port for macLink), listens for inbound
 * connections, and on accept logs the client and closes. No real
 * proxying yet — this validates that we can:
 *
 *  (a) bind / listen on a loopback address under Classic Mac OS,
 *  (b) accept incoming connections in a cooperative event loop,
 *  (c) free endpoints cleanly without leaking OT state.
 *
 * Phase 1+ extends this to dispatch accepted endpoints into the
 * actual CONNECT / HTTP / STARTTLS handlers.
 *
 * Defaults are macLink's chosen port assignments; final ports come
 * from prefs in later phases.
 */
#ifndef MLINK_LISTENER_H
#define MLINK_LISTENER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Default loopback port assignments (locked in scope doc). */
#define MLINK_PORT_HTTPS_CONNECT  8765
#define MLINK_PORT_SMTP_STARTTLS  8587
#define MLINK_PORT_IMAP_STARTTLS  8143
#define MLINK_PORT_POP3_STARTTLS  8110
#define MLINK_PORT_FTP_AUTH_TLS   8121

/* Bring up a single loopback TCP listener on the given port.
 * Returns 0 on success, negative on failure. Pump-driven; will not
 * block. Call once per port at startup. */
int mlink_listener_start(int port);

/* Drive any pending accept / handshake / close events. Idempotent.
 * Called once per WaitNextEvent iteration. */
void mlink_listener_pump(void);

/* Shut down all listeners. Idempotent. Closes any in-flight client
 * connections. */
void mlink_listener_stop_all(void);

/* Number of currently-active client connections across all listeners.
 * Diagnostic only. */
int mlink_listener_active_count(void);

#ifdef __cplusplus
}
#endif

#endif /* MLINK_LISTENER_H */
