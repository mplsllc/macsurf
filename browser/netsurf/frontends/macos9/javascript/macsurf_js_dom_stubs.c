/*
 * MacSurf -- macsurf_js_dom_stubs.c  (fixes164)
 *
 * Out-of-line no-op implementations for the libdom public API that
 * macsurf_js_dom.c calls. Every function here is external linkage
 * so CW8 resolves the reference across .o files -- no static-local
 * ambiguity.
 *
 * History:
 *   fixes162 shipped macsurf_js_dom.c assuming the Mac had a stale
 *   copy. fixes163 dropped `static` from the in-TU stubs so they
 *   became externs within the same file. Both attempts still left
 *   the Mac build with the same 9 undefined-symbol link errors.
 *   Whatever CW8 is doing to the symbol table on this TU, splitting
 *   the definitions into a separate compilation unit sidesteps it.
 *
 * Add this file to MacSurf.mcp alongside macsurf_js_dom.c.
 *
 * Signatures match the forward declarations now in macsurf_js_dom.c.
 * If the real libdom binding lands (dom_parser.c port), delete this
 * file, remove the forward decls from macsurf_js_dom.c, and include
 * dom/dom.h instead.
 */

#include <stddef.h>

struct dom_document;
struct dom_element;
struct dom_node;
struct dom_string;
typedef struct dom_element  dom_element;
typedef struct dom_document dom_document;
typedef struct dom_node     dom_node;
typedef struct dom_string   dom_string;

typedef int dom_exception;
#define DOM_NO_ERR 0

dom_exception
dom_string_create(const unsigned char *ptr, size_t len, dom_string **str)
{
	(void)ptr; (void)len;
	if (str != NULL) *str = NULL;
	return DOM_NO_ERR;
}

void
dom_string_unref(dom_string *str)
{
	(void)str;
}

const char *
dom_string_data(const dom_string *str)
{
	(void)str;
	return "";
}

size_t
dom_string_length(const dom_string *str)
{
	(void)str;
	return 0;
}

void
dom_node_ref(dom_node *node)
{
	(void)node;
}

void
dom_node_unref(dom_node *node)
{
	(void)node;
}

dom_exception
dom_document_get_element_by_id(dom_document *doc, dom_string *id,
		dom_element **element)
{
	(void)doc; (void)id;
	if (element != NULL) *element = NULL;
	return DOM_NO_ERR;
}

dom_exception
dom_document_create_element(dom_document *doc, dom_string *tag_name,
		dom_element **element)
{
	(void)doc; (void)tag_name;
	if (element != NULL) *element = NULL;
	return DOM_NO_ERR;
}

dom_exception
dom_element_get_tag_name(dom_element *el, dom_string **name)
{
	(void)el;
	if (name != NULL) *name = NULL;
	return DOM_NO_ERR;
}

dom_exception
dom_element_get_attribute(dom_element *el, dom_string *name,
		dom_string **value)
{
	(void)el; (void)name;
	if (value != NULL) *value = NULL;
	return DOM_NO_ERR;
}

dom_exception
dom_element_set_attribute(dom_element *el, dom_string *name,
		dom_string *value)
{
	(void)el; (void)name; (void)value;
	return DOM_NO_ERR;
}

dom_exception
dom_node_append_child(dom_node *parent, dom_node *new_child,
		dom_node **result)
{
	(void)parent; (void)new_child;
	if (result != NULL) *result = NULL;
	return DOM_NO_ERR;
}
