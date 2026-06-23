# Host-side unit tests for macTLS's portable code (key schedule, record
# layer, etc.). Built with the native compiler, NOT CodeWarrior -- these
# exercise the platform-independent crypto against known test vectors so
# we can verify it on Linux before any Mac is involved.
#
#   make test
#
# The modules under test are C89 (they also compile under CW8); this
# harness is plain host C linked against BearSSL's hash + HMAC + codec.

BEARSSL = ../../bearssl
CFLAGS  = -std=c99 -Wall -I$(BEARSSL)/inc -I$(BEARSSL)/src
BSSL    = $(wildcard $(BEARSSL)/src/hash/*.c $(BEARSSL)/src/mac/*.c \
                     $(BEARSSL)/src/codec/*.c $(BEARSSL)/src/symcipher/*.c \
                     $(BEARSSL)/src/aead/*.c $(BEARSSL)/src/int/*.c)
# Full BearSSL (adds ec/rsa/ssl/x509/kdf/rand) for the handshake test.
BSSL_ALL = $(shell find $(BEARSSL)/src -name '*.c')

OS9 = ../../os9

TESTS = test_tls13_keysched test_tls13_record test_tls13_ticket test_tls13_ticket_cache

# Offline crypto tests against known vectors.
test: $(TESTS)
	@for t in $(TESTS); do echo "--- $$t ---"; ./$$t || exit 1; done

# Live TLS 1.3 handshake against a real server. Needs network.
#   make handshake            (defaults to mactrove.com)
#   make handshake HOST=google.com
HOST ?= mactrove.com
handshake: test_tls13_handshake
	./test_tls13_handshake $(HOST) 443

# Live TLS 1.3 session resumption (two connections). Needs network.
#   make resume                 (defaults to cloudflare.com)
#   make resume RHOST=google.com
RHOST ?= cloudflare.com
resume: test_tls13_resume
	./test_tls13_resume $(RHOST) 443

test_tls13_resume: test_tls13_resume.c
	$(CC) -D_GNU_SOURCE $(CFLAGS) -Wno-pointer-sign \
	  test_tls13_resume.c \
	  $(OS9)/ostls_tls13_handshake.c $(OS9)/ostls_tls13_keysched.c \
	  $(OS9)/ostls_tls13_record.c $(OS9)/ostls_entropy.c \
	  $(OS9)/ostls_log.c $(OS9)/ostls_b3_anchors.c \
	  $(BSSL_ALL) -o $@

test_tls13_handshake: test_tls13_handshake.c
	$(CC) -D_GNU_SOURCE $(CFLAGS) -Wno-pointer-sign \
	  test_tls13_handshake.c \
	  $(OS9)/ostls_tls13_handshake.c $(OS9)/ostls_tls13_keysched.c \
	  $(OS9)/ostls_tls13_record.c $(OS9)/ostls_entropy.c \
	  $(OS9)/ostls_log.c $(OS9)/ostls_b3_anchors.c \
	  $(BSSL_ALL) -o $@

test_tls13_keysched: test_tls13_keysched.c ../../os9/ostls_tls13_keysched.c
	$(CC) $(CFLAGS) test_tls13_keysched.c ../../os9/ostls_tls13_keysched.c $(BSSL) -o $@

test_tls13_record: test_tls13_record.c ../../os9/ostls_tls13_record.c
	$(CC) $(CFLAGS) test_tls13_record.c ../../os9/ostls_tls13_record.c $(BSSL) -o $@

# Ticket cache is self-contained (only needs the hash vtables for the
# tls13_session_ticket type pulled in via the handshake header).
test_tls13_ticket_cache: test_tls13_ticket_cache.c ../../os9/ostls_ticket_cache.c
	$(CC) $(CFLAGS) test_tls13_ticket_cache.c ../../os9/ostls_ticket_cache.c $(BSSL) -o $@

# Ticket parse pulls in handshake.c, which references ec/x509/rsa, so it
# needs the full BearSSL like the live handshake test.
test_tls13_ticket: test_tls13_ticket.c
	$(CC) -D_GNU_SOURCE $(CFLAGS) -Wno-pointer-sign \
	  test_tls13_ticket.c \
	  $(OS9)/ostls_tls13_handshake.c $(OS9)/ostls_tls13_keysched.c \
	  $(OS9)/ostls_tls13_record.c $(OS9)/ostls_entropy.c \
	  $(OS9)/ostls_log.c $(OS9)/ostls_b3_anchors.c \
	  $(BSSL_ALL) -o $@

clean:
	rm -f $(TESTS) test_tls13_handshake test_tls13_resume

.PHONY: test handshake clean
