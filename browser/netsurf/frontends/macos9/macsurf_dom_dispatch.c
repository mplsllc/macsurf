/*
 * MacSurf - macsurf_dom_dispatch.c
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
#include <dom/core/namednodemap.h>
#include <dom/core/attr.h>

#include "macsurf_debug_log.h"

/*
 * The Facebook OS 9/OS X mount-watch needs a record below the QuickJS
 * wrapper layer.  Keep it deliberately small: normal pages can make many
 * thousands of DOM calls while parsing, whereas a native parent whose id is
 * mount_* is the one parent whose mutations must never be sampled away.
 */
#define MACSURF_DOMMAKE_BUDGET 64
#define MACSURF_DOMMUT_BUDGET 128
#define MACSURF_DOM_CHILD_COUNT_CAP 512

static int s_macsurf_dommake_budget = MACSURF_DOMMAKE_BUDGET;
static int s_macsurf_dommut_budget = MACSURF_DOMMUT_BUDGET;

/* Five-call proof at the actual libdom-dispatch boundary.  The larger
 * DOMMAKE/DOMMUT audit is intentionally still bounded and mount-aware; these
 * records are not an audit, only the prerequisite proof that its path is
 * live. */
#define MACSURF_DOM_SENTINEL_LIMIT 5
static int s_doms_create_element = 0;
static int s_doms_create_element_ns = 0;
static int s_doms_create_text = 0;
static int s_doms_create_comment = 0;
static int s_doms_append = 0;
static int s_doms_remove = 0;
static int s_doms_insert = 0;
static int s_doms_textcontent = 0;
static int s_doms_nodevalue = 0;

static void macsurf_dom_dispatch_sentinel(const char *op, int *count,
        void *first, void *second)
{
    if (count == NULL || *count >= MACSURF_DOM_SENTINEL_LIMIT) return;
    (*count)++;
    macsurf_debug_log_writef(
        "LIFE DOMDISP op=%s n=%d first=%p second=%p",
        op, *count, first, second);
}

void macsurf_dom_sentinel_reset(void)
{
    s_doms_create_element = 0;
    s_doms_create_element_ns = 0;
    s_doms_create_text = 0;
    s_doms_create_comment = 0;
    s_doms_append = 0;
    s_doms_remove = 0;
    s_doms_insert = 0;
    s_doms_textcontent = 0;
    s_doms_nodevalue = 0;
}

/* Return non-zero only for a REAL libdom element whose native id begins
 * "mount_".  This intentionally does not inspect a QuickJS wrapper: wrapper
 * identity/lifetime is one of the failure modes this diagnostic separates. */
static int macsurf_dom_native_mount_id(dom_node *node, char *out, int cap)
{
	dom_node_type type = 0;
	dom_string *id_name = NULL;
	dom_string *id_value = NULL;
	const char *data = NULL;
	size_t len = 0;
	size_t copy = 0;
	int is_mount = 0;

	if (out != NULL && cap > 0) out[0] = '\0';
	if (node == NULL || out == NULL || cap <= 0)
		return 0;
	if (dom_node_get_node_type(node, &type) != DOM_NO_ERR ||
			type != DOM_ELEMENT_NODE)
		return 0;
	if (dom_string_create((const uint8_t *)"id", 2, &id_name) != DOM_NO_ERR ||
			id_name == NULL)
		return 0;
	if (dom_element_get_attribute((dom_element *)node, id_name, &id_value)
			!= DOM_NO_ERR || id_value == NULL)
		goto done;
	data = dom_string_data(id_value);
	len = dom_string_byte_length(id_value);
	if (data == NULL || len < 6 || memcmp(data, "mount_", 6) != 0)
		goto done;
	copy = len;
	if (copy > (size_t)(cap - 1)) copy = (size_t)(cap - 1);
	memcpy(out, data, copy);
	out[copy] = '\0';
	is_mount = 1;

done:
	if (id_value != NULL) dom_string_unref(id_value);
	dom_string_unref(id_name);
	return is_mount;
}

/* Exposed to macsurf_qjs.c solely to keep its wrapper-entry trace unlimited
 * for the same native mount parent as this dispatcher trace. */
int macsurf_dom_node_is_mount(dom_node *node)
{
	char mount_id[2];
	return macsurf_dom_native_mount_id(node, mount_id, (int)sizeof mount_id);
}

/* Count directly from the supplied libdom node, not from a JS wrapper.  A
 * corruption-induced sibling loop must not turn a diagnostic into a hang. */
