/*
 * This file is part of libdom.
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 * Copyright 2007 John-Mark Bell <jmb@netsurf-browser.org>
 */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <libxml/parser.h>
#include <libxml/SAX2.h>
#include <libxml/xmlerror.h>

#include <dom/dom.h>

#include <libwapcaplet/libwapcaplet.h>

#include "xmlerror.h"
#include "xmlparser.h"
#include "utils.h"

#include "document_i.h"

#undef DEBUG_XML_PARSER

static void xml_parser_start_element_ns(void *ctx, const xmlChar *localname,
		const xmlChar *prefix, const xmlChar *URI,
		int nb_namespaces, const xmlChar **namespaces,
		int nb_attributes, int nb_defaulted,
		const xmlChar **attributes);
static void xml_parser_end_element_ns(void *ctx, const xmlChar *localname,
		const xmlChar *prefix, const xmlChar *URI);
static void xml_parser_reference(void *ctx, const xmlChar *name);
static void xml_parser_characters(void *ctx, const xmlChar *ch, int len);
static void xml_parser_comment(void *ctx, const xmlChar *value);
static void xml_parser_cdata_block(void *ctx, const xmlChar *value, int len);

static int xml_parser_is_standalone(void *ctx);
static int xml_parser_has_internal_subset(void *ctx);
static int xml_parser_has_external_subset(void *ctx);
static xmlParserInputPtr xml_parser_resolve_entity(void *ctx,
		const xmlChar *publicId, const xmlChar *systemId);
static xmlEntityPtr xml_parser_get_entity(void *ctx, const xmlChar *name);
static xmlEntityPtr xml_parser_get_parameter_entity(void *ctx,
		const xmlChar *name);

/* LibDOM's DTD handling is skeletal, so piggy-back on libxml2's
 * implementation until such time as we need not do so. When that
 * time arrives, all of the following callbacks can be dispensed with
 * (and the six above will want adjusting to taste). */
static void xml_parser_start_document(void *ctx);
static void xml_parser_end_document(void *ctx);
static void xml_parser_internal_subset(void *ctx, const xmlChar *name,
		const xmlChar *ExternalID, const xmlChar *SystemID);
static void xml_parser_external_subset(void *ctx, const xmlChar *name,
		const xmlChar *ExternalID, const xmlChar *SystemID);
static void xml_parser_entity_decl(void *ctx, const xmlChar *name,
		int type, const xmlChar *publicId, const xmlChar *systemId,
		xmlChar *content);
static void xml_parser_notation_decl(void *ctx, const xmlChar *name,
		const xmlChar *publicId, const xmlChar *systemId);
static void xml_parser_attribute_decl(void *ctx, const xmlChar *elem,
		const xmlChar *fullname, int type, int def,
		const xmlChar *defaultValue, xmlEnumerationPtr tree);
static void xml_parser_element_decl(void *ctx, const xmlChar *name,
		int type, xmlElementContentPtr content);
static void xml_parser_unparsed_entity_decl(void *ctx, const xmlChar *name,
		const xmlChar *publicId, const xmlChar *systemId,
		const xmlChar *notationName);

/**
 * libdom XML parser object
 */
struct dom_xml_parser {
	xmlParserCtxtPtr xml_ctx;	/**< libxml parser context */

	struct dom_document *doc;	/**< DOM Document we're building */
	struct dom_node *current;	/**< DOM node we're currently building */

	dom_msg msg;		/**< Informational message function */
	void *mctx;		/**< Pointer to client data */

	dom_exception err;	/**< Last DOM error, if any */
};

/**
 * SAX callback dispatch table
 */
static xmlSAXHandler sax_handler = {
	.internalSubset         = xml_parser_internal_subset,
	.isStandalone           = xml_parser_is_standalone,
	.hasInternalSubset      = xml_parser_has_internal_subset,
	.hasExternalSubset      = xml_parser_has_external_subset,
	.resolveEntity          = xml_parser_resolve_entity,
	.getEntity              = xml_parser_get_entity,
	.entityDecl             = xml_parser_entity_decl,
	.notationDecl           = xml_parser_notation_decl,
	.attributeDecl          = xml_parser_attribute_decl,
	.elementDecl            = xml_parser_element_decl,
	.unparsedEntityDecl     = xml_parser_unparsed_entity_decl,
	.setDocumentLocator     = NULL,
	.startDocument          = xml_parser_start_document,
	.endDocument            = xml_parser_end_document,
	.startElement           = NULL,
	.endElement             = NULL,
	.reference              = xml_parser_reference,
	.characters             = xml_parser_characters,
	.ignorableWhitespace    = xml_parser_characters,
	.processingInstruction  = NULL,
	.comment                = xml_parser_comment,
	.warning                = NULL,
	.error                  = NULL,
	.fatalError             = NULL,
	.getParameterEntity     = xml_parser_get_parameter_entity,
	.cdataBlock             = xml_parser_cdata_block,
	.externalSubset         = xml_parser_external_subset,
	.initialized            = XML_SAX2_MAGIC,
	._private               = NULL,
	.startElementNs         = xml_parser_start_element_ns,
	.endElementNs           = xml_parser_end_element_ns,
	.serror                 = NULL,
};

