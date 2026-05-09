/*
 * MacSurf — Mac OS 9 frontend for NetSurf
 * mac_iconv.c — iconv shim using Text Encoding Converter
 *
 * This file is part of MacSurf, built on the NetSurf engine.
 * Licensed under GPL v2.
 */

#include "mac_iconv.h"
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#ifdef __MACOS9__
#include <TextEncodingConverter.h>
#include <TextCommon.h>

/*
 * TEC status/error constants — may not be in all versions of
 * TextEncodingConverter.h.  Define fallbacks if missing.
 */
#ifndef kTECUsedFallbacksStatus
#define kTECUsedFallbacksStatus    1
#endif
#ifndef kTECPartialCharErr
#define kTECPartialCharErr         (-8783)
#endif
#ifndef kTECOutputBufferFullStatus
#define kTECOutputBufferFullStatus (-8785)
#endif
#ifndef kTECUnmappableElementErr
#define kTECUnmappableElementErr   (-8784)
#endif
#endif

#define ICONV_CACHE_MAX 16

struct mac_iconv_descriptor {
	char tocode[64];
	char fromcode[64];
#ifdef __MACOS9__
	TECObjectRef converter;
#endif
	int refcount;
};

/* Converter cache */
static struct mac_iconv_descriptor *cache[ICONV_CACHE_MAX];
static int cache_count;

#ifdef __MACOS9__

static struct mac_iconv_descriptor *cache_find(const char *tocode,
					       const char *fromcode)
{
	int i;
	for (i = 0; i < cache_count; i++) {
		if (strcmp(cache[i]->tocode, tocode) == 0 &&
		    strcmp(cache[i]->fromcode, fromcode) == 0) {
			cache[i]->refcount++;
			return cache[i];
		}
	}
	return NULL;
}

static void cache_insert(struct mac_iconv_descriptor *desc)
{
	if (cache_count < ICONV_CACHE_MAX) {
		cache[cache_count++] = desc;
	}
}

static void cache_remove(struct mac_iconv_descriptor *desc)
{
	int i;
	for (i = 0; i < cache_count; i++) {
		if (cache[i] == desc) {
			cache[i] = cache[--cache_count];
			return;
		}
	}
}

iconv_t iconv_open(const char *tocode, const char *fromcode)
{
	struct mac_iconv_descriptor *desc;
	TextEncoding to_enc, from_enc;
	TECObjectRef converter;
	OSStatus err;

	/* Check cache first */
	desc = cache_find(tocode, fromcode);
	if (desc != NULL)
		return (iconv_t)desc;

	/* Map IANA charset names to TextEncoding */
	err = TECGetTextEncodingFromInternetName(&to_enc,
		(ConstStr255Param)tocode);
	if (err != noErr) {
		errno = EINVAL;
		return (iconv_t)-1;
	}

	err = TECGetTextEncodingFromInternetName(&from_enc,
		(ConstStr255Param)fromcode);
	if (err != noErr) {
		errno = EINVAL;
		return (iconv_t)-1;
	}

	/* Create TEC converter */
	err = TECCreateConverter(&converter, from_enc, to_enc);
	if (err != noErr) {
		errno = EINVAL;
		return (iconv_t)-1;
	}

	desc = malloc(sizeof(*desc));
	if (desc == NULL) {
		TECDisposeConverter(converter);
		errno = ENOMEM;
		return (iconv_t)-1;
	}

	strncpy(desc->tocode, tocode, sizeof(desc->tocode) - 1);
	desc->tocode[sizeof(desc->tocode) - 1] = '\0';
	strncpy(desc->fromcode, fromcode, sizeof(desc->fromcode) - 1);
	desc->fromcode[sizeof(desc->fromcode) - 1] = '\0';
	desc->converter = converter;
	desc->refcount = 1;

	cache_insert(desc);

	return (iconv_t)desc;
}

size_t iconv(iconv_t cd, char **inbuf, size_t *inbytesleft,
	     char **outbuf, size_t *outbytesleft)
{
	struct mac_iconv_descriptor *desc = (struct mac_iconv_descriptor *)cd;
	ByteCount input_read, output_written;
	OSStatus err;

	/* Reset conversion state when inbuf is NULL */
	if (inbuf == NULL || *inbuf == NULL) {
		TECFlushText(desc->converter,
			     (TextPtr)(outbuf ? *outbuf : NULL),
			     outbuf ? *outbytesleft : 0,
			     &output_written);
		if (outbuf && outbytesleft) {
			*outbuf += output_written;
			*outbytesleft -= output_written;
		}
		return 0;
	}

	err = TECConvertText(desc->converter,
			     (ConstTextPtr)*inbuf,
			     *inbytesleft,
			     &input_read,
			     (TextPtr)*outbuf,
			     *outbytesleft,
			     &output_written);

	/* Update pointers regardless of error */
	*inbuf += input_read;
	*inbytesleft -= input_read;
	*outbuf += output_written;
	*outbytesleft -= output_written;

	if (err == noErr || err == kTECUsedFallbacksStatus)
		return (size_t)output_written;

	if (err == kTECPartialCharErr) {
		errno = EINVAL;
		return (size_t)-1;
	}
	if (err == kTECOutputBufferFullStatus) {
		errno = E2BIG;
		return (size_t)-1;
	}
	if (err == kTECUnmappableElementErr) {
		errno = EILSEQ;
		return (size_t)-1;
	}

	/* Unknown TEC error */
	errno = EINVAL;
	return (size_t)-1;
}

int iconv_close(iconv_t cd)
{
	struct mac_iconv_descriptor *desc = (struct mac_iconv_descriptor *)cd;

	if (desc == NULL || cd == (iconv_t)-1)
		return -1;

	desc->refcount--;
	if (desc->refcount <= 0) {
		TECDisposeConverter(desc->converter);
		cache_remove(desc);
		free(desc);
	}

	return 0;
}

#else /* Linux stubs */

iconv_t iconv_open(const char *tocode, const char *fromcode)
{
	(void)tocode;
	(void)fromcode;
	return (iconv_t)-1;
}

size_t iconv(iconv_t cd, char **inbuf, size_t *inbytesleft,
	     char **outbuf, size_t *outbytesleft)
{
	(void)cd;
	(void)inbuf;
	(void)inbytesleft;
	(void)outbuf;
	(void)outbytesleft;
	return (size_t)-1;
}

int iconv_close(iconv_t cd)
{
	(void)cd;
	return 0;
}

#endif /* __MACOS9__ */