static int macsurf_dom_native_child_count(dom_node *parent, int *capped)
{
	dom_node *child = NULL;
	dom_node *next = NULL;
	int count = 0;

	if (capped != NULL) *capped = 0;
	if (parent == NULL) return -1;
	if (dom_node_get_first_child(parent, &child) != DOM_NO_ERR)
		return -1;
	while (child != NULL) {
		count++;
		if (count >= MACSURF_DOM_CHILD_COUNT_CAP) {
			if (capped != NULL) *capped = 1;
			dom_node_unref(child);
			break;
		}
		next = NULL;
		if (dom_node_get_next_sibling(child, &next) != DOM_NO_ERR) {
			dom_node_unref(child);
			return -1;
		}
		dom_node_unref(child);
		child = next;
	}
	return count;
}

static int macsurf_dom_mutation_trace_begin(dom_node *parent,
		char *mount_id, int mount_id_cap)
{
	if (macsurf_dom_native_mount_id(parent, mount_id, mount_id_cap))
		return 1;
	if (s_macsurf_dommut_budget <= 0)
		return 0;
	s_macsurf_dommut_budget--;
	return 1;
}

static void macsurf_dom_trace_mutation(const char *op, dom_node *parent,
		dom_node *child, dom_node *ref, dom_node **result,
		dom_exception exc, int before, int after, int before_capped,
		int after_capped, const char *mount_id)
{
	dom_node *returned = NULL;

	if (exc == DOM_NO_ERR && result != NULL) returned = *result;
	macsurf_debug_log_writef(
		"LIFE DOMMUT %s parent=%p child=%p ref=%p ret=%p exc=%d "
		"before=%d after=%d bcap=%d acap=%d mount=%s",
		op, (void *)parent, (void *)child, (void *)ref, (void *)returned,
		(int)exc, before, after, before_capped, after_capped,
		(mount_id != NULL && mount_id[0] != '\0') ? mount_id : "-");
}

static int macsurf_dom_make_trace_take(void)
{
	if (s_macsurf_dommake_budget <= 0) return 0;
	s_macsurf_dommake_budget--;
	return 1;
}

/* dom_string_data() is byte data, not a promise of a C terminator.  Copy a
 * short tag-name preview before handing it to the %s-only debug formatter. */
static const char *macsurf_dom_string_preview(dom_string *str, char *out,
		int cap)
{
	const char *data;
	size_t len;
	size_t copy;

	if (out == NULL || cap <= 0) return "-";
	out[0] = '\0';
	if (str == NULL) return "-";
	data = dom_string_data(str);
	len = dom_string_byte_length(str);
	if (data == NULL) return "-";
	copy = len;
	if (copy > (size_t)(cap - 1)) copy = (size_t)(cap - 1);
	memcpy(out, data, copy);
	out[copy] = '\0';
	return out;
}

static void macsurf_dom_trace_make_element(const char *op,
		dom_document *doc, const char *tag, const char *ns,
		dom_exception exc, dom_element **element)
{
	dom_node *node = NULL;

	if (!macsurf_dom_make_trace_take()) return;
	if (exc == DOM_NO_ERR && element != NULL) node = (dom_node *)*element;
	if (ns != NULL) {
		macsurf_debug_log_writef(
			"LIFE DOMMAKE %s doc=%p tag=%s ns=%s exc=%d node=%p",
			op, (void *)doc, tag != NULL ? tag : "-", ns,
			(int)exc, (void *)node);
	} else {
		macsurf_debug_log_writef(
			"LIFE DOMMAKE %s doc=%p tag=%s exc=%d node=%p",
			op, (void *)doc, tag != NULL ? tag : "-", (int)exc,
			(void *)node);
	}
}

static void macsurf_dom_trace_make_text(dom_document *doc, long len,
		dom_exception exc, dom_text **text)
{
	dom_node *node = NULL;

	if (!macsurf_dom_make_trace_take()) return;
	if (exc == DOM_NO_ERR && text != NULL) node = (dom_node *)*text;
	macsurf_debug_log_writef(
		"LIFE DOMMAKE text doc=%p len=%ld exc=%d node=%p",
		(void *)doc, len, (int)exc, (void *)node);
}

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

/* fixes382 (M1) - JS->DOM->render: the JS bridge needs the real <html>
 * root to expose document.documentElement/body/head as wrapped elements. */
dom_exception macsurf_dom_document_get_document_element(dom_document *doc,
    dom_element **result)
{
    return dom_document_get_document_element(doc, result);
}

dom_exception macsurf_dom_document_create_element(dom_document *doc, 
    dom_string *tag_name, dom_element **element)
{
    char tag[48];
    dom_exception exc = dom_document_create_element(doc, tag_name, element);
    macsurf_dom_trace_make_element("elem", doc,
            macsurf_dom_string_preview(tag_name, tag, (int)sizeof tag), NULL,
            exc, element);
    return exc;
}