static void *dom_xml_alloc(void *ptr, size_t len, void *pw)
{
	UNUSED(pw);

	if (ptr == NULL)
		return len > 0 ? malloc(len) : NULL;

	if (len == 0) {
		free(ptr);
		return NULL;
	}

	return realloc(ptr, len);
}

/**
 * Create an XML parser instance
 *
 * \param enc      Source charset, or NULL
 * \param int_enc  Desired charset of document buffer (UTF-8 or UTF-16)
 * \param msg      Informational message function
 * \param mctx     Pointer to client-specific private data
 * \return Pointer to instance, or NULL on memory exhaustion
 *
 * Neither \p enc nor \p int_enc are used here.
 * libxml only supports a UTF-8 document buffer and forcibly setting the
 * parser encoding is not yet implemented
 */
dom_xml_parser *dom_xml_parser_create(const char *enc, const char *int_enc,
		dom_msg msg, void *mctx, dom_document **document)
{
	dom_xml_parser *parser;
	dom_exception err;
	int ret;

	UNUSED(enc);
	UNUSED(int_enc);

	parser = dom_xml_alloc(NULL, sizeof(dom_xml_parser), NULL);
	if (parser == NULL) {
		msg(DOM_MSG_CRITICAL, mctx, "No memory for parser");
		return NULL;
	}

	parser->xml_ctx =
		xmlCreatePushParserCtxt(&sax_handler, parser, "", 0, NULL);
	if (parser->xml_ctx == NULL) {
		msg(DOM_MSG_CRITICAL, mctx, "Failed to create XML parser");
		dom_xml_alloc(parser, 0, NULL);
		return NULL;
	}

	/* Set options of parsing context */
	ret = xmlCtxtUseOptions(parser->xml_ctx, XML_PARSE_DTDATTR | 
			XML_PARSE_DTDLOAD);
	if (ret != 0) {
		msg(DOM_MSG_CRITICAL, mctx, "Failed setting parser options");
		xmlFreeParserCtxt(parser->xml_ctx);
		dom_xml_alloc(parser, 0, NULL);
		return NULL;
	}

	err = dom_implementation_create_document(
			DOM_IMPLEMENTATION_XML,
			/* namespace */ NULL,
			/* qname */ NULL,
			/* doctype */ NULL,
			NULL,
			NULL,
			document);

	if (err != DOM_NO_ERR) {
		msg(DOM_MSG_CRITICAL, mctx, "Failed creating document");
		xmlFreeParserCtxt(parser->xml_ctx);
		dom_xml_alloc(parser, 0, NULL);
		return NULL;
	}

	parser->doc = (dom_document *) dom_node_ref(*document);
	parser->current = dom_node_ref(parser->doc);
	parser->msg = msg;
	parser->mctx = mctx;
	parser->err = DOM_NO_ERR;

	return parser;
}

/**
 * Destroy an XML parser instance
 *
 * \param parser  The parser instance to destroy
 */
void dom_xml_parser_destroy(dom_xml_parser *parser)
{
	xmlFreeDoc(parser->xml_ctx->myDoc);
	xmlFreeParserCtxt(parser->xml_ctx);
	dom_node_unref(parser->current);
	dom_node_unref(parser->doc);
	dom_xml_alloc(parser, 0, NULL);
}

/**
 * Parse a chunk of data
 *
 * \param parser  The XML parser instance to use for parsing
 * \param data    Pointer to data chunk
 * \param len     Byte length of data chunk
 * \return DOM_XML_OK on success, DOM_XML_EXTERNAL_ERR | libxml error on failure
 */
dom_xml_error dom_xml_parser_parse_chunk(dom_xml_parser *parser,
		uint8_t *data, size_t len)
{
	xmlParserErrors err;

	err = xmlParseChunk(parser->xml_ctx, (char *) data, len, 0);
	if (err != XML_ERR_OK) {
		parser->msg(DOM_MSG_ERROR, parser->mctx, 
				"xmlParseChunk failed: %d", err);
		return DOM_XML_EXTERNAL_ERR | err;
	}

	if (parser->err != DOM_NO_ERR) {
		return DOM_XML_DOM_ERR | parser->err;
	}

	return DOM_XML_OK;
}

