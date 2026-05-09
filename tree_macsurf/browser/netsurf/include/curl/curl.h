/*
 * Stub curl/curl.h for Mac OS 9 syntax checking.
 * MacSurf does not use cURL — all fetching goes through Open Transport
 * to the MacSurf proxy via plain HTTP.
 *
 * This stub exists only because content/fetchers/curl.h unconditionally
 * includes <curl/curl.h>. The curl fetcher will not be compiled or
 * registered in the MacSurf build.
 */

#ifndef MACOS9_STUB_CURL_H
#define MACOS9_STUB_CURL_H

typedef void CURLM;
typedef void CURL;

#endif
