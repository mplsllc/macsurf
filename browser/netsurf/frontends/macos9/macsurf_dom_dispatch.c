/*
 * MacSurf — macsurf_dom_dispatch.c
 *
 * Provides external linkage for LibDOM functions that are defined as
 * static inline in the LibDOM headers. 
 */

#include <string.h>

#include <dom/dom.h>
#include <dom/core/node.h>
#include <dom/core/element.h>
#include <dom/core/document.h>
#include <dom/core/string.h>
#include <dom/core/characterdata.h>

void macsurf_dom_node_ref(dom_node *node)
{
    dom_node_ref(node);
}

void macsurf_dom_node_unref(dom_node *node)
{
    dom_node_unref(node);
}

void macsurf_dom_string_unref(dom_string *str)
{
    dom_string_unref(str);
}

dom_exception macsurf_dom_document_get_element_by_id(dom_document *doc,
    dom_string *id, dom_element **element)
{
    return dom_document_get_element_by_id(doc, id, element);
}

/* fixes382 (M1) — JS->DOM->render: the JS bridge needs the real <html>
 * root to expose document.documentElement/body/head as wrapped elements. */
dom_exception macsurf_dom_document_get_document_element(dom_document *doc,
    dom_element **result)
{
    return dom_document_get_document_element(doc, result);
}

dom_exception macsurf_dom_document_create_element(dom_document *doc, 
    dom_string *tag_name, dom_element **element)
{
    return dom_document_create_element(doc, tag_name, element);
}

dom_exception macsurf_dom_document_create_text_node(dom_document *doc,
    dom_string *data, dom_text **text)
{
    return dom_document_create_text_node(doc, data, text);
}

dom_exception macsurf_dom_element_get_tag_name(dom_element *el, 
    dom_string **name)
{
    return dom_element_get_tag_name(el, name);
}

dom_exception macsurf_dom_element_get_attribute(dom_element *el, 
    dom_string *name, dom_string **value)
{
    return dom_element_get_attribute(el, name, value);
}

dom_exception macsurf_dom_element_set_attribute(dom_element *el, 
    dom_string *name, dom_string *value)
{
    return dom_element_set_attribute(el, name, value);
}

dom_exception macsurf_dom_node_append_child(dom_node *parent, 
    dom_node *new_child, dom_node **result)
{
    return dom_node_append_child(parent, new_child, result);
}

dom_exception macsurf_dom_node_remove_child(dom_node *parent,
    dom_node *old_child, dom_node **result)
{
    return dom_node_remove_child(parent, old_child, result);
}

/* fixes385 (M4) — ordered insertion (React reconciler inserts before a
 * reference node rather than append-only). */
dom_exception macsurf_dom_node_insert_before(dom_node *parent,
    dom_node *new_child, dom_node *ref_child, dom_node **result)
{
    return dom_node_insert_before(parent, new_child, ref_child, result);
}

dom_exception macsurf_dom_node_get_node_type(dom_node *node,
    dom_node_type *result)
{
    return dom_node_get_node_type(node, result);
}

dom_exception macsurf_dom_node_get_first_child(dom_node *node,
    dom_node **result)
{
    return dom_node_get_first_child(node, result);
}

dom_exception macsurf_dom_node_get_next_sibling(dom_node *node,
    dom_node **result)
{
    return dom_node_get_next_sibling(node, result);
}

dom_exception macsurf_dom_node_get_node_name(dom_node *node,
    dom_string **result)
{
    return dom_node_get_node_name(node, result);
}

dom_exception macsurf_dom_node_get_node_value(dom_node *node,
    dom_string **result)
{
    return dom_node_get_node_value(node, result);
}

dom_exception macsurf_dom_node_set_node_value(dom_node *node,
    dom_string *value)
{
    return dom_node_set_node_value(node, value);
}

/* fixes319d — text-content accessors. Inline in dom/core/node.h; the
 * JS bridge needs an external symbol it can extern-link. */
dom_exception macsurf_dom_node_get_text_content(dom_node *node,
    dom_string **result)
{
    return dom_node_get_text_content(node, result);
}

dom_exception macsurf_dom_node_set_text_content(dom_node *node,
    dom_string *content)
{
    return dom_node_set_text_content(node, content);
}

dom_exception macsurf_dom_node_get_parent_node(dom_node *node,
    dom_node **result)
{
    return dom_node_get_parent_node(node, result);
}

/* fixes867 (#293) — owner document, for the DOM-mutation failure diagnostic in
 * macsurf_qjs.c (qjs_dom_mut_check).  Distinguishing WRONG_DOCUMENT_ERR from the
 * mutation-semaphore rejection needs to compare the child's owner against the
 * parent's, and both look identical from JS otherwise.  On the base
 * dom_node_vtable, so it is safe for element/text/comment/fragment alike --
 * unlike the dom_element_* ops (see qjs_wrap_fragment's note on the vtable-shape
 * hazard).  Returns an owned ref the caller must unref. */
dom_exception macsurf_dom_node_get_owner_document(dom_node *node,
    dom_document **result)
{
    return dom_node_get_owner_document(node, result);
}

dom_exception macsurf_dom_node_get_previous_sibling(dom_node *node,
    dom_node **result)
{
    return dom_node_get_previous_sibling(node, result);
}

dom_exception macsurf_dom_node_get_last_child(dom_node *node,
    dom_node **result)
{
    return dom_node_get_last_child(node, result);
}

dom_exception macsurf_dom_element_has_attribute(dom_element *el,
    dom_string *name, int *result)
{
    bool b = false;
    dom_exception exc = dom_element_has_attribute(el, name, &b);
    *result = b ? 1 : 0;
    return exc;
}