/**
 * Notify parser that datastream is empty
 *
 * \param parser  The XML parser instance to notify
 * \return DOM_XML_OK on success, DOM_XML_EXTERNAL_ERR | libxml error on failure
 *
 * This will force any remaining data through the parser
 */
dom_xml_error dom_xml_parser_completed(dom_xml_parser *parser)
{
	xmlParserErrors err;

	err = xmlParseChunk(parser->xml_ctx, "", 0, 1);
	if (err != XML_ERR_OK) {
		parser->msg(DOM_MSG_ERROR, parser->mctx,
				"xmlParseChunk failed: %d", err);
		return DOM_XML_EXTERNAL_ERR | err;
	}

	if (parser->err != DOM_NO_ERR) {
		return DOM_XML_DOM_ERR | parser->err;
	}

	return DOM_XML_OK;
}

static dom_exception
xml_parser_add_attribute(dom_xml_parser *parser, dom_element *elem,
		const xmlChar *localname, const xmlChar *prefix,
		const xmlChar *URI, const xmlChar *value, size_t len)
{
	dom_string *namespace = NULL;
	dom_string *name, *val;
	dom_exception err;

#ifdef DEBUG_XML_PARSER
	parser->msg(DOM_MSG_DEBUG, parser->mctx,
			" Attr(%s:%s, %.*s)", prefix, localname,
			len, value);
#endif

	if (URI != NULL) {
		err = dom_string_create_interned(
				(const uint8_t *) URI,
				strlen((const char *) URI),
				&namespace);
		if (err != DOM_NO_ERR) {
			parser->msg(DOM_MSG_CRITICAL, parser->mctx,
					"No memory for attr namespace");
			return err;
		}
	}

	if (prefix != NULL) {
		size_t qlen = strlen((const char *) prefix) +
				strlen((const char *) localname) + 2;
		char *qname = dom_xml_alloc(NULL, qlen, NULL);
		if (qname == NULL) {
			parser->msg(DOM_MSG_CRITICAL, parser->mctx,
					"No memory for attr qname");
			dom_string_unref(namespace);
			return DOM_NO_MEM_ERR;
		}
		if ((size_t) snprintf(qname, qlen, "%s:%s", prefix,
					localname) >= qlen) {
			parser->msg(DOM_MSG_CRITICAL, parser->mctx,
					"Parts too large for attr qname");
			dom_xml_alloc(qname, 0, NULL);
			dom_string_unref(namespace);
			return DOM_NO_MEM_ERR;
		}
		err = dom_string_create_interned(
				(const uint8_t *) qname, qlen-1, &name);
		if (err != DOM_NO_ERR) {
			parser->msg(DOM_MSG_CRITICAL, parser->mctx,
					"No memory for attribute name");
			dom_xml_alloc(qname, 0, NULL);
			dom_string_unref(namespace);
			return err;
		}
		dom_xml_alloc(qname, 0, NULL);
	} else {
		err = dom_string_create_interned(
				(const uint8_t *) localname,
				strlen((const char *) localname),
				&name);
		if (err != DOM_NO_ERR) {
			parser->msg(DOM_MSG_CRITICAL, parser->mctx,
					"No memory for attribute name");
			dom_string_unref(namespace);
			return err;
		}
	}

	err = dom_string_create((const uint8_t *) value, len, &val);
	if (err != DOM_NO_ERR) {
		parser->msg(DOM_MSG_CRITICAL, parser->mctx,
				"No memory for attribute value");
		dom_string_unref(name);
		dom_string_unref(namespace);
		return err;
	}

	if (namespace != NULL) {
		err = dom_element_set_attribute_ns(elem, namespace, name, val);
	} else {
		err = dom_element_set_attribute(elem, name, val);
	}

	dom_string_unref(val);
	dom_string_unref(name);
	dom_string_unref(namespace);

	if (err != DOM_NO_ERR) {
		parser->msg(DOM_MSG_CRITICAL, parser->mctx,
				"Failed setting attribute");
	}
	return err;
}

/**
 * Handle an element open SAX event
 *
 * \param ctx            The callback context
 * \param localname      The local name of the element
 * \param prefix         The element namespace prefix
 * \param URI            The element namespace URI
 * \param nb_namespaces  The number of namespace definitions
 * \param namespaces     Array of nb_namespaces prefix/URI pairs
 * \param nb_attributes  The total number of attributes
 * \param nb_defaulted   The number of defaulted attributes
 * \param attributes     Array of nb_attributes attribute values
 *
 * The number of non-defaulted attributes is \p nb_attributes - \p nb_defaulted
 * The defaulted attributes are at the end of the array \p attributes.
 */
