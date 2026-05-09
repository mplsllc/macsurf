/*
 * Copyright 2004 James Bursa <bursa@users.sourceforge.net>
 * Copyright 2006 Rob Kendrick <rjek@rjek.com>
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 *
 * NetSurf is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * NetSurf is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * \file
 * Localised message support implementation.
 *
 * Native language messages are loaded from a file and stored hashed by key for
 * fast access.
 */

#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <zlib.h>
#include <stdarg.h>

#include "utils/log.h"
#include "utils/messages.h"
#include "utils/utils.h"
#include "utils/hashtable.h"

/** Messages are stored in a fixed-size hash table. */
#define HASH_SIZE 101

/**
 * The hash table used to store the standard Messages file for the old API
 */
static struct hash_table *messages_hash = NULL;


/**
 * Create a message context
 *
 * generate a message context populated with english fallbacks for
 *   some formatted messages.
 */
static struct hash_table *messages_create_ctx(int hash_size)
{
	struct hash_table *nctx;
	const struct {
		const char *key;
		const char *value;
	} fallback[] = {
		{ "LoginDescription",
		  "The site %s is requesting your username and password. "
		  "The realm is \"%s\""},
		{ "PrivacyDescription",
		  "A privacy error occurred while communicating with %s this "
		  "may be a site configuration error or an attempt to steal "
		  "private information (passwords, messages or credit cards)"},
		{ "TimeoutDescription",
		  "A connection to %s could not be established. The site may "
		  "be temporarily unavailable or too busy to respond."},
		{ "FetchErrorDescription",
		  "An error occurred when connecting to %s"},
		{ NULL, NULL}
	};
	nctx = hash_create(hash_size);

	if (nctx != NULL) {
		int floop;
		for (floop = 0; fallback[floop].key != NULL; floop++) {
			hash_add(nctx,
				 fallback[floop].key,
				 fallback[floop].value);
		}
	}

	return nctx;
}

/**
 * Free memory used by a messages hash.
 * The context will not be valid after this function returns.
 *
 * \param  ctx  context of messages file to free
 */
static void messages_destroy_ctx(struct hash_table *ctx)
{
	if (ctx == NULL)
		return;

	hash_destroy(ctx);
}


/**
 * Read keys and values from messages file.
 *
 * \param  path  pathname of messages file
 * \param  ctx   reference of hash table to merge with or NULL to create one.
 * \return NSERROR_OK on sucess and ctx updated or error code on faliure.
 */
static nserror messages_load_ctx(const char *path, struct hash_table **ctx)
{
	struct hash_table *nctx; /* new context */
	nserror res;

	if (*ctx != NULL) {
		/**
		 * \note The passed hash is not copied here so this
		 * updates in place.
		 */
		return hash_add_file(*ctx, path);
	}

	nctx = messages_create_ctx(HASH_SIZE);
	if (nctx == NULL) {
		nslog_log(__FILE__, "", __LINE__,
			      "Unable to create hash table for messages file %s",
			      path);
		return NSERROR_NOMEM;
	}

	res = hash_add_file(nctx, path);
	if (res == NSERROR_OK) {
		*ctx = nctx;
	} else {
		hash_destroy(nctx);
	}

	return res;
}


/**
 * Fast lookup of a message by key.
 *
 * \param  key  key of message
 * \param  ctx  context of messages file to look up in
 * \return value of message, or key if not found
 */
static const char *
messages_get_ctx(const char *key, struct hash_table *ctx)
{
	const char *r = NULL;

	assert(key != NULL);

	/* allow attempts to retrieve messages before context is set up. */
	if (ctx != NULL) {
		r = hash_get(ctx, key);
	}

	/* If called with no context or unable to retrive a value
	 * return the key.
	 */
	if (r == NULL) {
		r = key;
	}

	return r;
}


