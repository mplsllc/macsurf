/*
 * Stub nsutils/ns_base64.h for Mac OS 9 syntax checking.
 * Real libnsutils is not yet built for this target.
 */

#ifndef NSUTILS_BASE64_H
#define NSUTILS_BASE64_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
	NSUERROR_OK = 0,
	NSUERROR_NOMEM,
	NSUERROR_BAD_INPUT
} nsuerror;

static inline nsuerror nsu_base64_encode(const uint8_t *input, size_t input_length, uint8_t *output, size_t *output_length)
{
	(void)input; (void)input_length;
	(void)output;
	*output_length = 0;
	return NSUERROR_NOMEM;
}

static inline nsuerror nsu_base64_decode_alloc(const uint8_t *input, size_t input_length, uint8_t **output, size_t *output_length)
{
	(void)input; (void)input_length;
	*output = NULL;
	*output_length = 0;
	return NSUERROR_NOMEM;
}

#endif