void xml_parser_start_element_ns(void *ctx, const xmlChar *localname,
		const xmlChar *prefix, const xmlChar *URI,
		int nb_namespaces, const xmlChar **namespaces,
		int nb_attributes, int nb_defaulted,
		const xmlChar **attributes)
{
	dom_xml_parser *parser = (dom_xml_parser *) ctx;
	dom_element *elem, *ins_elem;
	dom_string *tag_name;
	dom_string *namespace = NULL;
	int attr;

#ifdef DEBUG_XML_PARSER
	parser->msg(DOM_MSG_DEBUG, parser->mctx,
		"StartElementNS(%s:%s)", prefix, localname);
#endif

	if (parser->err != DOM_NO_ERR)
		return;

	if (URI != NULL) {
		parser->err = dom_string_create_interned(
				(const uint8_t *) URI,
				strlen((const char *) URI),
				&namespace);
		if (parser->err != DOM_NO_ERR) {
			parser->msg(DOM_MSG_CRITICAL, parser->mctx,
				    "No memory for namespace name");
			return;
		}
	}

	if (prefix != NULL) {
		size_t len = strlen((const char *) prefix) +
				strlen((const char *) localname) + 2;
		char *qname = dom_xml_alloc(NULL, len, NULL);
		if (qname == NULL) {
			parser->msg(DOM_MSG_CRITICAL, parser->mctx,
				    "No memory for qname");
			dom_string_unref(namespace);
			return;
		}
		if ((size_t) snprintf(qname, len, "%s:%s", prefix,
				localname) >= len) {
			parser->msg(DOM_MSG_CRITICAL, parser->mctx,
				    "Parts too large for qname");
			dom_xml_alloc(qname, 0, NULL);
			dom_string_unref(namespace);
			return;
		}
		parser->err = dom_string_create_interned(
				(const uint8_t *) qname, len-1,
				&tag_name);
		if (parser->err != DOM_NO_ERR) {
			parser->msg(DOM_MSG_CRITICAL, parser->mctx,
				    "No memory for tag name");
			dom_xml_alloc(qname, 0, NULL);
			dom_string_unref(namespace);
			return;
		}
		dom_xml_alloc(qname, 0, NULL);
	} else {
		parser->err = dom_string_create_interned(
				(const uint8_t *) localname,
				strlen((const char *) localname),
				&tag_name);
		if (parser->err != DOM_NO_ERR) {
			parser->msg(DOM_MSG_CRITICAL, parser->mctx,
				    "No memory for tag name");
			dom_string_unref(namespace);
			return;
		}
	}

	if (namespace == NULL) {
		parser->err = dom_document_create_element(parser->doc,
				tag_name, &elem);
	} else {
		parser->err = dom_document_create_element_ns(parser->doc,
				namespace, tag_name, &elem);
	}

	dom_string_unref(tag_name);
	dom_string_unref(namespace);

	if (parser->err != DOM_NO_ERR) {
		parser->msg(DOM_MSG_CRITICAL, parser->mctx,
			    "Failed to create element '%s'", localname);
		return;
	}

	UNUSED(nb_namespaces);
	UNUSED(namespaces);

	/* Add attributes to element */
	UNUSED(nb_defaulted);
	for (attr = 0; attr < nb_attributes; attr++) {
		const xmlChar *a_localname = attributes[attr * 5 + 0];
		const xmlChar *a_prefix = attributes[attr * 5 + 1];
		const xmlChar *a_URI = attributes[attr * 5 + 2];
		const xmlChar *a_value = attributes[attr * 5 + 3];
		const xmlChar *a_end = attributes[attr * 5 + 4];

		parser->err = xml_parser_add_attribute(parser, elem,
				a_localname, a_prefix, a_URI, a_value,
				a_end - a_value);
		if (parser->err != DOM_NO_ERR) {
			dom_node_unref(elem);
			return;
		}
	}

	parser->err = dom_node_append_child(parser->current,
			(struct dom_node *) elem,
			(struct dom_node **) (void *) &ins_elem);
	dom_node_unref(elem);

	if (parser->err != DOM_NO_ERR) {
		parser->msg(DOM_MSG_CRITICAL, parser->mctx,
				"Failed appending child node");
		return;
	}

	dom_node_unref(parser->current);
	parser->current = (struct dom_node *) ins_elem; /* Steal ref */
}

/**
 * Handle an element close SAX event
 *
 * \param ctx        The callback context
 * \param localname  The local name of the element
 * \param prefix     The element namespace prefix
 * \param URI        The element namespace URI
 */
