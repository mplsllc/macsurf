/*
 * MacSurf - macos9_gzip.h
 *
 * Streaming gzip (RFC 1952) / DEFLATE (RFC 1951) decoder for HTTP transport
 * decompression.
 *
 * WHY THIS EXISTS RATHER THAN lodepng_inflate()
 * ---------------------------------------------
 * lodepng is already in the build and exports lodepng_inflate(), but it is
 * one-shot: it takes a complete input buffer and allocates the entire output
 * in one call. Both macos9 fetchers deliver FETCH_DATA incrementally, from
 * inside a poll callback that must return promptly - OS 9 is cooperatively
 * scheduled, so a single non-yielding multi-hundred-millisecond inflate of a
 * whole document stalls WaitNextEvent and every other in-flight Open Transport
 * endpoint along with it. Routing gzip through lodepng would also mean holding
 * the compressed body, the decompressed body and lodepng's own doubling output
 * buffer simultaneously, and would put the decompression-bomb guard inside
 * lodepng's allocator instead of somewhere we control.
 *
 * This decoder is instead a resumable state machine: push whatever bytes have
 * arrived, get output through a callback in <= 32 KB pieces, keep 34 KB of
 * state per active fetch, and never block. See the fixes commit message for
 * the arithmetic that settled the buffered-vs-streaming question.
 *
 * C89 throughout (CW8): no //, no declarations after statements, no long long.
 * All arithmetic is 32-bit; nothing here needs a 64-bit multiply, so the CW8
 * PPC long-long codegen bug is not reachable.
 *
 * Usage:
 *      z = macos9_gunzip_create(emit_fn, ctx);
 *      ... per arriving chunk:  macos9_gunzip_push(z, buf, len)
 *      ... at end of body:      macos9_gunzip_finish(z)
 *      macos9_gunzip_destroy(z);
 *
 * push() returns DONE once the trailer has been read and verified; any bytes
 * after that (a second gzip member, or trailing garbage) are ignored.
 * Because output is streamed, a CRC/ISIZE mismatch is only detectable AFTER
 * the data has been handed on - the caller must treat an ERROR return as
 * "fail this fetch", not "discard the tail".
 */

#ifndef MACSURF_MACOS9_GZIP_H
#define MACSURF_MACOS9_GZIP_H

#define MACOS9_GUNZIP_OK     0    /* need more input */
#define MACOS9_GUNZIP_DONE   1    /* member complete, trailer verified */
#define MACOS9_GUNZIP_ERROR (-1)  /* corrupt stream or guard tripped */

/* Bomb guards. Both are enforced by the decoder itself, not by an allocator.
 *
 * MACOS9_GUNZIP_MAX_OUT is an absolute ceiling on decompressed bytes for one
 * response. Streaming means the decoder never holds that much, but its
 * CONSUMER does (llcache keeps the whole body), so this is really a limit on
 * what we are willing to hand to the rest of the browser on a 128 MB machine.
 * 16 MB is well above the largest body ever observed in a hardware log
 * (6.7 MB) and far below anything that would wedge the heap.
 *
 * MACOS9_GUNZIP_MAX_RATIO is checked only once output passes
 * MACOS9_GUNZIP_RATIO_FLOOR, because small files legitimately compress by
 * enormous factors (a 200-byte gzip of 60 KB of whitespace is ratio 300 and
 * completely normal). Above 1 MB of output, sustained 1000:1 is not a real
 * document. */
#define MACOS9_GUNZIP_MAX_OUT      (16UL * 1024UL * 1024UL)
#define MACOS9_GUNZIP_RATIO_FLOOR  (1024UL * 1024UL)
#define MACOS9_GUNZIP_MAX_RATIO    1000UL

/* Output sink. Called with decoded bytes; len is always > 0. The pointer is
 * only valid for the duration of the call (it points into the decoder's
 * sliding window). */
typedef void (*macos9_gunzip_emit_fn)(void *cbctx, const char *data, long len);

struct macos9_gunzip;

struct macos9_gunzip *macos9_gunzip_create(macos9_gunzip_emit_fn emit,
                                           void *cbctx);
void macos9_gunzip_destroy(struct macos9_gunzip *z);

/* Feed wire bytes. Emits zero or more times before returning. */
int macos9_gunzip_push(struct macos9_gunzip *z, const char *data, long len);

/* Call once the response body has ended. Flushes any held output and reports
 * ERROR if the stream never reached its trailer (truncated response). */
int macos9_gunzip_finish(struct macos9_gunzip *z);

/* Short human-readable reason for the last ERROR; never NULL. */
const char *macos9_gunzip_status(struct macos9_gunzip *z);

long macos9_gunzip_total_in(struct macos9_gunzip *z);
long macos9_gunzip_total_out(struct macos9_gunzip *z);

#endif /* MACSURF_MACOS9_GZIP_H */
