/*
 * MacSurf wrapper — dom/bindings/hubbub/parser.h
 *
 * Inlined copy of browser/libdom/bindings/hubbub/parser.h.
 * The original wrapper used #include "../../../../bindings/hubbub/parser.h"
 * which CW8 can't resolve. This file IS the content now.
 *
 * Note: the "hubbub_errors.h" include below resolves via the
 * libdom:bindings:hubbub: access path to the real errors.h
 * at browser/libdom/bindings/hubbub/errors.h.
 */

#ifndef dom_hubbub_parser_h_
#define dom_hubbub_parser_h_

#include <stddef.h>
#include <inttypes.h>

#include <dom/dom.h>

/* Inlined dom_hubbub_error (normally from "hubbub_errors.h" in this dir)
 * and HUBBUB constants. Self-contained so CW8 access-path quirks
 * don't break the cascade. */
#ifndef dom_hubbub_errors_h_
#define dom_hubbub_errors_h_
#ifndef hubbub_errors_h_
#define hubbub_errors_h_
typedef enum hubbub_error {
	HUBBUB_OK             = 0,
	HUBBUB_REPROCESS      = 1,
	HUBBUB_ENCODINGCHANGE = 2,
	HUBBUB_PAUSED         = 3,
	HUBBUB_NOMEM          = 5,
	HUBBUB_BADPARM        = 6,
	HUBBUB_INVALID        = 7,
	HUBBUB_FILENOTFOUND   = 8,
	HUBBUB_NEEDDATA       = 9,
	HUBBUB_BADENCODING    = 10,
	HUBBUB_UNKNOWN        = 11
} hubbub_error;
#endif
typedef enum {
	DOM_HUBBUB_OK           = 0,
	DOM_HUBBUB_NOMEM        = 1,
	DOM_HUBBUB_BADPARM      = 2,
	DOM_HUBBUB_DOM          = 3,
	DOM_HUBBUB_HUBBUB_ERR   = (1<<16),
	DOM_HUBBUB_HUBBUB_ERR_PAUSED = (DOM_HUBBUB_HUBBUB_ERR | HUBBUB_PAUSED),
	DOM_HUBBUB_HUBBUB_ERR_ENCODINGCHANGE = (DOM_HUBBUB_HUBBUB_ERR | HUBBUB_ENCODINGCHANGE),
	DOM_HUBBUB_HUBBUB_ERR_NOMEM = (DOM_HUBBUB_HUBBUB_ERR | HUBBUB_NOMEM),
	DOM_HUBBUB_HUBBUB_ERR_BADPARM = (DOM_HUBBUB_HUBBUB_ERR | HUBBUB_BADPARM),
	DOM_HUBBUB_HUBBUB_ERR_INVALID = (DOM_HUBBUB_HUBBUB_ERR | HUBBUB_INVALID),
	DOM_HUBBUB_HUBBUB_ERR_FILENOTFOUND = (DOM_HUBBUB_HUBBUB_ERR | HUBBUB_FILENOTFOUND),
	DOM_HUBBUB_HUBBUB_ERR_NEEDDATA = (DOM_HUBBUB_HUBBUB_ERR | HUBBUB_NEEDDATA),
	DOM_HUBBUB_HUBBUB_ERR_BADENCODING = (DOM_HUBBUB_HUBBUB_ERR | HUBBUB_BADENCODING),
	DOM_HUBBUB_HUBBUB_ERR_UNKNOWN = (DOM_HUBBUB_HUBBUB_ERR | HUBBUB_UNKNOWN)
} dom_hubbub_error;
#endif

/**
 * Type of script completion function
 */
typedef dom_hubbub_error (*dom_script)(void *ctx, struct dom_node *node);

typedef struct dom_hubbub_parser dom_hubbub_parser;

/* The encoding source of the document */
typedef enum dom_hubbub_encoding_source {
	DOM_HUBBUB_ENCODING_SOURCE_HEADER,
	DOM_HUBBUB_ENCODING_SOURCE_DETECTED,
	DOM_HUBBUB_ENCODING_SOURCE_META
} dom_hubbub_encoding_source;

typedef struct dom_hubbub_parser_params {
	const char *enc;
	bool fix_enc;
	bool enable_script;
	dom_script script;
	dom_msg msg;
	void *ctx;
	dom_events_default_action_fetcher daf;
} dom_hubbub_parser_params;

dom_hubbub_error dom_hubbub_parser_create(dom_hubbub_parser_params *params,
		dom_hubbub_parser **parser,
		dom_document **document);

dom_hubbub_error dom_hubbub_fragment_parser_create(dom_hubbub_parser_params *params,
		dom_document *document,
		dom_hubbub_parser **parser,
		dom_document_fragment **fragment);

void dom_hubbub_parser_destroy(dom_hubbub_parser *parser);

dom_hubbub_error dom_hubbub_parser_parse_chunk(dom_hubbub_parser *parser,
		const uint8_t *data, size_t len);

dom_hubbub_error dom_hubbub_parser_insert_chunk(dom_hubbub_parser *parser,
		const uint8_t *data, size_t length);

dom_hubbub_error dom_hubbub_parser_completed(dom_hubbub_parser *parser);

const char *dom_hubbub_parser_get_encoding(dom_hubbub_parser *parser,
		dom_hubbub_encoding_source *source);

dom_hubbub_error dom_hubbub_parser_pause(dom_hubbub_parser *parser, bool pause);

#endif
