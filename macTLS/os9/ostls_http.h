/*
 * ostls_http.h -- macTLS v0.3 HTTP convenience layer
 *
 * Provides helpers for common HTTP tasks on top of the OSTLSConnection
 * async stream. Handles request formatting, response header parsing,
 * and chunked transfer-encoding decoding.
 *
 * This layer is optional; callers can still use the raw stream API
 * for custom protocols or manual HTTP handling.
 */

#ifndef OSTLS_HTTP_H
#define OSTLS_HTTP_H

#include "ostls_async.h"

/*
 * Chunked decoder state.
 */
typedef enum {
    kOSTLSChunkStateSize = 0,   /* reading hex chunk-size line     */
    kOSTLSChunkStateData,       /* copying chunk data              */
    kOSTLSChunkStateCRLF,       /* consuming \r\n after data       */
    kOSTLSChunkStateTrailer,    /* consuming trailers after last 0 */
    kOSTLSChunkStateDone        /* full body received              */
} OSTLSChunkState;

typedef struct OSTLSChunkDecoder {
    OSTLSChunkState state;
    UInt32          remaining;  /* bytes left in current chunk     */
    char            size_buf[16];
    int             size_len;
    int             trailer_just_after_eol;
} OSTLSChunkDecoder;

/*
 * Initialise a chunked decoder.
 */
void OSTLS_HTTP_ChunkDecoderInit(OSTLSChunkDecoder *decoder);

/*
 * Process a chunk of raw response bytes.
 *   - `in_buf`, `in_len`: raw bytes from the wire (decrypted).
 *   - `out_buf`, `out_cap`: where to put the decoded plaintext.
 *   - `out_read`: actual bytes decoded into `out_buf`.
 *   - `in_consumed`: bytes of `in_buf` consumed.
 *
 * Returns kOSTLSAsync_OK on success. If the decoder reaches
 * kOSTLSChunkStateDone, it stops consuming.
 */
OSErr OSTLS_HTTP_ChunkDecoderProcess(OSTLSChunkDecoder *decoder,
                                     const void *in_buf, UInt32 in_len,
                                     void *out_buf, UInt32 out_cap,
                                     UInt32 *out_read, UInt32 *in_consumed);

/*
 * Format a basic HTTP/1.1 GET request.
 *   - `buf`, `cap`: output buffer for the request string.
 *   - `host`: "example.com" (for the Host: header).
 *   - `path`: "/index.html"
 *
 * Returns the number of bytes written, or 0 if the buffer is too small.
 */
UInt32 OSTLS_HTTP_FormatGet(char *buf, UInt32 cap,
                            const char *host, const char *path);

/*
 * Format a basic HTTP/1.1 POST request.
 *   - `buf`, `cap`: output buffer for the request string.
 *   - `host`: "example.com"
 *   - `path`: "/api/v1/update"
 *   - `content_type`: "application/x-www-form-urlencoded"
 *   - `content_length`: size of the body to follow.
 *
 * Returns the number of bytes written, or 0 if the buffer is too small.
 * The caller is responsible for sending the body separately via
 * OSTLS_Write.
 */
UInt32 OSTLS_HTTP_FormatPost(char *buf, UInt32 cap,
                             const char *host, const char *path,
                             const char *content_type, UInt32 content_length);

#endif /* OSTLS_HTTP_H */