void xml_parser_end_element_ns(void *ctx, const xmlChar *localname,
		const xmlChar *prefix, const xmlChar *URI)
{
	dom_xml_parser *parser = (dom_xml_parser *) ctx;
	dom_node *parent = NULL;

	UNUSED(localname);
	UNUSED(prefix);
	UNUSED(URI);

#ifdef DEBUG_XML_PARSER
	parser->msg(DOM_MSG_DEBUG, parser->mctx,
		"EndElementNS(%s:%s)", prefix, localname);
#endif

	if (parser->err != DOM_NO_ERR)
		return;

	parser->err = dom_node_get_parent_node(parser->current, &parent);
	if (parser->err != DOM_NO_ERR) {
		parser->msg(DOM_MSG_CRITICAL, parser->mctx,
			    "Unable to find a parent while closing element.");
		return;
	}

	if (parent == NULL) {
		parser->msg(DOM_MSG_CRITICAL, parser->mctx,
			    "Attempted to close more than was opened. (%s:%s %s)",
			    prefix, localname, URI);
		return;
	}

	dom_node_unref(parser->current);
	parser->current = parent;  /* Steal ref */
}

void xml_parser_reference(void *ctx, const xmlChar *name)
{
	dom_xml_parser *parser = (dom_xml_parser *) ctx;
	dom_string *data;
	dom_entity_reference *ref, *ins_ref;

#ifdef DEBUG_XML_PARSER
	parser->msg(DOM_MSG_DEBUG, parser->mctx,
		"Reference(%s)", name);
#endif

	if (parser->err != DOM_NO_ERR)
		return;

	parser->err = dom_string_create((const uint8_t *) name,
			strlen((const char *) name), &data);
	if (parser->err != DOM_NO_ERR) {
		parser->msg(DOM_MSG_CRITICAL, parser->mctx,
			    "No memory for entity reference");
		return;
	}

	parser->err = dom_document_create_entity_reference(
			parser->doc, data, &ref);
	dom_string_unref(data);

	if (parser->err != DOM_NO_ERR) {
		parser->msg(DOM_MSG_CRITICAL, parser->mctx,
				"Failed creating entity reference");
		return;
	}

	/* The subtree of an entity reference should be a copy of
	 * the corresponding Entity. However, as we don't have support
	 * for any of this, leave the subtree empty. */

	parser->err = dom_node_append_child(parser->current, ref, &ins_ref);
	if (parser->err != DOM_NO_ERR) {
		parser->msg(DOM_MSG_ERROR, parser->mctx,
				"Failed attaching entity reference");
		dom_node_unref(ref);
		return;
	}

	dom_node_unref(ins_ref);
	dom_node_unref(ref);
}

void xml_parser_characters(void *ctx, const xmlChar *ch, int len)
{
	dom_xml_parser *parser = (dom_xml_parser *) ctx;
	dom_string *data;
	dom_text *text, *ins_text;
	dom_node *lastchild = NULL;
	dom_node_type ntype = 0;

#ifdef DEBUG_XML_PARSER
	parser->msg(DOM_MSG_DEBUG, parser->mctx,
		"Characters(%.*s)", len, ch);
#endif

	if (parser->err != DOM_NO_ERR)
		return;

	parser->err = dom_string_create((const uint8_t *) ch, len, &data);
	if (parser->err != DOM_NO_ERR) {
		parser->msg(DOM_MSG_CRITICAL, parser->mctx,
			    "No memory for text contents");
		return;
	}

	parser->err = dom_node_get_last_child(parser->current, &lastchild);
	if (parser->err == DOM_NO_ERR && lastchild != NULL) {
		parser->err = dom_node_get_node_type(lastchild, &ntype);
	}
	if (parser->err != DOM_NO_ERR) {
		parser->msg(DOM_MSG_CRITICAL, parser->mctx,
			    "Failed determining type of last child");
		dom_string_unref(data);
		dom_node_unref(lastchild);
		return;
	}

	if (ntype == DOM_TEXT_NODE) {
		/* We can append this text instead */
		parser->err = dom_characterdata_append_data(
			(dom_characterdata *) lastchild, data);
		dom_string_unref(data);
		dom_node_unref(lastchild);
		if (parser->err != DOM_NO_ERR) {
			parser->msg(DOM_MSG_CRITICAL, parser->mctx,
				    "Failed appending to text node");
		}
		return;
	}

	dom_node_unref(lastchild);

	/* We can't append directly, so make a new node */
	parser->err = dom_document_create_text_node(parser->doc, data, &text);
	dom_string_unref(data);

	if (parser->err != DOM_NO_ERR) {
		parser->msg(DOM_MSG_CRITICAL, parser->mctx,
			    "No memory for text node");
		return;
	}

	parser->err = dom_node_append_child(parser->current, text, &ins_text);
	if (parser->err != DOM_NO_ERR) {
		parser->msg(DOM_MSG_ERROR, parser->mctx,
				"Failed attaching text node");
		dom_node_unref(text);
		return;
	}

	dom_node_unref(ins_text);
	dom_node_unref(text);
}

