/*
 * ostls_tls13_selftest.h -- on-device TLS 1.3 crypto self-test.
 *
 * Runs the key schedule and record layer against known RFC 8446/8448
 * vectors, plus a record round-trip and tamper-rejection check, logging
 * PASS/FAIL via the macTLS log channel. No network. Returns the number
 * of failures (0 = all pass).
 *
 * This is what proves the C89-ported TLS 1.3 crypto compiles AND produces
 * correct results on real PPC under CodeWarrior 8 -- the host (Linux)
 * tests already pass, this confirms the same on hardware (including the
 * 64-bit record sequence number under CW8 codegen).
 */

#ifndef OSTLS_TLS13_SELFTEST_H
#define OSTLS_TLS13_SELFTEST_H

int OSTLS_TLS13_SelfTest(void);

#endif /* OSTLS_TLS13_SELFTEST_H */