dom_exception macsurf_dom_document_create_text_node(dom_document *doc,
    dom_string *data, dom_text **text)
{
    dom_exception exc = dom_document_create_text_node(doc, data, text);
    macsurf_dom_trace_make_text(doc,
            data != NULL ? (long)dom_string_byte_length(data) : -1,
            exc, text);
    return exc;
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
    char mount_id[80];
    int before = -1, after = -1;
    int before_capped = 0, after_capped = 0;
    int trace;
    dom_exception exc;

    macsurf_dom_dispatch_sentinel("appendChild", &s_doms_append, parent,
            new_child);
    trace = macsurf_dom_mutation_trace_begin(parent, mount_id,
            (int)sizeof mount_id);
    if (trace)
        before = macsurf_dom_native_child_count(parent, &before_capped);
    exc = dom_node_append_child(parent, new_child, result);
    if (trace) {
        after = macsurf_dom_native_child_count(parent, &after_capped);
        macsurf_dom_trace_mutation("append", parent, new_child, NULL, result,
                exc, before, after, before_capped, after_capped, mount_id);
    }
    return exc;
}

dom_exception macsurf_dom_node_remove_child(dom_node *parent,
    dom_node *old_child, dom_node **result)
{
    char mount_id[80];
    int before = -1, after = -1;
    int before_capped = 0, after_capped = 0;
    int trace;
    dom_exception exc;

    macsurf_dom_dispatch_sentinel("removeChild", &s_doms_remove, parent,
            old_child);
    trace = macsurf_dom_mutation_trace_begin(parent, mount_id,
            (int)sizeof mount_id);
    if (trace)
        before = macsurf_dom_native_child_count(parent, &before_capped);
    exc = dom_node_remove_child(parent, old_child, result);
    if (trace) {
        after = macsurf_dom_native_child_count(parent, &after_capped);
        macsurf_dom_trace_mutation("remove", parent, old_child, NULL, result,
                exc, before, after, before_capped, after_capped, mount_id);
    }
    return exc;
}

/* fixes385 (M4) - ordered insertion (React reconciler inserts before a
 * reference node rather than append-only). */
dom_exception macsurf_dom_node_insert_before(dom_node *parent,
    dom_node *new_child, dom_node *ref_child, dom_node **result)
{
    char mount_id[80];
    int before = -1, after = -1;
    int before_capped = 0, after_capped = 0;
    int trace;
    dom_exception exc;

    macsurf_dom_dispatch_sentinel("insertBefore", &s_doms_insert, parent,
            new_child);
    trace = macsurf_dom_mutation_trace_begin(parent, mount_id,
            (int)sizeof mount_id);
    if (trace)
        before = macsurf_dom_native_child_count(parent, &before_capped);
    exc = dom_node_insert_before(parent, new_child, ref_child, result);
    if (trace) {
        after = macsurf_dom_native_child_count(parent, &after_capped);
        macsurf_dom_trace_mutation("insert", parent, new_child, ref_child,
                result, exc, before, after, before_capped, after_capped,
                mount_id);
    }
    return exc;
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
    macsurf_dom_dispatch_sentinel("nodeValue", &s_doms_nodevalue, node,
            value);
    return dom_node_set_node_value(node, value);
}

/* fixes319d - text-content accessors. Inline in dom/core/node.h; the
 * JS bridge needs an external symbol it can extern-link. */
dom_exception macsurf_dom_node_get_text_content(dom_node *node,
    dom_string **result)
{
    return dom_node_get_text_content(node, result);
}

dom_exception macsurf_dom_node_set_text_content(dom_node *node,
    dom_string *content)
{
    macsurf_dom_dispatch_sentinel("textContent", &s_doms_textcontent, node,
            content);
    return dom_node_set_text_content(node, content);
}

dom_exception macsurf_dom_node_get_parent_node(dom_node *node,
    dom_node **result)
{
    return dom_node_get_parent_node(node, result);
}

/* fixes867 (#293) - owner document, for the DOM-mutation failure diagnostic in
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
    if (tag == NULL || dom_string_create((const uint8_t *)tag,
            (unsigned)strlen(tag), &ds) != DOM_NO_ERR || ds == NULL) {
        macsurf_dom_trace_make_element("elem", doc, tag, NULL, 5, element);
        return 5; /* DOM_NO_MEMORY_ERR */
    }
    macsurf_dom_dispatch_sentinel("createElement", &s_doms_create_element,
            doc, ds);
    exc = dom_document_create_element(doc, ds, element);
    dom_string_unref(ds);
    macsurf_dom_trace_make_element("elem", doc, tag, NULL, exc, element);
    return exc;
}

