/*
 * MacSurf — nsutils/base64.h  (declarations only)
 *
 * fixes591: CW8 does not emit `static`-in-header function bodies into the
 * including TU, so nsu_base64_encode/decode_alloc stayed undefined at link.
 * The bodies now live (external linkage) in netsurf/content/llcache.c, the
 * sole in-build consumer; this header only declares them.
 */

#ifndef NSUTILS_BASE64_H
#define NSUTILS_BASE64_H

#include <stddef.h>

typedef enum {
    NSUERROR_OK        = 0,
    NSUERROR_NOMEM     = 1,
    NSUERROR_BAD_INPUT = 2
} nsuerror;

extern nsuerror nsu_base64_encode(
        const unsigned char *input, size_t input_length,
        unsigned char *output, size_t *output_length);

extern nsuerror nsu_base64_decode_alloc(
        const unsigned char *input, size_t input_length,
        unsigned char **output, size_t *output_length);

extern nsuerror nsu_base64_encode_url(
        const unsigned char *input, size_t input_length,
        unsigned char *output, size_t *output_length);

extern nsuerror nsu_base64_decode_alloc_url(
        const unsigned char *input, size_t input_length,
        unsigned char **output, size_t *output_length);

#endif /* NSUTILS_BASE64_H */
