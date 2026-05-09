/*
 * Copyright 2020 Vincent Sanders <vince@netsurf-browser.org>
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
 * helpers for X509 certificate chains
 */

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <nsutils/base64.h>

#include "utils/errors.h"
#include "utils/log.h"
#include "utils/nsurl.h"

#include "netsurf/ssl_certs.h"

/*
 * create new certificate chain
 *
 * exported interface documented in netsurf/ssl_certs.h
 */
nserror
cert_chain_alloc(size_t depth, struct cert_chain **chain_out)
{
	struct cert_chain* chain;

	chain = calloc(1, sizeof(struct cert_chain));
	if (chain == NULL) {
		return NSERROR_NOMEM;
	}

	chain->depth = depth;

	*chain_out = chain;

	return NSERROR_OK;
}


/*
 * duplicate certificate chain into existing chain
 *
 * exported interface documented in netsurf/ssl_certs.h
 */
nserror
cert_chain_dup_into(const struct cert_chain *src, struct cert_chain *dst)
{
	size_t depth;
	for (depth = 0; depth < dst->depth; depth++) {
		if (dst->certs[depth].der != NULL) {
			free(dst->certs[depth].der);
			dst->certs[depth].der = NULL;
		}
	}

	dst->depth = src->depth;

	for (depth = 0; depth < src->depth; depth++) {
		dst->certs[depth].err = src->certs[depth].err;
		dst->certs[depth].der_length = src->certs[depth].der_length;
		if (src->certs[depth].der != NULL) {
			dst->certs[depth].der = malloc(src->certs[depth].der_length);
			if (dst->certs[depth].der == NULL) {
				return NSERROR_NOMEM;
			}
			memcpy(dst->certs[depth].der,
			       src->certs[depth].der,
			       src->certs[depth].der_length);
		}

	}

	return NSERROR_OK;
}


/*
 * duplicate certificate chain
 *
 * exported interface documented in netsurf/ssl_certs.h
 */
nserror
cert_chain_dup(const struct cert_chain *src, struct cert_chain **dst_out)
{
	struct cert_chain* dst;
	size_t depth;
	nserror res;

	res = cert_chain_alloc(src->depth, &dst);
	if (res != NSERROR_OK) {
		return res;
	}

	for (depth = 0; depth < src->depth; depth++) {
		dst->certs[depth].err = src->certs[depth].err;
		dst->certs[depth].der_length = src->certs[depth].der_length;
		if (src->certs[depth].der != NULL) {
			dst->certs[depth].der = malloc(src->certs[depth].der_length);
			if (dst->certs[depth].der == NULL) {
				cert_chain_free(dst);
				return NSERROR_NOMEM;
			}
			memcpy(dst->certs[depth].der,
			       src->certs[depth].der,
			       src->certs[depth].der_length);
		}

	}

	*dst_out = dst;
	return NSERROR_OK;
}


#define MIN_CERT_LEN 64

/**
 * process a part of a query extracting the certificate of an error code
 */
static nserror
process_query_section(const char *str, size_t len, struct cert_chain* chain)
{
	nsuerror nsures;

	if ((len > (5 + MIN_CERT_LEN)) &&
	    (strncmp(str, "cert=", 5) == 0)) {
		/* possible certificate entry */
		nsures = nsu_base64_decode_alloc_url(
			(const uint8_t *)str + 5,
			len - 5,
			&chain->certs[chain->depth].der,
			&chain->certs[chain->depth].der_length);
		if (nsures == NSUERROR_OK) {
			chain->depth++;
		}
	} else if ((len > 8) &&
		   (strncmp(str, "certerr=", 8) == 0)) {
		/* certificate entry error code */
		if (chain->depth > 0) {
			chain->certs[chain->depth - 1].err = strtoul(str + 8, NULL, 10);
		}
	}
	return NSERROR_OK;
}

/*
 * create a certificate chain from a fetch query string
 *
 * exported interface documented in netsurf/ssl_certs.h
 */
