/*
 * Copyright 2019 Daniel Silverstone <dsilvers@netsurf-browser.org>
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
 *
 * SSL related types and values
 */

#ifndef NETSURF_SSL_CERTS_H_
#define NETSURF_SSL_CERTS_H_

struct nsurl;

/**
 * ssl certificate error status
 *
 * Do not reorder / remove entries because these may be persisted to the disk
 * cache as simple ints.
 */
typedef enum {
	SSL_CERT_ERR_OK,	/**< Nothing wrong with this certificate */
	SSL_CERT_ERR_UNKNOWN,	/**< Unknown error */
	SSL_CERT_ERR_BAD_ISSUER, /**< Bad issuer */
	SSL_CERT_ERR_BAD_SIG,	/**< Bad signature on this certificate */
	SSL_CERT_ERR_TOO_YOUNG,	/**< This certificate is not yet valid */
	SSL_CERT_ERR_TOO_OLD,	/**< This certificate is no longer valid */
	SSL_CERT_ERR_SELF_SIGNED, /**< This certificate (or the chain) is self signed */
	SSL_CERT_ERR_CHAIN_SELF_SIGNED, /**< This certificate chain is self signed */
	SSL_CERT_ERR_REVOKED,	/**< This certificate has been revoked */
	SSL_CERT_ERR_HOSTNAME_MISMATCH, /**< This certificate host did not match the server */
	SSL_CERT_ERR_CERT_MISSING, /**< This certificate was missing from the chain, its data is useless */
} ssl_cert_err;

/** Always the max known ssl certificate error type */
#define SSL_CERT_ERR_MAX_KNOWN SSL_CERT_ERR_HOSTNAME_MISMATCH

/** maximum number of X509 certificates in chain for TLS connection */
#define MAX_CERT_DEPTH 10

/**
 * X509 certificate chain
 */
struct cert_chain {
	/**
	 * the number of certificates in the chain
	 * */
	size_t depth;
	struct {
		/**
		 * Whatever is wrong with this certificate
		 */
		ssl_cert_err err;

		/**
		 * data in Distinguished Encoding Rules (DER) format
		 */
		uint8_t *der;

		/**
		 * DER length
		 */
		size_t der_length;
	} certs[MAX_CERT_DEPTH];
};

/**
 * create new certificate chain
 *
 * \param dpth the depth to set in the new chain.
 * \param chain_out A pointer to recive the new chain.
 * \return NSERROR_OK on success or NSERROR_NOMEM on memory exhaustion
 */
nserror cert_chain_alloc(size_t depth, struct cert_chain **chain_out);

/**
 * duplicate a certificate chain into an existing chain
 *
 * \param src The certificate chain to copy from
 * \param dst The chain to overwrite with a copy of src
 * \return NSERROR_OK on success or NSERROR_NOMEM on memory exhaustion
 *
 * NOTE: if this returns NSERROR_NOMEM then the destination chain will have
 * some amount of content and should be cleaned up with cert_chain_free.
 */
nserror cert_chain_dup_into(const struct cert_chain *src, struct cert_chain *dst);

/**
 * duplicate a certificate chain
 *
 * \param src The certificate chain to copy from
 * \param dst_out A pointer to recive the duplicated chain
 * \return NSERROR_OK on success or NSERROR_NOMEM on memory exhaustion
 */
nserror cert_chain_dup(const struct cert_chain *src, struct cert_chain **dst_out);

/**
 * create a certificate chain from a fetch query string
 *
 * \param url The url to convert the query from
 * \param dst_out A pointer to recive the duplicated chain
 * \return NSERROR_OK on success or NSERROR_NOMEM on memory exhaustion
 */
nserror cert_chain_from_query(struct nsurl *url, struct cert_chain **chain_out);

/**
 * create a fetch query string from a certificate chain
 *
 *
 * \return NSERROR_OK on success or NSERROR_NOMEM on memory exhaustion
 */
nserror cert_chain_to_query(struct cert_chain *chain, struct nsurl **url_out);

/**
 * free a certificate chain
 *
 * \param chain The certificate chain to free
 * \return NSERROR_OK on success
 */
nserror cert_chain_free(struct cert_chain *chain);

/**
 * total number of data bytes in a chain
 *
 * \param chain The chain to size
 * \return the number of bytes used by the chain
 */
size_t cert_chain_size(const struct cert_chain *chain);

#endif /* NETSURF_SSL_CERTS_H_ */