void xml_parser_comment(void *ctx, const xmlChar *value)
{
	dom_xml_parser *parser = (dom_xml_parser *) ctx;
	dom_comment *comment, *ins_comment = NULL;
	dom_string *data;

#ifdef DEBUG_XML_PARSER
	parser->msg(DOM_MSG_DEBUG, parser->mctx,
		"Comment(%s)", value);
#endif

	if (parser->err != DOM_NO_ERR)
		return;

	parser->err = dom_string_create((const uint8_t *) value,
			strlen((const char *) value), &data);
	if (parser->err != DOM_NO_ERR) {
		parser->msg(DOM_MSG_CRITICAL, parser->mctx,
				"No memory for comment data");
		return;
	}

	parser->err = dom_document_create_comment(parser->doc, data, &comment);
	dom_string_unref(data);

	if (parser->err != DOM_NO_ERR) {
		parser->msg(DOM_MSG_CRITICAL, parser->mctx,
				"No memory for comment node");
		return;
	}


	parser->err = dom_node_append_child(parser->current, comment,
			&ins_comment);
	if (parser->err != DOM_NO_ERR) {
		parser->msg(DOM_MSG_CRITICAL, parser->mctx,
				"Failed attaching comment node");
		dom_node_unref(comment);
		return;
	}

	dom_node_unref(ins_comment);
	dom_node_unref(comment);
}

void xml_parser_cdata_block(void *ctx, const xmlChar *value, int len)
{
	dom_xml_parser *parser = (dom_xml_parser *) ctx;
	dom_string *data;
	dom_cdata_section *cdata, *ins_cdata;

#ifdef DEBUG_XML_PARSER
	parser->msg(DOM_MSG_DEBUG, parser->mctx,
		"CDATA(%.*s)", len, value);
#endif

	if (parser->err != DOM_NO_ERR)
		return;

	parser->err = dom_string_create((const uint8_t *) value, len, &data);
	if (parser->err != DOM_NO_ERR) {
		parser->msg(DOM_MSG_CRITICAL, parser->mctx,
			    "No memory for cdata section contents");
		return;
	}

	parser->err = dom_document_create_cdata_section(parser->doc, data,
			&cdata);
	dom_string_unref(data);

	if (parser->err != DOM_NO_ERR) {
		parser->msg(DOM_MSG_CRITICAL, parser->mctx,
			    "No memory for cdata section");
		return;
	}

	parser->err = dom_node_append_child(parser->current, cdata,
			&ins_cdata);
	if (parser->err != DOM_NO_ERR) {
		parser->msg(DOM_MSG_ERROR, parser->mctx,
				"Failed attaching cdata section");
		dom_node_unref(cdata);
		return;
	}

	dom_node_unref(ins_cdata);
	dom_node_unref(cdata);
}

int xml_parser_is_standalone(void *ctx)
{
	dom_xml_parser *parser = (dom_xml_parser *) ctx;

#ifdef DEBUG_XML_PARSER
	parser->msg(DOM_MSG_DEBUG, parser->mctx, "IsStandalone");
#endif

	if (parser->err != DOM_NO_ERR)
		return 0;

	return xmlSAX2IsStandalone(parser->xml_ctx);
}

int xml_parser_has_internal_subset(void *ctx)
{
	dom_xml_parser *parser = (dom_xml_parser *) ctx;

#ifdef DEBUG_XML_PARSER
	parser->msg(DOM_MSG_DEBUG, parser->mctx, "HasInternalSubset");
#endif

	if (parser->err != DOM_NO_ERR)
		return 0;

	return xmlSAX2HasInternalSubset(parser->xml_ctx);
}

int xml_parser_has_external_subset(void *ctx)
{
	dom_xml_parser *parser = (dom_xml_parser *) ctx;

#ifdef DEBUG_XML_PARSER
	parser->msg(DOM_MSG_DEBUG, parser->mctx, "HasExternalSubset");
#endif

	if (parser->err != DOM_NO_ERR)
		return 0;

	return xmlSAX2HasExternalSubset(parser->xml_ctx);
}