nserror cert_chain_from_query(struct nsurl *url, struct cert_chain **chain_out)
{
	struct cert_chain* chain;
	nserror res;
	char *querystr;
	size_t querylen;
	size_t kvstart;
	size_t kvlen;

	res = nsurl_get(url, NSURL_QUERY, &querystr, &querylen);
	if (res != NSERROR_OK) {
		return res;
	}

	if (querylen < MIN_CERT_LEN) {
		free(querystr);
		return NSERROR_NEED_DATA;
	}

	res = cert_chain_alloc(0, &chain);
	if (res != NSERROR_OK) {
		free(querystr);
		return res;
	}

	for (kvlen = 0, kvstart = 0; kvstart < querylen; kvstart += kvlen) {
		/* get query section length */
		kvlen = 0;
		while (((kvstart + kvlen) < querylen) &&
		       (querystr[kvstart + kvlen] != '&')) {
			kvlen++;
		}

		res = process_query_section(querystr + kvstart, kvlen, chain);
		if (res != NSERROR_OK) {
			break;
		}
		kvlen++; /* account for & separator */
	}
	free(querystr);

	if (chain->depth > 0) {
		*chain_out = chain;
	} else {
		free(chain);
		return NSERROR_INVALID;
	}

	return NSERROR_OK;
}


/*
 * create a fetch query string from a certificate chain
 *
 * exported interface documented in netsurf/ssl_certs.h
 */
nserror cert_chain_to_query(struct cert_chain *chain, struct nsurl **url_out )
{
	nserror res;
	nsurl *url;
	size_t allocsize;
	size_t urlstrlen;
	uint8_t *urlstr;
	size_t depth;

	allocsize = 20;
	for (depth = 0; depth < chain->depth; depth++) {
		allocsize += 7; /* allow for &cert= */
		allocsize += 4 * ((chain->certs[depth].der_length + 2) / 3);
		if (chain->certs[depth].err != SSL_CERT_ERR_OK) {
			allocsize += 20; /* allow for &certerr=4000000000 */
		}
	}

	urlstr = malloc(allocsize);
	if (urlstr == NULL) {
		return NSERROR_NOMEM;
	}

	urlstrlen = snprintf((char *)urlstr, allocsize, "about:certificate");
	for (depth = 0; depth < chain->depth; depth++) {
		int written;
		nsuerror nsures;
		size_t output_length;

		written = snprintf((char *)urlstr + urlstrlen,
					allocsize - urlstrlen,
					"&cert=");
		if (written < 0) {
			free(urlstr);
			return NSERROR_UNKNOWN;
		}
		if ((size_t)written >= allocsize - urlstrlen) {
			free(urlstr);
			return NSERROR_UNKNOWN;
		}

		urlstrlen += (size_t)written;

		output_length = allocsize - urlstrlen;
		nsures = nsu_base64_encode_url(
			chain->certs[depth].der,
			chain->certs[depth].der_length,
			(uint8_t *)urlstr + urlstrlen,
			&output_length);
		if (nsures != NSUERROR_OK) {
			free(urlstr);
			return (nserror)nsures;
		}
		urlstrlen += output_length;

		if (chain->certs[depth].err != SSL_CERT_ERR_OK) {
			written = snprintf((char *)urlstr + urlstrlen,
					allocsize - urlstrlen,
					"&certerr=%d",
					chain->certs[depth].err);
			if (written < 0) {
				free(urlstr);
				return NSERROR_UNKNOWN;
			}
			if ((size_t)written >= allocsize - urlstrlen) {
				free(urlstr);
				return NSERROR_UNKNOWN;
			}

			urlstrlen += (size_t)written;
		}

	}
	urlstr[17] = '?';
	urlstr[urlstrlen] = 0;

	res = nsurl_create((const char *)urlstr, &url);
	free(urlstr);

	if (res == NSERROR_OK) {
		*url_out = url;
	}

	return res;
}

/*
 * free certificate chain
 *
 * exported interface documented in netsurf/ssl_certs.h
 */
nserror cert_chain_free(struct cert_chain* chain)
{
	size_t depth;

	if (chain != NULL) {
		for (depth = 0; depth < chain->depth; depth++) {
			if (chain->certs[depth].der != NULL) {
				free(chain->certs[depth].der);
			}
		}

		free(chain);
	}

	return NSERROR_OK;
}


/*
 * calculate storage used of certificate chain
 *
 * exported interface documented in netsurf/ssl_certs.h
 */
size_t cert_chain_size(const struct cert_chain *chain)
{
	size_t size = 0;
	size_t depth;

	if (chain != NULL) {
		size += sizeof(struct cert_chain);

		for (depth = 0; depth < chain->depth; depth++) {
			if (chain->certs[depth].der != NULL) {
				size += chain->certs[depth].der_length;
			}
		}
	}

	return size;
}
