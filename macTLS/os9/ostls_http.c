/*
 * ostls_http.c -- macTLS v0.3 HTTP convenience layer. See os9/ostls_http.h.
 */

#include "ostls_http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void
OSTLS_HTTP_ChunkDecoderInit(OSTLSChunkDecoder *decoder)
{
    if (decoder == NULL) return;
    memset(decoder, 0, sizeof *decoder);
    decoder->state = kOSTLSChunkStateSize;
}

OSErr
OSTLS_HTTP_ChunkDecoderProcess(OSTLSChunkDecoder *decoder,
                               const void *in_buf, UInt32 in_len,
                               void *out_buf, UInt32 out_cap,
                               UInt32 *out_read, UInt32 *in_consumed)
{
    const unsigned char *b;
    unsigned char *o;
    UInt32 pos = 0;
    UInt32 written = 0;

    if (decoder == NULL || in_buf == NULL || out_buf == NULL ||
        out_read == NULL || in_consumed == NULL) {
        return (OSErr)kOSTLSAsync_BadArgs;
    }

    b = (const unsigned char *)in_buf;
    o = (unsigned char *)out_buf;
    *out_read = 0;
    *in_consumed = 0;

    while (pos < in_len && decoder->state != kOSTLSChunkStateDone) {
        switch (decoder->state) {
        case kOSTLSChunkStateSize: {
            while (pos < in_len) {
                unsigned char ch = b[pos++];
                if (ch == '\n') {
                    long sz;
                    decoder->size_buf[decoder->size_len] = '\0';
                    sz = strtol(decoder->size_buf, NULL, 16);
                    decoder->size_len = 0;
                    decoder->remaining = (UInt32)sz;
                    if (sz == 0) {
                        decoder->state = kOSTLSChunkStateTrailer;
                        decoder->trailer_just_after_eol = 1;
                    } else {
                        decoder->state = kOSTLSChunkStateData;
                    }
                    break;
                }
                if (ch == '\r') continue;
                /* Drop chunk extensions after ';' */
                if (ch == ';') {
                    /* Skip until \n */
                    while (pos < in_len && b[pos] != '\n') pos++;
                    continue;
                }
                if (decoder->size_len < (int)sizeof(decoder->size_buf) - 1) {
                    decoder->size_buf[decoder->size_len++] = (char)ch;
                }
            }
            break;
        }

        case kOSTLSChunkStateData: {
            UInt32 avail_in = in_len - pos;
            UInt32 avail_out = out_cap - written;
            UInt32 deliver = (avail_in < decoder->remaining) ? avail_in : decoder->remaining;
            if (deliver > avail_out) deliver = avail_out;

            if (deliver > 0) {
                memcpy(o + written, b + pos, deliver);
                written += deliver;
                pos += deliver;
                decoder->remaining -= deliver;
            }

            if (decoder->remaining == 0) {
                decoder->state = kOSTLSChunkStateCRLF;
            } else if (written >= out_cap) {
                /* Out of space in caller buffer; return partial progress. */
                goto done;
            }
            break;
        }

        case kOSTLSChunkStateCRLF: {
            while (pos < in_len) {
                unsigned char ch = b[pos++];
                if (ch == '\n') {
                    decoder->state = kOSTLSChunkStateSize;
                    break;
                }
            }
            break;
        }

        case kOSTLSChunkStateTrailer: {
            while (pos < in_len) {
                unsigned char ch = b[pos++];
                if (ch == '\r') continue;
                if (ch == '\n') {
                    if (decoder->trailer_just_after_eol) {
                        decoder->state = kOSTLSChunkStateDone;
                        break;
                    }
                    decoder->trailer_just_after_eol = 1;
                } else {
                    decoder->trailer_just_after_eol = 0;
                }
            }
            break;
        }

        case kOSTLSChunkStateDone:
            break;
        }
    }

done:
    *out_read = written;
    *in_consumed = pos;
    return (OSErr)kOSTLSAsync_OK;
}

UInt32
OSTLS_HTTP_FormatGet(char *buf, UInt32 cap,
                    const char *host, const char *path)
{
    int n;
    if (buf == NULL || host == NULL || path == NULL) return 0;

    n = sprintf(buf,
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: macTLS/0.3\r\n"
        "Accept: */*\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host);

    if (n < 0 || (UInt32)n >= cap) return 0;
    return (UInt32)n;
}

UInt32
OSTLS_HTTP_FormatPost(char *buf, UInt32 cap,
                     const char *host, const char *path,
                     const char *content_type, UInt32 content_length)
{
    int n;
    if (buf == NULL || host == NULL || path == NULL || content_type == NULL) return 0;

    n = sprintf(buf,
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: macTLS/0.3\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %lu\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host, content_type, (unsigned long)content_length);

    if (n < 0 || (UInt32)n >= cap) return 0;
    return (UInt32)n;
}