xmlParserInputPtr xml_parser_resolve_entity(void *ctx,
		const xmlChar *publicId, const xmlChar *systemId)
{
	dom_xml_parser *parser = (dom_xml_parser *) ctx;

#ifdef DEBUG_XML_PARSER
	parser->msg(DOM_MSG_DEBUG, parser->mctx,
		"ResolveEntity(%s, %s)", publicId, systemId);
#endif

	if (parser->err != DOM_NO_ERR)
		return NULL;

	return xmlSAX2ResolveEntity(parser->xml_ctx, publicId, systemId);
}

xmlEntityPtr xml_parser_get_entity(void *ctx, const xmlChar *name)
{
	dom_xml_parser *parser = (dom_xml_parser *) ctx;

#ifdef DEBUG_XML_PARSER
	parser->msg(DOM_MSG_DEBUG, parser->mctx,
		"GetEntity(%s)", name);
#endif

	if (parser->err != DOM_NO_ERR)
		return NULL;

	return xmlSAX2GetEntity(parser->xml_ctx, name);
}

xmlEntityPtr xml_parser_get_parameter_entity(void *ctx, const xmlChar *name)
{
	dom_xml_parser *parser = (dom_xml_parser *) ctx;

#ifdef DEBUG_XML_PARSER
	parser->msg(DOM_MSG_DEBUG, parser->mctx,
		"GetParameterEntity(%s)", name);
#endif

	if (parser->err != DOM_NO_ERR)
		return NULL;

	return xmlSAX2GetParameterEntity(parser->xml_ctx, name);
}

void xml_parser_start_document(void *ctx)
{
	dom_xml_parser *parser = (dom_xml_parser *) ctx;

#ifdef DEBUG_XML_PARSER
	parser->msg(DOM_MSG_DEBUG, parser->mctx, "StartDocument");
#endif

	if (parser->err != DOM_NO_ERR)
		return;

	if (parser->current != (dom_node *) parser->doc) {
		parser->msg(DOM_MSG_CRITICAL, parser->mctx,
				"Unexpected elements before document start");
		return;
	}

	xmlSAX2StartDocument(parser->xml_ctx);
}

void xml_parser_end_document(void *ctx)
{
	dom_xml_parser *parser = (dom_xml_parser *) ctx;

#ifdef DEBUG_XML_PARSER
	parser->msg(DOM_MSG_DEBUG, parser->mctx, "EndDocument");
#endif

	if (parser->err != DOM_NO_ERR)
		return;

	if (parser->current != (dom_node *) parser->doc) {
		parser->msg(DOM_MSG_CRITICAL, parser->mctx,
				"Unclosed elements before document end");
		return;
	}

	xmlSAX2EndDocument(parser->xml_ctx);
}

void xml_parser_internal_subset(void *ctx, const xmlChar *name,
		const xmlChar *ExternalID, const xmlChar *SystemID)
{
	dom_xml_parser *parser = (dom_xml_parser *) ctx;
	dom_document_type *doctype, *ins_doctype = NULL;
#if LIBXML_VERSION >= 21200
	const xmlError *xmlerr;
#else
	xmlErrorPtr xmlerr;
#endif

#ifdef DEBUG_XML_PARSER
	parser->msg(DOM_MSG_DEBUG, parser->mctx,
		"InternalSubset(%s, %s, %s)", name, ExternalID, SystemID);
#endif

	if (parser->err != DOM_NO_ERR)
		return;

	if (parser->current != (dom_node *) parser->doc) {
		/* Ignore DTDs appearing after the root element */
		return;
	}

	xmlSAX2InternalSubset(parser->xml_ctx, name, ExternalID, SystemID);
	xmlerr = xmlCtxtGetLastError(parser->xml_ctx);
	if (xmlerr != NULL && xmlerr->level >= XML_ERR_ERROR) {
		parser->msg(DOM_MSG_CRITICAL, parser->mctx,
				"Failed creating internal subset");
		return;
	}

	parser->err = dom_implementation_create_document_type(
		(const char *) name,
		ExternalID ? (const char *) ExternalID : "",
		SystemID ? (const char *) SystemID : "",
		&doctype);

	if (parser->err != DOM_NO_ERR) {
		parser->msg(DOM_MSG_CRITICAL, parser->mctx,
				"Failed to create document type");
		return;
	}

	parser->err = dom_node_append_child(parser->doc, doctype,
			&ins_doctype);
	if (parser->err != DOM_NO_ERR) {
		parser->msg(DOM_MSG_CRITICAL, parser->mctx,
					"Failed attaching doctype");
		dom_node_unref(doctype);
		return;
	}

	dom_node_unref(ins_doctype);
	dom_node_unref(doctype);
}

