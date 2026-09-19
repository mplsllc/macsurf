/*
 * Parser API types used by the Hubbub implementation.
 *
 * CodeWarrior resolves the public <hubbub/parser.h> include by basename on
 * some access-path configurations, where it collides with libdom's parser.h.
 */

#ifndef macsurf_hub_parser_api_h_
#define macsurf_hub_parser_api_h_

#include <stdbool.h>
#include <inttypes.h>

#include <hubbub/errors.h>
#include <hubbub/functypes.h>
#include <hubbub/tree.h>
#include <hubbub/types.h>

typedef struct hubbub_parser hubbub_parser;

typedef enum hubbub_parser_opttype {
	HUBBUB_PARSER_TOKEN_HANDLER,
	HUBBUB_PARSER_ERROR_HANDLER,
	HUBBUB_PARSER_CONTENT_MODEL,
	HUBBUB_PARSER_TREE_HANDLER,
	HUBBUB_PARSER_DOCUMENT_NODE,
	HUBBUB_PARSER_ENABLE_SCRIPTING,
	HUBBUB_PARSER_PAUSE
} hubbub_parser_opttype;

typedef union hubbub_parser_optparams {
	struct {
		hubbub_token_handler handler;
		void *pw;
	} token_handler;

	struct {
		hubbub_error_handler handler;
		void *pw;
	} error_handler;

	struct {
		hubbub_content_model model;
	} content_model;

	hubbub_tree_handler *tree_handler;
	void *document_node;
	bool enable_scripting;
	bool pause_parse;
} hubbub_parser_optparams;

hubbub_error hubbub_parser_create(const char *enc, bool fix_enc,
		hubbub_parser **parser);
hubbub_error hubbub_parser_destroy(hubbub_parser *parser);
hubbub_error hubbub_parser_setopt(hubbub_parser *parser,
		hubbub_parser_opttype type,
		hubbub_parser_optparams *params);
hubbub_error hubbub_parser_parse_chunk(hubbub_parser *parser,
		const uint8_t *data, size_t len);
hubbub_error hubbub_parser_insert_chunk(hubbub_parser *parser,
		const uint8_t *data, size_t len);
hubbub_error hubbub_parser_completed(hubbub_parser *parser);
const char *hubbub_parser_read_charset(hubbub_parser *parser,
		hubbub_charset_source *source);

#endif