dom_exception macsurf_dom_element_remove_attribute(dom_element *el,
    dom_string *name)
{
    return dom_element_remove_attribute(el, name);
}

dom_exception macsurf_dom_document_create_element_s(dom_document *doc,
    const char *tag, dom_element **element)
{
    dom_string *ds = NULL;
    dom_exception exc;
    dom_string_create((const uint8_t *)tag, (unsigned)strlen(tag), &ds);
    if (ds == NULL) return 5; /* DOM_NO_MEMORY_ERR */
    exc = dom_document_create_element(doc, ds, element);
    dom_string_unref(ds);
    return exc;
}

/* fixes870 (#297) — document.createElementNS() from plain JS C strings.
 *
 * This is Preact's ONLY element factory -- its renderer never calls
 * createElement at all:
 *     e = document.createElementNS(a, k, w.is && w)
 * so with no createElementNS, a Preact app renders NOTHING.
 *
 * Uses the REAL namespaced create rather than falling back to
 * create_element_s(), because the namespace is not cosmetic here: hubbub tags
 * HTML elements HUBBUB_NS_HTML, and the parser binding then builds them with
 * dom_document_create_element_ns(doc, dom_namespaces[HUBBUB_NS_HTML], ...) --
 * i.e. every parser-built element is in the XHTML namespace. Going through this
 * path makes a Preact-created <div> byte-identical to a parsed one, where
 * create_element_s() would give it a NULL namespace instead. It also gets SVG
 * right for free (libdom knows the namespace table; see libdom's
 * src/utils/namespace.c).
 *
 * `ns` NULL/empty => a null-namespace element, which is what
 * createElementNS(null, 'div') means per spec. */
dom_exception macsurf_dom_document_create_element_ns_s(dom_document *doc,
    const char *ns, const char *qname, dom_element **element)
{
    dom_string *ns_s = NULL;
    dom_string *qn_s = NULL;
    dom_exception exc;

    if (qname == NULL) return 5; /* DOM_NO_MEMORY_ERR */
    if (dom_string_create((const uint8_t *)qname, (unsigned)strlen(qname),
                          &qn_s) != DOM_NO_ERR || qn_s == NULL) {
        return 5;
    }
    if (ns != NULL && ns[0] != '\0') {
        if (dom_string_create((const uint8_t *)ns, (unsigned)strlen(ns),
                              &ns_s) != DOM_NO_ERR) {
            dom_string_unref(qn_s);
            return 5;
        }
    }
    exc = dom_document_create_element_ns(doc, ns_s, qn_s, element);
    if (ns_s != NULL) dom_string_unref(ns_s);
    dom_string_unref(qn_s);
    return exc;
}

/* fixes846 (#167 S3) — document.createTextNode() from a plain JS C string,
 * mirroring create_element_s above. */
dom_exception macsurf_dom_document_create_text_node_s(dom_document *doc,
    const char *data, dom_text **text)
{
    dom_string *ds = NULL;
    dom_exception exc;
    dom_string_create((const uint8_t *)data, (unsigned)strlen(data), &ds);
    if (ds == NULL) return 5; /* DOM_NO_MEMORY_ERR */
    exc = dom_document_create_text_node(doc, ds, text);
    dom_string_unref(ds);
    return exc;
}

/* fixes846 (#167 S3) — document.createDocumentFragment(). */
dom_exception macsurf_dom_document_create_document_fragment(dom_document *doc,
    dom_document_fragment **fragment)
{
    return dom_document_create_document_fragment(doc, fragment);
}

/* fixes846 (#167 S3) — text/comment node data accessors (dom_characterdata
 * is the shared vtable base for dom_text/dom_comment/dom_cdatasection; the
 * cast to dom_characterdata* mirrors how dom_element_* calls elsewhere in
 * this file take a dom_node* through similarly-shaped casts). */
dom_exception macsurf_dom_characterdata_get_data(dom_node *node,
    dom_string **data)
{
    return dom_characterdata_get_data((dom_characterdata *) node, data);
}

dom_exception macsurf_dom_characterdata_set_data_s(dom_node *node,
    const char *data)
{
    dom_string *ds = NULL;
    dom_exception exc;
    dom_string_create((const uint8_t *)data, (unsigned)strlen(data), &ds);
    if (ds == NULL) return 5; /* DOM_NO_MEMORY_ERR */
    exc = dom_characterdata_set_data((dom_characterdata *) node, ds);
    dom_string_unref(ds);
    return exc;
}

/* fixes878 — real cloneNode. The JS binding previously handed back the element
 * ITSELF, so the universal clone-and-append idiom
 *     parent.appendChild(tpl.cloneNode(true))
 * MOVED the original instead of copying it: pages rendered one relocated node
 * where they meant N copies, silently and with no error. This is libdom's own
 * virtual clone, so `deep` is honoured properly.
 *
 * The result carries a ref that the caller owns (libdom's clone returns a
 * ref'd node), matching dom_node_get_first_child et al above -- the QuickJS
 * wrappers adopt that transferred ref. */
dom_exception macsurf_dom_node_clone_node(dom_node *node, int deep,
    dom_node **result)
{
    return dom_node_clone_node(node, (bool) (deep != 0), result);
}

/* fixes878 — node.contains(). Non-virtual in libdom (see the comment at
 * dom/core/node.h:205), and it correctly reports true for the node itself,
 * which is what the DOM spec requires. `contains` used to be hardcoded
 * `return false`. */
dom_exception macsurf_dom_node_contains(dom_node *node, dom_node *other,
    int *contains)
{
    bool c = false;
    dom_exception exc;
    exc = dom_node_contains(node, other, &c);
    *contains = c ? 1 : 0;
    return exc;
}