/* fixes870 (#297) - document.createElementNS() from plain JS C strings.
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

    if (qname == NULL) {
        macsurf_dom_trace_make_element("elemNS", doc, NULL, ns, 5, element);
        return 5; /* DOM_NO_MEMORY_ERR */
    }
    if (dom_string_create((const uint8_t *)qname, (unsigned)strlen(qname),
                          &qn_s) != DOM_NO_ERR || qn_s == NULL) {
        macsurf_dom_trace_make_element("elemNS", doc, qname, ns, 5, element);
        return 5;
    }
    if (ns != NULL && ns[0] != '\0') {
        if (dom_string_create((const uint8_t *)ns, (unsigned)strlen(ns),
                              &ns_s) != DOM_NO_ERR) {
            dom_string_unref(qn_s);
            macsurf_dom_trace_make_element("elemNS", doc, qname, ns, 5,
                    element);
            return 5;
        }
    }
    macsurf_dom_dispatch_sentinel("createElementNS",
            &s_doms_create_element_ns, doc, qn_s);
    exc = dom_document_create_element_ns(doc, ns_s, qn_s, element);
    if (ns_s != NULL) dom_string_unref(ns_s);
    dom_string_unref(qn_s);
    macsurf_dom_trace_make_element("elemNS", doc, qname, ns, exc, element);
    return exc;
}

/* fixes846 (#167 S3) - document.createTextNode() from a plain JS C string,
 * mirroring create_element_s above. */
dom_exception macsurf_dom_document_create_text_node_s(dom_document *doc,
    const char *data, dom_text **text)
{
    dom_string *ds = NULL;
    dom_exception exc;
    if (data == NULL || dom_string_create((const uint8_t *)data,
            (unsigned)strlen(data), &ds) != DOM_NO_ERR || ds == NULL) {
        macsurf_dom_trace_make_text(doc,
                data != NULL ? (long)strlen(data) : -1, 5, text);
        return 5; /* DOM_NO_MEMORY_ERR */
    }
    macsurf_dom_dispatch_sentinel("createTextNode", &s_doms_create_text,
            doc, ds);
    exc = dom_document_create_text_node(doc, ds, text);
    dom_string_unref(ds);
    macsurf_dom_trace_make_text(doc, (long)strlen(data), exc, text);
    return exc;
}

/* A Comment is not an empty Text node.  React's hydration protocol uses
 * comment nodeType/name boundaries, so callers must receive libdom's actual
 * dom_comment object and preserve its CharacterData semantics. */
dom_exception macsurf_dom_document_create_comment_s(dom_document *doc,
    const char *data, dom_comment **comment)
{
    dom_string *ds = NULL;
    dom_exception exc;

    if (data == NULL) data = "";
    dom_string_create((const uint8_t *)data, (unsigned)strlen(data), &ds);
    if (ds == NULL) return 5; /* DOM_NO_MEMORY_ERR */
    macsurf_dom_dispatch_sentinel("createComment", &s_doms_create_comment,
            doc, ds);
    exc = dom_document_create_comment(doc, ds, comment);
    dom_string_unref(ds);
    return exc;
}

/* fixes846 (#167 S3) - document.createDocumentFragment(). */
dom_exception macsurf_dom_document_create_document_fragment(dom_document *doc,
    dom_document_fragment **fragment)
{
    return dom_document_create_document_fragment(doc, fragment);
}

/* fixes846 (#167 S3) - text/comment node data accessors (dom_characterdata
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
    macsurf_dom_dispatch_sentinel("nodeValue", &s_doms_nodevalue, node,
            NULL);
    dom_string_create((const uint8_t *)data, (unsigned)strlen(data), &ds);
    if (ds == NULL) return 5; /* DOM_NO_MEMORY_ERR */
    exc = dom_characterdata_set_data((dom_characterdata *) node, ds);
    dom_string_unref(ds);
    return exc;
}

/* fixes878 - real cloneNode. The JS binding previously handed back the element
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

/* fixes1168 (#262) - attribute enumeration for the innerHTML serializer.
 * dom_node_get_attributes / dom_attr_get_name / dom_attr_get_value are
 * static-inline vtable dispatchers in the libdom headers; CW8 cannot link
 * them from other TUs (same reason every other accessor here is wrapped).
 * The namednodemap itself (get_length/item/unref) is a real exported
 * function in namednodemap.c and is called directly by the caller. */
dom_exception macsurf_dom_node_get_attributes(dom_node *node,
    dom_namednodemap **result)
{
    return dom_node_get_attributes(node, result);
}

dom_exception macsurf_dom_attr_get_name(dom_node *attr,
    dom_string **name)
{
    return dom_attr_get_name((dom_attr *) attr, name);
}

dom_exception macsurf_dom_attr_get_value(dom_node *attr,
    dom_string **value)
{
    return dom_attr_get_value((dom_attr *) attr, value);
}

/* fixes878 - node.contains(). Non-virtual in libdom (see the comment at
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