void xml_parser_external_subset(void *ctx, const xmlChar *name,
		const xmlChar *ExternalID, const xmlChar *SystemID)
{
	dom_xml_parser *parser = (dom_xml_parser *) ctx;
#if LIBXML_VERSION >= 21200
	const xmlError *xmlerr;
#else
	xmlErrorPtr xmlerr;
#endif

#ifdef DEBUG_XML_PARSER
	parser->msg(DOM_MSG_DEBUG, parser->mctx,
		"ExternalSubset(%s, %s, %s)", name, ExternalID, SystemID);
#endif

	if (parser->err != DOM_NO_ERR)
		return;

	if (parser->current != (dom_node *) parser->doc) {
		/* Ignore DTDs appearing after the root element */
		return;
	}

	xmlSAX2ExternalSubset(parser->xml_ctx, name, ExternalID, SystemID);
	xmlerr = xmlCtxtGetLastError(parser->xml_ctx);
	if (xmlerr != NULL && xmlerr->level >= XML_ERR_ERROR) {
		parser->msg(DOM_MSG_CRITICAL, parser->mctx,
				"Failed creating external subset");
		return;
	}
}

void xml_parser_entity_decl(void *ctx, const xmlChar *name,
		int type, const xmlChar *publicId, const xmlChar *systemId,
		xmlChar *content)
{
	dom_xml_parser *parser = (dom_xml_parser *) ctx;

#ifdef DEBUG_XML_PARSER
	parser->msg(DOM_MSG_DEBUG, parser->mctx,
		"EntityDecl(%s, %d, %s, %s, %s)", name, type, publicId,
		systemId, content);
#endif

	if (parser->err != DOM_NO_ERR)
		return;

	if (parser->current != (dom_node *) parser->doc) {
		/* Ignore DTDs appearing after the root element */
		return;
	}

	xmlSAX2EntityDecl(parser->xml_ctx, name, type, publicId, systemId,
			content);
}

void xml_parser_notation_decl(void *ctx, const xmlChar *name,
		const xmlChar *publicId, const xmlChar *systemId)
{
	dom_xml_parser *parser = (dom_xml_parser *) ctx;

#ifdef DEBUG_XML_PARSER
	parser->msg(DOM_MSG_DEBUG, parser->mctx,
		"NotationDecl(%s, %s, %s)", name, publicId, systemId);
#endif

	if (parser->err != DOM_NO_ERR)
		return;

	if (parser->current != (dom_node *) parser->doc) {
		/* Ignore DTDs appearing after the root element */
		return;
	}

	xmlSAX2NotationDecl(parser->xml_ctx, name, publicId, systemId);
}

void xml_parser_attribute_decl(void *ctx, const xmlChar *elem,
		const xmlChar *fullname, int type, int def,
		const xmlChar *defaultValue, xmlEnumerationPtr tree)
{
	dom_xml_parser *parser = (dom_xml_parser *) ctx;

#ifdef DEBUG_XML_PARSER
	parser->msg(DOM_MSG_DEBUG, parser->mctx,
		"AttributeDecl(%s, %s)", elem, fullname);
#endif

	if (parser->err != DOM_NO_ERR)
		return;

	if (parser->current != (dom_node *) parser->doc) {
		/* Ignore DTDs appearing after the root element */
		return;
	}

	xmlSAX2AttributeDecl(parser->xml_ctx, elem, fullname, type, def,
			defaultValue, tree);
}

void xml_parser_element_decl(void *ctx, const xmlChar *name,
		int type, xmlElementContentPtr content)
{
	dom_xml_parser *parser = (dom_xml_parser *) ctx;

#ifdef DEBUG_XML_PARSER
	parser->msg(DOM_MSG_DEBUG, parser->mctx,
		"ElementDecl(%s, %d)", name, type);
#endif

	if (parser->err != DOM_NO_ERR)
		return;

	if (parser->current != (dom_node *) parser->doc) {
		/* Ignore DTDs appearing after the root element */
		return;
	}

	xmlSAX2ElementDecl(parser->xml_ctx, name, type, content);
}

void xml_parser_unparsed_entity_decl(void *ctx, const xmlChar *name,
		const xmlChar *publicId, const xmlChar *systemId,
		const xmlChar *notationName)
{
	dom_xml_parser *parser = (dom_xml_parser *) ctx;

#ifdef DEBUG_XML_PARSER
	parser->msg(DOM_MSG_DEBUG, parser->mctx,
		"UnparsedEntityDecl(%s, %s, %s, %s)", name, publicId,
		systemId, notationName);
#endif

	if (parser->err != DOM_NO_ERR)
		return;

	if (parser->current != (dom_node *) parser->doc) {
		/* Ignore DTDs appearing after the root element */
		return;
	}

	xmlSAX2UnparsedEntityDecl(parser->xml_ctx, name, publicId, systemId,
			notationName);
}