/* exported interface documented in messages.h */
nserror messages_add_from_file(const char *path)
{
	if (path == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	nslog_log(__FILE__, "", __LINE__, "Loading Messages from '%s'", path);

	return messages_load_ctx(path, &messages_hash);
}


/* exported interface documented in messages.h */
nserror messages_add_from_inline(const uint8_t *data, size_t size)
{
	/* ensure the hash table is initialised */
	if (messages_hash == NULL) {
		messages_hash = messages_create_ctx(HASH_SIZE);
	}
	if (messages_hash == NULL) {
		NSLOG(netsurf, INFO, "Unable to create hash table");
		return NSERROR_NOMEM;
	}
	return hash_add_inline(messages_hash, data, size);
}


/* exported interface documented in messages.h */
nserror messages_add_key_value(const char *key, const char *value)
{
	/* ensure the hash table is initialised */
	if (messages_hash == NULL) {
		messages_hash = messages_create_ctx(HASH_SIZE);
	}
	if (messages_hash == NULL) {
		NSLOG(netsurf, INFO, "Unable to create hash table");
		return NSERROR_NOMEM;
	}
	return hash_add(messages_hash, key, value);
}


/* exported interface documented in messages.h */
char *messages_get_buff(const char *key, ...)
{
	const char *msg_fmt;
	char *buff = NULL; /* formatted buffer to return */
	int buff_len = 0;
	va_list ap;

	assert(key != NULL);

	if (messages_hash == NULL) {
		return NULL;
	}

	msg_fmt = hash_get(messages_hash, key);

	if (msg_fmt == NULL) {
		return NULL;
	}

	va_start(ap, key);
	buff_len = vsnprintf(buff, buff_len, msg_fmt, ap);
	va_end(ap);

	buff = malloc(buff_len + 1);

	if (buff != NULL) {
		va_start(ap, key);
		vsnprintf(buff, buff_len + 1, msg_fmt, ap);
		va_end(ap);
	}

	return buff;
}


/* exported function documented in utils/messages.h */
const char *messages_get(const char *key)
{
	return messages_get_ctx(key, messages_hash);
}


/* exported function documented in utils/messages.h */
const char *messages_get_errorcode(nserror code)
{
	switch (code) {
	case NSERROR_OK:
		/* No error */
		return messages_get_ctx("OK", messages_hash);

	case NSERROR_NOMEM:
		/* Memory exhaustion */
		return messages_get_ctx("NoMemory", messages_hash);

	case NSERROR_NO_FETCH_HANDLER:
		/* No fetch handler for URL scheme */
		return messages_get_ctx("NoHandler", messages_hash);

	case NSERROR_NOT_FOUND:
		/* Requested item not found */
		return messages_get_ctx("NotFound", messages_hash);

	case NSERROR_NOT_DIRECTORY:
		/* Missing directory */
		return messages_get_ctx("NotDirectory", messages_hash);

	case NSERROR_SAVE_FAILED:
		/* Failed to save data */
		return messages_get_ctx("SaveFailed", messages_hash);

	case NSERROR_CLONE_FAILED:
		/* Failed to clone handle */
		return messages_get_ctx("CloneFailed", messages_hash);

	case NSERROR_INIT_FAILED:
		/* Initialisation failed */
		return messages_get_ctx("InitFailed", messages_hash);

	case NSERROR_BMP_ERROR:
		/* A BMP error occurred */
		return messages_get_ctx("BMPError", messages_hash);

	case NSERROR_GIF_ERROR:
		/* A GIF error occurred */
		return messages_get_ctx("GIFError", messages_hash);

	case NSERROR_ICO_ERROR:
		/* A ICO error occurred */
		return messages_get_ctx("ICOError", messages_hash);

	case NSERROR_PNG_ERROR:
		/* A PNG error occurred */
		return messages_get_ctx("PNGError", messages_hash);

	case NSERROR_SPRITE_ERROR:
		/* A RISC OS Sprite error occurred */
		return messages_get_ctx("SpriteError", messages_hash);

	case NSERROR_SVG_ERROR:
		/* A SVG error occurred */
		return messages_get_ctx("SVGError", messages_hash);

	case NSERROR_BAD_ENCODING:
		/* The character set is unknown */
		return messages_get_ctx("BadEncoding", messages_hash);

	case NSERROR_NEED_DATA:
		/* More data needed */
		return messages_get_ctx("NeedData", messages_hash);

	case NSERROR_ENCODING_CHANGE:
		/* The character set encoding change was unhandled */
		return messages_get_ctx("EncodingChanged", messages_hash);

	case NSERROR_BAD_PARAMETER:
		/* Bad Parameter */
		return messages_get_ctx("BadParameter", messages_hash);

	case NSERROR_INVALID:
		/* Invalid data */
		return messages_get_ctx("Invalid", messages_hash);

	case NSERROR_BOX_CONVERT:
		/* Box conversion failed */
		return messages_get_ctx("BoxConvert", messages_hash);

	case NSERROR_STOPPED:
		/* Content conversion stopped */
		return messages_get_ctx("Stopped", messages_hash);

	case NSERROR_DOM:
		/* DOM call returned error */
		return messages_get_ctx("ParsingFail", messages_hash);

	case NSERROR_CSS:
		/* CSS call returned error */
		return messages_get_ctx("CSSGeneric", messages_hash);

	case NSERROR_CSS_BASE:
		/* CSS base sheet failed */
		return messages_get_ctx("CSSBase", messages_hash);

	case NSERROR_BAD_URL:
		/* Bad URL */
		return messages_get_ctx("BadURL", messages_hash);

	case NSERROR_BAD_CONTENT:
		/* Bad Content */
		return messages_get_ctx("BadContent", messages_hash);

	case NSERROR_FRAME_DEPTH:
		/* Exceeded frame depth */
		return messages_get_ctx("FrameDepth", messages_hash);

	case NSERROR_PERMISSION:
		/* Permission error */
		return messages_get_ctx("PermissionError", messages_hash);

	case NSERROR_BAD_SIZE:
		/* Bad size */
		return messages_get_ctx("BadSize", messages_hash);

	case NSERROR_NOSPACE:
		/* Insufficient space */
		return messages_get_ctx("NoSpace", messages_hash);

	case NSERROR_NOT_IMPLEMENTED:
		/* Functionality is not implemented */
		return messages_get_ctx("NotImplemented", messages_hash);

	case NSERROR_UNKNOWN:
		/* Unknown error */
		return messages_get_ctx("Unknown", messages_hash);

	case NSERROR_BAD_AUTH:
		/* Authentication required */
		return messages_get_ctx("BadAuth", messages_hash);

	case NSERROR_BAD_REDIRECT:
		/* unsupported redirects */
		return messages_get_ctx("UnsupportedRedirect", messages_hash);

	case NSERROR_CYCLIC_REDIRECT:
		/* Too many redirects */
		return messages_get_ctx("CyclicRedirect", messages_hash);

	case NSERROR_UNSAFE_REDIRECT:
		/* Unsafe redirects */
		return messages_get_ctx("UnsafeRedirect", messages_hash);

	case NSERROR_BAD_CERTS:
		/* Certificate chain verification failure */
		return messages_get_ctx("CertificateVerificationNeeded", messages_hash);

	case NSERROR_TIMEOUT:
		/* Operation timed out */
		return messages_get_ctx("Timeout", messages_hash);
	}

	/* The switch has no default, so the compiler should tell us when we
	 * forget to add messages for new error codes.  As such, we should
	 * never get here.
	 */
	assert(0);
	return messages_get_ctx("Unknown", messages_hash);
}

/* exported function documented in utils/messages.h */
const char *messages_get_sslcode(ssl_cert_err code)
{
	switch (code) {
	case SSL_CERT_ERR_OK:
		/* Nothing wrong with this certificate */
		return messages_get_ctx("SSLCertErrOk", messages_hash);

	case SSL_CERT_ERR_UNKNOWN:
		/* Unknown error */
		return messages_get_ctx("SSLCertErrUnknown", messages_hash);

	case SSL_CERT_ERR_BAD_ISSUER:
		/* Bad issuer */
		return messages_get_ctx("SSLCertErrBadIssuer", messages_hash);

	case SSL_CERT_ERR_BAD_SIG:
		/* Bad signature on this certificate */
		return messages_get_ctx("SSLCertErrBadSig", messages_hash);

	case SSL_CERT_ERR_TOO_YOUNG:
		/* This certificate is not yet valid */
		return messages_get_ctx("SSLCertErrTooYoung", messages_hash);

	case SSL_CERT_ERR_TOO_OLD:
		/* This certificate is no longer valid */
		return messages_get_ctx("SSLCertErrTooOld", messages_hash);

	case SSL_CERT_ERR_SELF_SIGNED:
		/* This certificate is self signed */
		return messages_get_ctx("SSLCertErrSelfSigned", messages_hash);

	case SSL_CERT_ERR_CHAIN_SELF_SIGNED:
		/* This certificate chain is self signed */
		return messages_get_ctx("SSLCertErrChainSelfSigned", messages_hash);

	case SSL_CERT_ERR_REVOKED:
		/* This certificate has been revoked */
		return messages_get_ctx("SSLCertErrRevoked", messages_hash);

	case SSL_CERT_ERR_HOSTNAME_MISMATCH:
		/* Common name is invalid */
		return messages_get_ctx("SSLCertErrHostnameMismatch", messages_hash);

	case SSL_CERT_ERR_CERT_MISSING:
		/* Common name is invalid */
		return messages_get_ctx("SSLCertErrCertMissing", messages_hash);

	}

	/* The switch has no default, so the compiler should tell us when we
	 * forget to add messages for new error codes.  As such, we should
	 * never get here.
	 */
	assert(0);
	return messages_get_ctx("Unknown", messages_hash);
}

/* exported function documented in utils/messages.h */
void messages_destroy(void)
{
	messages_destroy_ctx(messages_hash);
	messages_hash = NULL;
}
