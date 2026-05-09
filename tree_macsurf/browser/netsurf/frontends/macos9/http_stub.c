/*
 * MacSurf — http_stub.c
 * Stub implementations for NetSurf HTTP header parsing.
 * Licensed under GPL v2.
 */

#include <stddef.h>
#include "utils/errors.h"

struct http_cache_control;
struct http_content_type;
struct http_content_disposition;
struct http_www_authenticate;
struct http_strict_transport_security;
struct http_challenge;
struct http_parameter;

/* Content-Type */
nserror http_parse_content_type(const char *header,
		struct http_content_type **result)
{
	*result = NULL;
	return NSERROR_NOT_FOUND;
}

void http_content_type_destroy(struct http_content_type *ct) {}

/* Cache-Control */
nserror http_parse_cache_control(const char *header,
		struct http_cache_control **result)
{
	*result = NULL;
	return NSERROR_NOT_FOUND;
}

void http_cache_control_destroy(struct http_cache_control *cc) {}

/* Content-Disposition */
nserror http_parse_content_disposition(const char *header,
		struct http_content_disposition **result)
{
	*result = NULL;
	return NSERROR_NOT_FOUND;
}

void http_content_disposition_destroy(
		struct http_content_disposition *cd) {}

/* WWW-Authenticate */
nserror http_parse_www_authenticate(const char *header,
		struct http_www_authenticate **result)
{
	*result = NULL;
	return NSERROR_NOT_FOUND;
}

void http_www_authenticate_destroy(
		struct http_www_authenticate *wa) {}

/* Strict-Transport-Security */
nserror http_parse_strict_transport_security(const char *header,
		struct http_strict_transport_security **result)
{
	*result = NULL;
	return NSERROR_NOT_FOUND;
}

void http_strict_transport_security_destroy(
		struct http_strict_transport_security *sts) {}

/* Cache-Control accessors */
unsigned char http_cache_control_max_age(void *cc) { (void)cc; return 0; }
unsigned char http_cache_control_has_max_age(void *cc) { (void)cc; return 0; }
unsigned char http_cache_control_no_store(void *cc) { (void)cc; return 0; }
unsigned char http_cache_control_no_cache(void *cc) { (void)cc; return 0; }

/* Strict-Transport-Security accessors */
unsigned long http_strict_transport_security_max_age(void *sts) { (void)sts; return 0; }
unsigned char http_strict_transport_security_include_subdomains(void *sts) { (void)sts; return 0; }
