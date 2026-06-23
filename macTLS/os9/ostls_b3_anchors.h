/*
 * ostls_b3_anchors.h
 *
 * Stage B3 embedded X.509 trust anchors for MacTLSTest and MacSurf.
 * Full Mozilla CCADB root bundle (121 anchors, 82 RSA + 39 EC).
 *
 * Source: curl.se cacert.pem snapshot, converted from the Mozilla NSS
 * source via BearSSL's `brssl ta` tool, with the C99 designated
 * union initialisers rewritten as a runtime-init loop to satisfy CW8
 * C89. Regenerated via macTLS/tools/regenerate_anchors.sh.
 *
 * Coverage notes: includes all major CAs trusted by Mozilla Firefox
 * and downstream consumers (Cloudflare via Sectigo/USERTrust, Google
 * via GTS R1-R4, AWS via Amazon Root, Microsoft, Apple, IdenTrust /
 * Let's Encrypt, GlobalSign, GoDaddy, Buypass, QuoVadis, etc.).
 *
 * Rotation: BearSSL only consults the ROOT anchor by Distinguished
 * Name match against the chain the server presents. As long as the
 * server's chain terminates in one of these roots, validation
 * succeeds. When a root is decommissioned (typically a year or more
 * of advance notice in the CA/B Forum) rerun the regeneration script.
 */

#ifndef OSTLS_B3_ANCHORS_H
#define OSTLS_B3_ANCHORS_H

#include <stddef.h>     /* size_t */

/*
 * BearSSL's trust-anchor typedef is anonymous-struct-derived rather
 * than a forward-declarable tag, so we pull in bearssl_x509.h here.
 * Callers of this header already need bearssl.h anyway.
 */
#include "bearssl_x509.h"

#define OSTLS_B3_NUM_ANCHORS  121

/*
 * Return a pointer to the populated, ready-to-use trust-anchor
 * array and its count. The pointer is to a static-lifetime array
 * inside ostls_b3_anchors.c; the caller does NOT own the storage.
 * The first call lazily initialises the array; subsequent calls
 * return the same pointer with no work.
 */
void OSTLS_B3_GetAnchors(const br_x509_trust_anchor **out_anchors,
                         size_t *out_count);

#endif /* OSTLS_B3_ANCHORS_H */
