/*
 * ostls_tls13_otprobe.h -- drive a real TLS 1.3 handshake over Open
 * Transport on the Mac.
 *
 * The Mac-side counterpart of tests/host/test_tls13_handshake.c: same
 * transport-agnostic handshake driver, but the bytes move over OT
 * (OTSnd/OTRcv) instead of a Linux socket, and the chain is validated
 * against the embedded anchors. This is the on-device proof that the
 * TLS 1.3 handshake completes on real PowerPC, the step before wiring it
 * into the async fetch path (Stage D). See TLS13_SCOPE.md.
 *
 * Returns noErr (0) when the handshake reaches kTLS13_Complete; writes a
 * short status string to out_msg and the negotiated suite to *out_cipher.
 */

#ifndef OSTLS_TLS13_OTPROBE_H
#define OSTLS_TLS13_OTPROBE_H

#ifdef __MWERKS__
#include <Types.h>          /* OSErr */
#else
#include "ostls_time.h"     /* provides OSErr (guarded) on the Retro68 path */
#endif

OSErr OSTLS_TLS13_OTProbe(const char *target_host_port,
                          const char *server_name,
                          char *out_msg, unsigned long out_msg_len,
                          unsigned short *out_cipher);

#endif /* OSTLS_TLS13_OTPROBE_H */
