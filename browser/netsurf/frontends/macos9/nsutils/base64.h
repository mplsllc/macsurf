/*
 * MacSurf stub — nsutils/base64.h
 * Minimal C89-compatible stub for CodeWarrior 8 compilation.
 *
 * Symbols stubbed:
 *   types: nsuerror
 *   enums: NSUERROR_OK, NSUERROR_NOMEM, NSUERROR_BAD_INPUT
 *   funcs: nsu_base64_encode, nsu_base64_decode_alloc,
 *          nsu_base64_encode_url, nsu_base64_decode_alloc_url
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
