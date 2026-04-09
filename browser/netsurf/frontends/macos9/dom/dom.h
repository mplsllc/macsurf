/*
 * MacSurf monolithic libdom stub header
 * C89-compatible, CodeWarrior 8 / GCC PPC Mac OS 9
 * No inline, no // comments, no _Bool
 */

#ifndef MACOS9_DOM_DOM_H
#define MACOS9_DOM_DOM_H

#include <stddef.h>
#include <stdarg.h>

/* =========================================================
 * 1. Bool compat
 * ========================================================= */
/* true/false come from MacTypes.h as enum constants */
#ifndef bool
typedef unsigned char bool;
#endif

/* =========================================================
 * Forward-declare lwc_string_s for dom_string_intern
 * ========================================================= */
struct lwc_string_s;

/* =========================================================
 * 2. Integer types
 * ========================================================= */
typedef short          dom_short;
typedef unsigned short dom_ushort;
typedef long           dom_long;
typedef unsigned long  dom_ulong;

/* =========================================================
 * 3. dom_msg enum + type
 * ========================================================= */
enum {
    DOM_MSG_DEBUG     = 0,
    DOM_MSG_INFO      = 1,
    DOM_MSG_NOTICE    = 2,
    DOM_MSG_WARNING   = 3,
    DOM_MSG_ERROR     = 4,
    DOM_MSG_CRITICAL  = 5,
    DOM_MSG_ALERT     = 6,
    DOM_MSG_EMERGENCY = 7
};

typedef void (*dom_msg)(unsigned long severity, void *ctx, const char *msg, ...);

/* =========================================================
 * 4. Forward declarations of opaque types
 * ========================================================= */
typedef struct dom_node_internal      dom_node_internal;
typedef struct dom_element            dom_element;
typedef struct dom_attr               dom_attr;
typedef struct dom_document           dom_document;
typedef struct dom_document_type      dom_document_type;
typedef struct dom_characterdata      dom_characterdata;
typedef struct dom_text               dom_text;
typedef struct dom_comment            dom_comment;
typedef struct dom_cdata_section      dom_cdata_section;
typedef struct dom_processing_instruction dom_processing_instruction;
typedef struct dom_entity_reference   dom_entity_reference;
typedef struct dom_nodelist           dom_nodelist;
typedef struct dom_namednodemap       dom_namednodemap;
typedef struct dom_implementation     dom_implementation;
typedef struct dom_type_info          dom_type_info;
typedef struct dom_abstract_view      dom_abstract_view;
typedef struct dom_configuration      dom_configuration;
typedef struct dom_tokenlist          dom_tokenlist;
typedef struct dom_event              dom_event;
typedef struct dom_event_target       dom_event_target;
typedef struct dom_event_listener     dom_event_listener;
typedef struct dom_keyboard_event     dom_keyboard_event;
typedef struct dom_mouse_event        dom_mouse_event;
typedef struct dom_mouse_wheel_event  dom_mouse_wheel_event;
typedef struct dom_mouse_multi_wheel_event dom_mouse_multi_wheel_event;
typedef struct dom_ui_event           dom_ui_event;
typedef struct dom_text_event         dom_text_event;
typedef struct dom_custom_event       dom_custom_event;
typedef struct dom_mutation_event     dom_mutation_event;
typedef struct dom_mutation_name_event dom_mutation_name_event;
typedef struct dom_html_document      dom_html_document;
typedef struct dom_html_element       dom_html_element;
typedef struct dom_html_collection    dom_html_collection;
typedef struct dom_html_options_collection dom_html_options_collection;
typedef struct dom_html_anchor_element    dom_html_anchor_element;
typedef struct dom_html_applet_element    dom_html_applet_element;
typedef struct dom_html_area_element      dom_html_area_element;
typedef struct dom_html_base_element      dom_html_base_element;
typedef struct dom_html_base_font_element dom_html_base_font_element;
typedef struct dom_html_body_element      dom_html_body_element;
typedef struct dom_html_br_element        dom_html_br_element;
typedef struct dom_html_button_element    dom_html_button_element;
typedef struct dom_html_canvas_element    dom_html_canvas_element;
typedef struct dom_html_directory_element dom_html_directory_element;
typedef struct dom_html_div_element       dom_html_div_element;
typedef struct dom_html_dlist_element     dom_html_dlist_element;
typedef struct dom_html_field_set_element dom_html_field_set_element;
typedef struct dom_html_font_element      dom_html_font_element;
typedef struct dom_html_form_element      dom_html_form_element;
typedef struct dom_html_frame_element     dom_html_frame_element;
typedef struct dom_html_frame_set_element dom_html_frame_set_element;
typedef struct dom_html_head_element      dom_html_head_element;
typedef struct dom_html_heading_element   dom_html_heading_element;
typedef struct dom_html_hr_element        dom_html_hr_element;
typedef struct dom_html_html_element      dom_html_html_element;
typedef struct dom_html_iframe_element    dom_html_iframe_element;
typedef struct dom_html_image_element     dom_html_image_element;
typedef struct dom_html_input_element     dom_html_input_element;
typedef struct dom_html_isindex_element   dom_html_isindex_element;
typedef struct dom_html_label_element     dom_html_label_element;
typedef struct dom_html_legend_element    dom_html_legend_element;
typedef struct dom_html_li_element        dom_html_li_element;
typedef struct dom_html_link_element      dom_html_link_element;
typedef struct dom_html_map_element       dom_html_map_element;
typedef struct dom_html_menu_element      dom_html_menu_element;
typedef struct dom_html_meta_element      dom_html_meta_element;
typedef struct dom_html_mod_element       dom_html_mod_element;
typedef struct dom_html_object_element    dom_html_object_element;
typedef struct dom_html_olist_element     dom_html_olist_element;
typedef struct dom_html_opt_group_element dom_html_opt_group_element;
typedef struct dom_html_option_element    dom_html_option_element;
typedef struct dom_html_paragraph_element dom_html_paragraph_element;
typedef struct dom_html_param_element     dom_html_param_element;
typedef struct dom_html_pre_element       dom_html_pre_element;
typedef struct dom_html_quote_element     dom_html_quote_element;
typedef struct dom_html_script_element    dom_html_script_element;
typedef struct dom_html_select_element    dom_html_select_element;
typedef struct dom_html_style_element     dom_html_style_element;
typedef struct dom_html_table_caption_element  dom_html_table_caption_element;
typedef struct dom_html_table_cell_element     dom_html_table_cell_element;
typedef struct dom_html_table_col_element      dom_html_table_col_element;
typedef struct dom_html_table_element          dom_html_table_element;
typedef struct dom_html_table_row_element      dom_html_table_row_element;
typedef struct dom_html_table_section_element  dom_html_table_section_element;
typedef struct dom_html_text_area_element      dom_html_text_area_element;
typedef struct dom_html_title_element          dom_html_title_element;
typedef struct dom_html_u_list_element         dom_html_u_list_element;
typedef struct dom_hubbub_parser               dom_hubbub_parser;
typedef struct dom_document_fragment           dom_document_fragment;

/* =========================================================
 * 5. Exception enums
 * ========================================================= */
typedef enum {
    DOM_EXCEPTION_CLASS_NORMAL   = 0,
    DOM_EXCEPTION_CLASS_EVENT    = (1 << 16),
    DOM_EXCEPTION_CLASS_INTERNAL = (1 << 17)
} dom_exception_class;

typedef enum {
    DOM_NO_ERR                    = 0,
    DOM_INDEX_SIZE_ERR            = 1,
    DOM_DOMSTRING_SIZE_ERR        = 2,
    DOM_HIERARCHY_REQUEST_ERR     = 3,
    DOM_WRONG_DOCUMENT_ERR        = 4,
    DOM_INVALID_CHARACTER_ERR     = 5,
    DOM_NO_DATA_ALLOWED_ERR       = 6,
    DOM_NO_MODIFICATION_ALLOWED_ERR = 7,
    DOM_NOT_FOUND_ERR             = 8,
    DOM_NOT_SUPPORTED_ERR         = 9,
    DOM_INUSE_ATTRIBUTE_ERR       = 10,
    DOM_INVALID_STATE_ERR         = 11,
    DOM_SYNTAX_ERR                = 12,
    DOM_INVALID_MODIFICATION_ERR  = 13,
    DOM_NAMESPACE_ERR             = 14,
    DOM_INVALID_ACCESS_ERR        = 15,
    DOM_VALIDATION_ERR            = 16,
    DOM_TYPE_MISMATCH_ERR         = 17,
    DOM_UNSPECIFIED_EVENT_TYPE_ERR = (1 << 16),
    DOM_DISPATCH_REQUEST_ERR      = (1 << 16) + 1,
    DOM_NO_MEM_ERR                = (1 << 17),
    DOM_ATTR_WRONG_TYPE_ERR       = (1 << 17) + 1
} dom_exception;

/* =========================================================
 * 6. dom_string struct + API
 * ========================================================= */
struct dom_string {
    unsigned long refcnt;
};
typedef struct dom_string dom_string;

extern dom_exception dom_string_create(const unsigned char *ptr, size_t len, dom_string **str);
extern dom_exception dom_string_create_interned(const unsigned char *ptr, size_t len, dom_string **str);
extern void          dom_string_destroy(dom_string *str);
extern dom_exception dom_string_intern(dom_string *str, struct lwc_string_s **lwcstr);
extern bool          dom_string_isequal(const dom_string *s1, const dom_string *s2);
extern bool          dom_string_caseless_isequal(const dom_string *s1, const dom_string *s2);
extern bool          dom_string_lwc_isequal(const dom_string *str, struct lwc_string_s *lwcstr);
extern bool          dom_string_caseless_lwc_isequal(const dom_string *str, struct lwc_string_s *lwcstr);
extern const char   *dom_string_data(const dom_string *str);
extern size_t        dom_string_byte_length(const dom_string *str);
extern dom_exception dom_string_length(dom_string *str, unsigned long *len);
extern dom_exception dom_string_index(dom_string *str, unsigned long idx, unsigned long *ch);
extern dom_exception dom_string_rindex(dom_string *str, unsigned long idx, unsigned long *ch);
extern dom_exception dom_string_at(dom_string *str, unsigned long off, unsigned long *ch);
extern dom_exception dom_string_concat(dom_string *s1, dom_string *s2, dom_string **result);
extern dom_exception dom_string_substr(dom_string *str, unsigned long i1, unsigned long i2, dom_string **result);
extern dom_exception dom_string_insert(dom_string *target, dom_string *source, unsigned long offset, dom_string **result);
extern dom_exception dom_string_replace(dom_string *target, dom_string *source, unsigned long i1, unsigned long i2, dom_string **result);
extern dom_exception dom_string_toupper(dom_string *str, bool ascii_only, dom_string **result);
extern dom_exception dom_string_tolower(dom_string *str, bool ascii_only, dom_string **result);
extern unsigned int  dom_string_hash(dom_string *str);

static dom_string *dom_string_ref(dom_string *str)
{
    if (str != NULL) str->refcnt++;
    return str;
}

static void dom_string_unref(dom_string *str)
{
    if (str != NULL && --(str->refcnt) == 0) dom_string_destroy(str);
}

/* =========================================================
 * 7. Node type enums
 * ========================================================= */
typedef enum {
    DOM_ELEMENT_NODE                = 1,
    DOM_ATTRIBUTE_NODE              = 2,
    DOM_TEXT_NODE                   = 3,
    DOM_CDATA_SECTION_NODE          = 4,
    DOM_ENTITY_REFERENCE_NODE       = 5,
    DOM_ENTITY_NODE                 = 6,
    DOM_PROCESSING_INSTRUCTION_NODE = 7,
    DOM_COMMENT_NODE                = 8,
    DOM_DOCUMENT_NODE               = 9,
    DOM_DOCUMENT_TYPE_NODE          = 10,
    DOM_DOCUMENT_FRAGMENT_NODE      = 11,
    DOM_NOTATION_NODE               = 12,
    DOM_NODE_TYPE_COUNT
} dom_node_type;

typedef enum {
    DOM_NODE_CLONED   = 1,
    DOM_NODE_IMPORTED = 2,
    DOM_NODE_DELETED  = 3,
    DOM_NODE_RENAMED  = 4,
    DOM_NODE_ADOPTED  = 5
} dom_node_operation;

typedef enum {
    DOM_DOCUMENT_POSITION_DISCONNECTED       = 0x01,
    DOM_DOCUMENT_POSITION_PRECEDING          = 0x02,
    DOM_DOCUMENT_POSITION_FOLLOWING          = 0x04,
    DOM_DOCUMENT_POSITION_CONTAINS          = 0x08,
    DOM_DOCUMENT_POSITION_CONTAINED_BY      = 0x10,
    DOM_DOCUMENT_POSITION_IMPLEMENTATION_SPECIFIC = 0x20
} dom_document_position;

typedef enum {
    DOM_DOCUMENT_QUIRKS_MODE_NONE    = 0,
    DOM_DOCUMENT_QUIRKS_MODE_LIMITED = 1,
    DOM_DOCUMENT_QUIRKS_MODE_FULL    = 2
} dom_document_quirks_mode;

/* =========================================================
 * 8. dom_node struct + API
 * ========================================================= */
struct dom_node {
    const void    *vtable;
    unsigned long  refcnt;
};
typedef struct dom_node dom_node;

typedef void (*dom_user_data_handler)(dom_node_operation operation,
                                      dom_string *key,
                                      void *data,
                                      dom_node *src,
                                      dom_node *dst);

extern void dom_node_try_destroy(dom_node *node);
#define dom_node_try_destroy(n) (dom_node_try_destroy)((dom_node *)(n))

static dom_node *_dom_node_ref_fn(dom_node *node)
{
    if (node != NULL) node->refcnt++;
    return node;
}

static void _dom_node_unref_fn(dom_node *node)
{
    if (node != NULL && --(node->refcnt) == 0) dom_node_try_destroy(node);
}

#define dom_node_ref(n)   _dom_node_ref_fn((dom_node *)(n))
#define dom_node_unref(n) _dom_node_unref_fn((dom_node *)(n))

extern dom_exception dom_node_get_node_name(dom_node *node, dom_string **result);
extern dom_exception dom_node_get_node_value(dom_node *node, dom_string **result);
extern dom_exception dom_node_set_node_value(dom_node *node, dom_string *value);
extern dom_exception dom_node_get_node_type(dom_node *node, dom_node_type *result);
extern dom_exception dom_node_get_parent_node(dom_node *node, dom_node **result);
extern dom_exception dom_node_get_child_nodes(dom_node *node, dom_nodelist **result);
extern dom_exception dom_node_get_first_child(dom_node *node, dom_node **result);
extern dom_exception dom_node_get_last_child(dom_node *node, dom_node **result);
extern dom_exception dom_node_get_previous_sibling(dom_node *node, dom_node **result);
extern dom_exception dom_node_get_next_sibling(dom_node *node, dom_node **result);
extern dom_exception dom_node_get_attributes(dom_node *node, dom_namednodemap **result);
extern dom_exception dom_node_get_owner_document(dom_node *node, dom_document **result);
extern dom_exception dom_node_insert_before(dom_node *node, dom_node *new_node, dom_node *ref_node, dom_node **result);
extern dom_exception dom_node_replace_child(dom_node *node, dom_node *new_node, dom_node *old_node, dom_node **result);
extern dom_exception dom_node_remove_child(dom_node *node, dom_node *old_node, dom_node **result);
extern dom_exception dom_node_append_child(dom_node *node, dom_node *new_node, dom_node **result);
extern dom_exception dom_node_has_child_nodes(dom_node *node, bool *result);
extern dom_exception dom_node_clone_node(dom_node *node, bool deep, dom_node **result);
extern dom_exception dom_node_normalize(dom_node *node);
extern dom_exception dom_node_is_supported(dom_node *node, dom_string *feature, dom_string *version, bool *result);
extern dom_exception dom_node_get_namespace(dom_node *node, dom_string **result);
extern dom_exception dom_node_get_prefix(dom_node *node, dom_string **result);
extern dom_exception dom_node_set_prefix(dom_node *node, dom_string *prefix);
extern dom_exception dom_node_get_local_name(dom_node *node, dom_string **result);
extern dom_exception dom_node_has_attributes(dom_node *node, bool *result);
extern dom_exception dom_node_get_base(dom_node *node, dom_string **result);
extern dom_exception dom_node_compare_document_position(dom_node *node, dom_node *other, unsigned short *result);
extern dom_exception dom_node_get_text_content(dom_node *node, dom_string **result);
extern dom_exception dom_node_set_text_content(dom_node *node, dom_string *content);
extern dom_exception dom_node_is_same(dom_node *node, dom_node *other, bool *result);
extern dom_exception dom_node_lookup_prefix(dom_node *node, dom_string *ns, dom_string **result);
extern dom_exception dom_node_is_default_namespace(dom_node *node, dom_string *ns, bool *result);
extern dom_exception dom_node_lookup_namespace(dom_node *node, dom_string *prefix, dom_string **result);
extern dom_exception dom_node_is_equal(dom_node *node, dom_node *other, bool *result);
extern dom_exception dom_node_get_feature(dom_node *node, dom_string *feature, dom_string *version, void **result);
extern dom_exception dom_node_set_user_data(dom_node *node, dom_string *key, void *data, dom_user_data_handler handler, void **result);
extern dom_exception dom_node_get_user_data(dom_node *node, dom_string *key, void **result);
extern dom_exception _dom_node_contains(dom_node_internal *node, dom_node_internal *other, bool *contains);

#define dom_node_get_node_name(n, r)               (dom_node_get_node_name)((dom_node *)(n), (r))
#define dom_node_get_node_value(n, r)              (dom_node_get_node_value)((dom_node *)(n), (r))
#define dom_node_set_node_value(n, v)              (dom_node_set_node_value)((dom_node *)(n), (v))
#define dom_node_get_node_type(n, r)               (dom_node_get_node_type)((dom_node *)(n), (r))
#define dom_node_get_parent_node(n, r)             (dom_node_get_parent_node)((dom_node *)(n), (dom_node **)(r))
#define dom_node_get_child_nodes(n, r)             (dom_node_get_child_nodes)((dom_node *)(n), (r))
#define dom_node_get_first_child(n, r)             (dom_node_get_first_child)((dom_node *)(n), (dom_node **)(r))
#define dom_node_get_last_child(n, r)              (dom_node_get_last_child)((dom_node *)(n), (dom_node **)(r))
#define dom_node_get_previous_sibling(n, r)        (dom_node_get_previous_sibling)((dom_node *)(n), (dom_node **)(r))
#define dom_node_get_next_sibling(n, r)            (dom_node_get_next_sibling)((dom_node *)(n), (dom_node **)(r))
#define dom_node_get_attributes(n, r)              (dom_node_get_attributes)((dom_node *)(n), (r))
#define dom_node_get_owner_document(n, r)          (dom_node_get_owner_document)((dom_node *)(n), (r))
#define dom_node_insert_before(n, nn, ref, ret)    (dom_node_insert_before)((dom_node *)(n), (dom_node *)(nn), (dom_node *)(ref), (dom_node **)(ret))
#define dom_node_replace_child(n, nn, old, ret)    (dom_node_replace_child)((dom_node *)(n), (dom_node *)(nn), (dom_node *)(old), (dom_node **)(ret))
#define dom_node_remove_child(n, old, ret)         (dom_node_remove_child)((dom_node *)(n), (dom_node *)(old), (dom_node **)(ret))
#define dom_node_append_child(n, nn, ret)          (dom_node_append_child)((dom_node *)(n), (dom_node *)(nn), (dom_node **)(ret))
#define dom_node_has_child_nodes(n, r)             (dom_node_has_child_nodes)((dom_node *)(n), (r))
#define dom_node_clone_node(n, d, r)               (dom_node_clone_node)((dom_node *)(n), (d), (dom_node **)(r))
#define dom_node_normalize(n)                      (dom_node_normalize)((dom_node *)(n))
#define dom_node_is_supported(n, f, v, r)          (dom_node_is_supported)((dom_node *)(n), (f), (v), (r))
#define dom_node_get_namespace(n, r)               (dom_node_get_namespace)((dom_node *)(n), (r))
#define dom_node_get_prefix(n, r)                  (dom_node_get_prefix)((dom_node *)(n), (r))
#define dom_node_set_prefix(n, p)                  (dom_node_set_prefix)((dom_node *)(n), (p))
#define dom_node_get_local_name(n, r)              (dom_node_get_local_name)((dom_node *)(n), (r))
#define dom_node_has_attributes(n, r)              (dom_node_has_attributes)((dom_node *)(n), (r))
#define dom_node_get_base(n, r)                    (dom_node_get_base)((dom_node *)(n), (r))
#define dom_node_compare_document_position(n, o, r) (dom_node_compare_document_position)((dom_node *)(n), (dom_node *)(o), (r))
#define dom_node_get_text_content(n, r)            (dom_node_get_text_content)((dom_node *)(n), (r))
#define dom_node_set_text_content(n, c)            (dom_node_set_text_content)((dom_node *)(n), (c))
#define dom_node_is_same(n, o, r)                  (dom_node_is_same)((dom_node *)(n), (dom_node *)(o), (r))
#define dom_node_lookup_prefix(n, ns, r)           (dom_node_lookup_prefix)((dom_node *)(n), (ns), (r))
#define dom_node_is_default_namespace(n, ns, r)    (dom_node_is_default_namespace)((dom_node *)(n), (ns), (r))
#define dom_node_lookup_namespace(n, p, r)         (dom_node_lookup_namespace)((dom_node *)(n), (p), (r))
#define dom_node_is_equal(n, o, r)                 (dom_node_is_equal)((dom_node *)(n), (dom_node *)(o), (r))
#define dom_node_get_feature(n, f, v, r)           (dom_node_get_feature)((dom_node *)(n), (f), (v), (r))
#define dom_node_set_user_data(n, k, d, h, r)      (dom_node_set_user_data)((dom_node *)(n), (k), (d), (h), (r))
#define dom_node_get_user_data(n, k, r)            (dom_node_get_user_data)((dom_node *)(n), (k), (r))
#define dom_node_contains(n, o, c)                 _dom_node_contains((dom_node_internal *)(n), (dom_node_internal *)(o), (c))

/* =========================================================
 * 9. dom_element API
 * ========================================================= */
extern dom_exception dom_element_get_tag_name(dom_element *ele, dom_string **result);
extern dom_exception dom_element_get_attribute(dom_element *ele, dom_string *name, dom_string **value);
extern dom_exception dom_element_set_attribute(dom_element *ele, dom_string *name, dom_string *value);
extern dom_exception dom_element_remove_attribute(dom_element *ele, dom_string *name);
extern dom_exception dom_element_get_attribute_node(dom_element *ele, dom_string *name, dom_attr **result);
extern dom_exception dom_element_set_attribute_node(dom_element *ele, dom_attr *attr, dom_attr **result);
extern dom_exception dom_element_remove_attribute_node(dom_element *ele, dom_attr *attr, dom_attr **result);
extern dom_exception dom_element_get_attribute_ns(dom_element *ele, dom_string *ns, dom_string *name, dom_string **value);
extern dom_exception dom_element_set_attribute_ns(dom_element *ele, dom_string *ns, dom_string *qname, dom_string *value);
extern dom_exception dom_element_remove_attribute_ns(dom_element *ele, dom_string *ns, dom_string *name);
extern dom_exception dom_element_get_attribute_node_ns(dom_element *ele, dom_string *ns, dom_string *name, dom_attr **result);
extern dom_exception dom_element_set_attribute_node_ns(dom_element *ele, dom_attr *attr, dom_attr **result);
extern dom_exception dom_element_get_elements_by_tag_name(dom_element *ele, dom_string *name, dom_nodelist **result);
extern dom_exception dom_element_get_elements_by_tag_name_ns(dom_element *ele, dom_string *ns, dom_string *name, dom_nodelist **result);
extern dom_exception dom_element_has_attribute(dom_element *ele, dom_string *name, bool *result);
extern dom_exception dom_element_has_attribute_ns(dom_element *ele, dom_string *ns, dom_string *name, bool *result);
extern dom_exception dom_element_get_schema_type_info(dom_element *ele, dom_type_info **result);
extern dom_exception dom_element_set_id_attribute(dom_element *ele, dom_string *name, bool is_id);
extern dom_exception dom_element_set_id_attribute_ns(dom_element *ele, dom_string *ns, dom_string *name, bool is_id);
extern dom_exception dom_element_set_id_attribute_node(dom_element *ele, dom_attr *attr, bool is_id);
extern dom_exception dom_element_get_classes(dom_element *ele, dom_string ***classes, unsigned int *n_classes);
extern dom_exception dom_element_has_class(dom_element *ele, dom_string *name, bool *match);

#define dom_element_get_tag_name(e, r)               (dom_element_get_tag_name)((dom_element *)(e), (r))
#define dom_element_get_attribute(e, n, v)           (dom_element_get_attribute)((dom_element *)(e), (n), (v))
#define dom_element_set_attribute(e, n, v)           (dom_element_set_attribute)((dom_element *)(e), (n), (v))
#define dom_element_remove_attribute(e, n)           (dom_element_remove_attribute)((dom_element *)(e), (n))
#define dom_element_get_attribute_node(e, n, r)      (dom_element_get_attribute_node)((dom_element *)(e), (n), (r))
#define dom_element_set_attribute_node(e, a, r)      (dom_element_set_attribute_node)((dom_element *)(e), (a), (r))
#define dom_element_remove_attribute_node(e, a, r)   (dom_element_remove_attribute_node)((dom_element *)(e), (a), (r))
#define dom_element_get_attribute_ns(e, ns, n, v)    (dom_element_get_attribute_ns)((dom_element *)(e), (ns), (n), (v))
#define dom_element_set_attribute_ns(e, ns, q, v)    (dom_element_set_attribute_ns)((dom_element *)(e), (ns), (q), (v))
#define dom_element_remove_attribute_ns(e, ns, n)    (dom_element_remove_attribute_ns)((dom_element *)(e), (ns), (n))
#define dom_element_get_attribute_node_ns(e, ns, n, r) (dom_element_get_attribute_node_ns)((dom_element *)(e), (ns), (n), (r))
#define dom_element_set_attribute_node_ns(e, a, r)  (dom_element_set_attribute_node_ns)((dom_element *)(e), (a), (r))
#define dom_element_get_elements_by_tag_name(e, n, r) (dom_element_get_elements_by_tag_name)((dom_element *)(e), (n), (r))
#define dom_element_get_elements_by_tag_name_ns(e, ns, n, r) (dom_element_get_elements_by_tag_name_ns)((dom_element *)(e), (ns), (n), (r))
#define dom_element_has_attribute(e, n, r)           (dom_element_has_attribute)((dom_element *)(e), (n), (r))
#define dom_element_has_attribute_ns(e, ns, n, r)    (dom_element_has_attribute_ns)((dom_element *)(e), (ns), (n), (r))
#define dom_element_get_schema_type_info(e, r)       (dom_element_get_schema_type_info)((dom_element *)(e), (r))
#define dom_element_set_id_attribute(e, n, i)        (dom_element_set_id_attribute)((dom_element *)(e), (n), (i))
#define dom_element_set_id_attribute_ns(e, ns, n, i) (dom_element_set_id_attribute_ns)((dom_element *)(e), (ns), (n), (i))
#define dom_element_set_id_attribute_node(e, a, i)   (dom_element_set_id_attribute_node)((dom_element *)(e), (a), (i))
#define dom_element_get_classes(e, c, n)             (dom_element_get_classes)((dom_element *)(e), (c), (n))
#define dom_element_has_class(e, n, m)               (dom_element_has_class)((dom_element *)(e), (n), (m))

/* =========================================================
 * 10. dom_attr API
 * ========================================================= */
extern dom_exception dom_attr_get_name(dom_attr *attr, dom_string **result);
extern dom_exception dom_attr_get_specified(dom_attr *attr, bool *result);
extern dom_exception dom_attr_get_value(dom_attr *attr, dom_string **result);
extern dom_exception dom_attr_set_value(dom_attr *attr, dom_string *value);
extern dom_exception dom_attr_get_owner_element(dom_attr *attr, dom_element **result);
extern dom_exception dom_attr_is_id(dom_attr *attr, bool *result);
extern dom_exception dom_attr_get_schema_type_info(dom_attr *attr, dom_type_info **result);
extern dom_exception dom_attr_get_integer(dom_attr *attr, unsigned int *value);
extern dom_exception dom_attr_set_integer(dom_attr *attr, unsigned int value);
extern dom_exception dom_attr_get_short(dom_attr *attr, unsigned short *value);
extern dom_exception dom_attr_set_short(dom_attr *attr, unsigned short value);
extern dom_exception dom_attr_get_bool(dom_attr *attr, bool *value);
extern dom_exception dom_attr_set_bool(dom_attr *attr, bool value);
extern dom_exception dom_attr_mark_readonly(dom_attr *attr, bool ro);

/* =========================================================
 * 11. dom_document API
 * ========================================================= */
extern dom_exception dom_document_get_doctype(dom_document *doc, dom_document_type **result);
extern dom_exception dom_document_get_implementation(dom_document *doc, dom_implementation **result);
extern dom_exception dom_document_get_document_element(dom_document *doc, dom_element **result);
extern dom_exception dom_document_create_element(dom_document *doc, dom_string *tag_name, dom_element **result);
extern dom_exception dom_document_create_element_ns(dom_document *doc, dom_string *ns, dom_string *qname, dom_element **result);
extern dom_exception dom_document_create_document_fragment(dom_document *doc, dom_document_fragment **result);
extern dom_exception dom_document_create_text_node(dom_document *doc, dom_string *data, dom_text **result);
extern dom_exception dom_document_create_comment(dom_document *doc, dom_string *data, dom_comment **result);
extern dom_exception dom_document_create_cdata_section(dom_document *doc, dom_string *data, dom_cdata_section **result);
extern dom_exception dom_document_create_processing_instruction(dom_document *doc, dom_string *target, dom_string *data, dom_processing_instruction **result);
extern dom_exception dom_document_create_attribute(dom_document *doc, dom_string *name, dom_attr **result);
extern dom_exception dom_document_create_attribute_ns(dom_document *doc, dom_string *ns, dom_string *qname, dom_attr **result);
extern dom_exception dom_document_create_entity_reference(dom_document *doc, dom_string *name, dom_entity_reference **result);
extern dom_exception dom_document_get_elements_by_tag_name(dom_document *doc, dom_string *tag_name, dom_nodelist **result);
extern dom_exception dom_document_get_elements_by_tag_name_ns(dom_document *doc, dom_string *ns, dom_string *name, dom_nodelist **result);
extern dom_exception dom_document_get_element_by_id(dom_document *doc, dom_string *id, dom_element **result);
extern dom_exception dom_document_import_node(dom_document *doc, dom_node *node, bool deep, dom_node **result);
extern dom_exception dom_document_adopt_node(dom_document *doc, dom_node *node, dom_node **result);
extern dom_exception dom_document_normalize(dom_document *doc);
extern dom_exception dom_document_rename_node(dom_document *doc, dom_node *node, dom_string *ns, dom_string *qname, dom_node **result);
extern dom_exception dom_document_get_dom_config(dom_document *doc, dom_configuration **result);
extern dom_exception dom_document_get_xml_encoding(dom_document *doc, dom_string **result);
extern dom_exception dom_document_get_xml_standalone(dom_document *doc, bool *result);
extern dom_exception dom_document_set_xml_standalone(dom_document *doc, bool standalone);
extern dom_exception dom_document_get_xml_version(dom_document *doc, dom_string **result);
extern dom_exception dom_document_set_xml_version(dom_document *doc, dom_string *version);
extern dom_exception dom_document_get_strict_error_checking(dom_document *doc, bool *result);
extern dom_exception dom_document_set_strict_error_checking(dom_document *doc, bool strict);
extern dom_exception dom_document_get_uri(dom_document *doc, dom_string **result);
extern dom_exception dom_document_set_uri(dom_document *doc, dom_string *uri);
extern dom_exception dom_document_get_input_encoding(dom_document *doc, dom_string **result);
extern dom_exception dom_document_get_quirks_mode(dom_document *doc, dom_document_quirks_mode *result);
extern dom_exception dom_document_set_quirks_mode(dom_document *doc, dom_document_quirks_mode mode);

#define dom_document_get_doctype(d, r)                  (dom_document_get_doctype)((dom_document *)(d), (r))
#define dom_document_get_implementation(d, r)           (dom_document_get_implementation)((dom_document *)(d), (r))
#define dom_document_get_document_element(d, r)         (dom_document_get_document_element)((dom_document *)(d), (r))
#define dom_document_create_element(d, t, r)            (dom_document_create_element)((dom_document *)(d), (t), (r))
#define dom_document_create_element_ns(d, ns, q, r)     (dom_document_create_element_ns)((dom_document *)(d), (ns), (q), (r))
#define dom_document_create_document_fragment(d, r)     (dom_document_create_document_fragment)((dom_document *)(d), (r))
#define dom_document_create_text_node(d, dt, r)         (dom_document_create_text_node)((dom_document *)(d), (dt), (r))
#define dom_document_create_comment(d, dt, r)           (dom_document_create_comment)((dom_document *)(d), (dt), (r))
#define dom_document_create_cdata_section(d, dt, r)     (dom_document_create_cdata_section)((dom_document *)(d), (dt), (r))
#define dom_document_create_processing_instruction(d, t, dt, r) (dom_document_create_processing_instruction)((dom_document *)(d), (t), (dt), (r))
#define dom_document_create_attribute(d, n, r)          (dom_document_create_attribute)((dom_document *)(d), (n), (r))
#define dom_document_create_attribute_ns(d, ns, q, r)   (dom_document_create_attribute_ns)((dom_document *)(d), (ns), (q), (r))
#define dom_document_create_entity_reference(d, n, r)   (dom_document_create_entity_reference)((dom_document *)(d), (n), (r))
#define dom_document_get_elements_by_tag_name(d, t, r)  (dom_document_get_elements_by_tag_name)((dom_document *)(d), (t), (r))
#define dom_document_get_elements_by_tag_name_ns(d, ns, n, r) (dom_document_get_elements_by_tag_name_ns)((dom_document *)(d), (ns), (n), (r))
#define dom_document_get_element_by_id(d, i, r)         (dom_document_get_element_by_id)((dom_document *)(d), (i), (r))
#define dom_document_import_node(d, n, dp, r)           (dom_document_import_node)((dom_document *)(d), (dom_node *)(n), (dp), (dom_node **)(r))
#define dom_document_adopt_node(d, n, r)                (dom_document_adopt_node)((dom_document *)(d), (dom_node *)(n), (dom_node **)(r))
#define dom_document_normalize(d)                       (dom_document_normalize)((dom_document *)(d))
#define dom_document_rename_node(d, n, ns, q, r)        (dom_document_rename_node)((dom_document *)(d), (dom_node *)(n), (ns), (q), (dom_node **)(r))
#define dom_document_get_dom_config(d, r)               (dom_document_get_dom_config)((dom_document *)(d), (r))
#define dom_document_get_xml_encoding(d, r)             (dom_document_get_xml_encoding)((dom_document *)(d), (r))
#define dom_document_get_xml_standalone(d, r)           (dom_document_get_xml_standalone)((dom_document *)(d), (r))
#define dom_document_set_xml_standalone(d, s)           (dom_document_set_xml_standalone)((dom_document *)(d), (s))
#define dom_document_get_xml_version(d, r)              (dom_document_get_xml_version)((dom_document *)(d), (r))
#define dom_document_set_xml_version(d, v)              (dom_document_set_xml_version)((dom_document *)(d), (v))
#define dom_document_get_strict_error_checking(d, r)    (dom_document_get_strict_error_checking)((dom_document *)(d), (r))
#define dom_document_set_strict_error_checking(d, s)    (dom_document_set_strict_error_checking)((dom_document *)(d), (s))
#define dom_document_get_uri(d, r)                      (dom_document_get_uri)((dom_document *)(d), (r))
#define dom_document_set_uri(d, u)                      (dom_document_set_uri)((dom_document *)(d), (u))
#define dom_document_get_input_encoding(d, r)           (dom_document_get_input_encoding)((dom_document *)(d), (r))
#define dom_document_get_quirks_mode(d, r)              (dom_document_get_quirks_mode)((dom_document *)(d), (r))
#define dom_document_set_quirks_mode(d, m)              (dom_document_set_quirks_mode)((dom_document *)(d), (m))

/* =========================================================
 * 12. dom_document_type API
 * ========================================================= */
extern dom_exception dom_document_type_get_name(dom_document_type *dt, dom_string **result);
extern dom_exception dom_document_type_get_entities(dom_document_type *dt, dom_namednodemap **result);
extern dom_exception dom_document_type_get_notations(dom_document_type *dt, dom_namednodemap **result);
extern dom_exception dom_document_type_get_public_id(dom_document_type *dt, dom_string **result);
extern dom_exception dom_document_type_get_system_id(dom_document_type *dt, dom_string **result);
extern dom_exception dom_document_type_get_internal_subset(dom_document_type *dt, dom_string **result);

/* =========================================================
 * 13. dom_characterdata API
 * ========================================================= */
extern dom_exception dom_characterdata_get_data(dom_characterdata *cdata, dom_string **result);
extern dom_exception dom_characterdata_set_data(dom_characterdata *cdata, dom_string *value);
extern dom_exception dom_characterdata_get_length(dom_characterdata *cdata, unsigned long *result);
extern dom_exception dom_characterdata_substring_data(dom_characterdata *cdata, unsigned long offset, unsigned long count, dom_string **result);
extern dom_exception dom_characterdata_append_data(dom_characterdata *cdata, dom_string *arg);
extern dom_exception dom_characterdata_insert_data(dom_characterdata *cdata, unsigned long offset, dom_string *arg);
extern dom_exception dom_characterdata_delete_data(dom_characterdata *cdata, unsigned long offset, unsigned long count);
extern dom_exception dom_characterdata_replace_data(dom_characterdata *cdata, unsigned long offset, unsigned long count, dom_string *arg);

/* =========================================================
 * 14. dom_text API
 * ========================================================= */
extern dom_exception dom_text_split_text(dom_text *text, unsigned long offset, dom_text **result);
extern dom_exception dom_text_get_is_element_content_whitespace(dom_text *text, bool *result);
extern dom_exception dom_text_get_whole_text(dom_text *text, dom_string **result);

/* =========================================================
 * 15. dom_nodelist API
 * ========================================================= */
extern void          dom_nodelist_ref(dom_nodelist *list);
extern void          dom_nodelist_unref(dom_nodelist *list);
extern dom_exception dom_nodelist_get_length(dom_nodelist *list, unsigned long *result);
extern dom_exception dom_nodelist_item(dom_nodelist *list, unsigned long index, dom_node **node);

/* =========================================================
 * 16. dom_namednodemap API
 * ========================================================= */
extern void          dom_namednodemap_ref(dom_namednodemap *map);
extern void          dom_namednodemap_unref(dom_namednodemap *map);
extern dom_exception dom_namednodemap_get_length(dom_namednodemap *map, unsigned long *result);
extern dom_exception dom_namednodemap_item(dom_namednodemap *map, unsigned long index, dom_node **result);
extern dom_exception dom_namednodemap_get_named_item(dom_namednodemap *map, dom_string *name, dom_node **result);
extern dom_exception dom_namednodemap_set_named_item(dom_namednodemap *map, dom_node *arg, dom_node **result);
extern dom_exception dom_namednodemap_remove_named_item(dom_namednodemap *map, dom_string *name, dom_node **result);
extern dom_exception dom_namednodemap_get_named_item_ns(dom_namednodemap *map, dom_string *ns, dom_string *name, dom_node **result);
extern dom_exception dom_namednodemap_set_named_item_ns(dom_namednodemap *map, dom_node *arg, dom_node **result);
extern dom_exception dom_namednodemap_remove_named_item_ns(dom_namednodemap *map, dom_string *ns, dom_string *name, dom_node **result);

/* =========================================================
 * 17. dom_tokenlist API
 * ========================================================= */
extern dom_exception dom_tokenlist_create(dom_element *ele, dom_string *attr, dom_tokenlist **list);
extern void          dom_tokenlist_ref(dom_tokenlist *list);
extern void          dom_tokenlist_unref(dom_tokenlist *list);
extern dom_exception dom_tokenlist_get_length(dom_tokenlist *list, unsigned long *result);
extern dom_exception dom_tokenlist_item(dom_tokenlist *list, unsigned long index, dom_string **result);
extern dom_exception dom_tokenlist_contains(dom_tokenlist *list, dom_string *token, bool *result);
extern dom_exception dom_tokenlist_add(dom_tokenlist *list, dom_string *token);
extern dom_exception dom_tokenlist_remove(dom_tokenlist *list, dom_string *token);
extern dom_exception dom_tokenlist_get_value(dom_tokenlist *list, dom_string **result);
extern dom_exception dom_tokenlist_set_value(dom_tokenlist *list, dom_string *value);

/* =========================================================
 * 18. dom_implementation API
 * ========================================================= */
extern dom_exception dom_implementation_has_feature(dom_implementation *impl, dom_string *feature, dom_string *version, bool *result);
extern dom_exception dom_implementation_create_document_type(dom_implementation *impl, dom_string *qname, dom_string *public_id, dom_string *system_id, dom_document_type **result);
extern dom_exception dom_implementation_create_document(dom_implementation *impl, dom_string *ns, dom_string *qname, dom_document_type *doctype, dom_document **result);
extern dom_exception dom_implementation_get_feature(dom_implementation *impl, dom_string *feature, dom_string *version, void **result);

/* =========================================================
 * 19. Event enums + APIs
 * ========================================================= */
typedef enum {
    DOM_CAPTURING_PHASE = 1,
    DOM_AT_TARGET       = 2,
    DOM_BUBBLING_PHASE  = 3
} dom_event_flow_phase;

typedef enum {
    DOM_KEY_LOCATION_STANDARD = 0,
    DOM_KEY_LOCATION_LEFT     = 1,
    DOM_KEY_LOCATION_RIGHT    = 2,
    DOM_KEY_LOCATION_NUMPAD   = 3
} dom_key_location;

typedef enum {
    DOM_MUTATION_MODIFICATION = 1,
    DOM_MUTATION_ADDITION     = 2,
    DOM_MUTATION_REMOVAL      = 3
} dom_mutation_type;

typedef enum {
    DOM_DEFAULT_ACTION_STARTED   = 0,
    DOM_DEFAULT_ACTION_PREVENTED = 1,
    DOM_DEFAULT_ACTION_END       = 2,
    DOM_DEFAULT_ACTION_FINISHED  = 3
} dom_default_action_phase;

typedef void (*dom_default_action_callback)(dom_event *evt, void *pw);
typedef dom_default_action_callback (*dom_events_default_action_fetcher)(dom_string *type, dom_default_action_phase phase, void **pw);

extern dom_exception _dom_event_create(dom_event **evt);
extern void          _dom_event_ref(dom_event *evt);
extern void          _dom_event_unref(dom_event *evt);
extern dom_exception dom_event_init(dom_event *evt, dom_string *type, bool bubbles, bool cancelable);
extern dom_exception dom_event_init_ns(dom_event *evt, dom_string *ns, dom_string *type, bool bubbles, bool cancelable);
extern dom_exception dom_event_get_type(dom_event *evt, dom_string **result);
extern dom_exception dom_event_get_target(dom_event *evt, dom_event_target **result);
extern dom_exception dom_event_get_current_target(dom_event *evt, dom_event_target **result);
extern dom_exception dom_event_get_event_phase(dom_event *evt, dom_event_flow_phase *result);
extern dom_exception dom_event_get_bubbles(dom_event *evt, bool *result);
extern dom_exception dom_event_get_cancelable(dom_event *evt, bool *result);
extern dom_exception dom_event_get_timestamp(dom_event *evt, unsigned long *result);
extern dom_exception dom_event_get_namespace(dom_event *evt, dom_string **result);
extern dom_exception dom_event_stop_propagation(dom_event *evt);
extern dom_exception dom_event_stop_immediate_propagation(dom_event *evt);
extern dom_exception dom_event_prevent_default(dom_event *evt);
extern dom_exception dom_event_is_default_prevented(dom_event *evt, bool *result);
extern dom_exception dom_event_get_is_trusted(dom_event *evt, bool *result);
extern dom_exception dom_event_set_is_trusted(dom_event *evt, bool trusted);
extern bool          dom_event_in_dispatch(dom_event *evt);
extern bool          dom_event_is_initialised(dom_event *evt);
extern bool          dom_event_is_custom(dom_event *evt);

#define dom_event_create(e)   _dom_event_create((dom_event **)(e))
#define dom_event_ref(n)      _dom_event_ref((dom_event *)(n))
#define dom_event_unref(n)    _dom_event_unref((dom_event *)(n))

extern dom_exception _dom_event_listener_create(dom_string *type, void (*handler)(dom_event *evt, void *pw), void *pw, bool capture, dom_event_listener **listener);
extern void dom_event_listener_ref(dom_event_listener *listener);
extern void dom_event_listener_unref(dom_event_listener *listener);

#define dom_event_listener_create(t, h, p, c, l) _dom_event_listener_create((t), (h), (p), (c), (l))

extern dom_exception dom_event_target_add_event_listener(dom_event_target *et, dom_string *type, dom_event_listener *listener, bool capture);
extern dom_exception dom_event_target_remove_event_listener(dom_event_target *et, dom_string *type, dom_event_listener *listener, bool capture);
extern dom_exception dom_event_target_dispatch_event(dom_event_target *et, dom_event *evt, bool *result);
extern dom_exception dom_event_target_add_event_listener_ns(dom_event_target *et, dom_string *ns, dom_string *type, dom_event_listener *listener, bool capture);
extern dom_exception dom_event_target_remove_event_listener_ns(dom_event_target *et, dom_string *ns, dom_string *type, dom_event_listener *listener, bool capture);

#define dom_event_target_add_event_listener(et, t, l, c)          (dom_event_target_add_event_listener)((dom_event_target *)(et), (t), (l), (c))
#define dom_event_target_remove_event_listener(et, t, l, c)       (dom_event_target_remove_event_listener)((dom_event_target *)(et), (t), (l), (c))
#define dom_event_target_dispatch_event(et, e, r)                  (dom_event_target_dispatch_event)((dom_event_target *)(et), (e), (r))
#define dom_event_target_add_event_listener_ns(et, ns, t, l, c)   (dom_event_target_add_event_listener_ns)((dom_event_target *)(et), (ns), (t), (l), (c))
#define dom_event_target_remove_event_listener_ns(et, ns, t, l, c) (dom_event_target_remove_event_listener_ns)((dom_event_target *)(et), (ns), (t), (l), (c))

extern dom_exception dom_keyboard_event_create(dom_keyboard_event **evt);
extern dom_exception dom_keyboard_event_init(dom_keyboard_event *evt, dom_string *type, bool bubbles, bool cancelable, dom_abstract_view *view, dom_string *key, dom_key_location location, dom_string *modifiers_list, bool repeat);
extern dom_exception dom_keyboard_event_init_ns(dom_keyboard_event *evt, dom_string *ns, dom_string *type, bool bubbles, bool cancelable, dom_abstract_view *view, dom_string *key, dom_key_location location, dom_string *modifiers_list, bool repeat);
extern dom_exception dom_keyboard_event_get_key(dom_keyboard_event *evt, dom_string **result);
extern dom_exception dom_keyboard_event_get_code(dom_keyboard_event *evt, dom_string **result);
extern dom_exception dom_keyboard_event_get_location(dom_keyboard_event *evt, dom_key_location *result);
extern dom_exception dom_keyboard_event_get_modifier_state(dom_keyboard_event *evt, dom_string *key, bool *result);
extern dom_exception dom_keyboard_event_get_ctrl_key(dom_keyboard_event *evt, bool *result);
extern dom_exception dom_keyboard_event_get_shift_key(dom_keyboard_event *evt, bool *result);
extern dom_exception dom_keyboard_event_get_alt_key(dom_keyboard_event *evt, bool *result);
extern dom_exception dom_keyboard_event_get_meta_key(dom_keyboard_event *evt, bool *result);

extern dom_exception dom_mouse_event_init(dom_mouse_event *evt, dom_string *type, bool bubbles, bool cancelable, dom_abstract_view *view, long detail, long screen_x, long screen_y, long client_x, long client_y, bool ctrl, bool alt, bool shift, bool meta, unsigned short button, dom_event_target *related);
extern dom_exception dom_mouse_event_init_ns(dom_mouse_event *evt, dom_string *ns, dom_string *type, bool bubbles, bool cancelable, dom_abstract_view *view, long detail, long screen_x, long screen_y, long client_x, long client_y, bool ctrl, bool alt, bool shift, bool meta, unsigned short button, dom_event_target *related);
extern dom_exception dom_mouse_event_get_screen_x(dom_mouse_event *evt, long *result);
extern dom_exception dom_mouse_event_get_screen_y(dom_mouse_event *evt, long *result);
extern dom_exception dom_mouse_event_get_client_x(dom_mouse_event *evt, long *result);
extern dom_exception dom_mouse_event_get_client_y(dom_mouse_event *evt, long *result);
extern dom_exception dom_mouse_event_get_ctrl_key(dom_mouse_event *evt, bool *result);
extern dom_exception dom_mouse_event_get_shift_key(dom_mouse_event *evt, bool *result);
extern dom_exception dom_mouse_event_get_alt_key(dom_mouse_event *evt, bool *result);
extern dom_exception dom_mouse_event_get_meta_key(dom_mouse_event *evt, bool *result);
extern dom_exception dom_mouse_event_get_button(dom_mouse_event *evt, unsigned short *result);
extern dom_exception dom_mouse_event_get_related_target(dom_mouse_event *evt, dom_event_target **result);
extern dom_exception dom_mouse_event_get_modifier_state(dom_mouse_event *evt, dom_string *key, bool *result);

extern dom_exception dom_mouse_wheel_event_init_ns(dom_mouse_wheel_event *evt, dom_string *ns, dom_string *type, bool bubbles, bool cancelable, dom_abstract_view *view, long detail, long screen_x, long screen_y, long client_x, long client_y, bool ctrl, bool alt, bool shift, bool meta, unsigned short button, dom_event_target *related, dom_string *modifiers, long wheel_delta);
extern dom_exception dom_mouse_wheel_event_get_wheel_delta(dom_mouse_wheel_event *evt, long *result);

extern dom_exception dom_mouse_multi_wheel_event_init_ns(dom_mouse_multi_wheel_event *evt, dom_string *ns, dom_string *type, bool bubbles, bool cancelable, dom_abstract_view *view, long detail, long screen_x, long screen_y, long client_x, long client_y, bool ctrl, bool alt, bool shift, bool meta, unsigned short button, dom_event_target *related, dom_string *modifiers, long wheel_delta_x, long wheel_delta_y, long wheel_delta_z);
extern dom_exception dom_mouse_multi_wheel_event_get_wheel_delta_x(dom_mouse_multi_wheel_event *evt, long *result);
extern dom_exception dom_mouse_multi_wheel_event_get_wheel_delta_y(dom_mouse_multi_wheel_event *evt, long *result);
extern dom_exception dom_mouse_multi_wheel_event_get_wheel_delta_z(dom_mouse_multi_wheel_event *evt, long *result);

extern dom_exception dom_ui_event_init(dom_ui_event *evt, dom_string *type, bool bubbles, bool cancelable, dom_abstract_view *view, long detail);
extern dom_exception dom_ui_event_init_ns(dom_ui_event *evt, dom_string *ns, dom_string *type, bool bubbles, bool cancelable, dom_abstract_view *view, long detail);
extern dom_exception dom_ui_event_get_view(dom_ui_event *evt, dom_abstract_view **result);
extern dom_exception dom_ui_event_get_detail(dom_ui_event *evt, long *result);

extern dom_exception dom_text_event_init(dom_text_event *evt, dom_string *type, bool bubbles, bool cancelable, dom_abstract_view *view, dom_string *data);
extern dom_exception dom_text_event_init_ns(dom_text_event *evt, dom_string *ns, dom_string *type, bool bubbles, bool cancelable, dom_abstract_view *view, dom_string *data);
extern dom_exception dom_text_event_get_data(dom_text_event *evt, dom_string **result);

extern dom_exception dom_custom_event_init_ns(dom_custom_event *evt, dom_string *ns, dom_string *type, bool bubbles, bool cancelable, void *detail);
extern dom_exception dom_custom_event_get_detail(dom_custom_event *evt, void **result);

extern dom_exception dom_mutation_event_init(dom_mutation_event *evt, dom_string *type, bool bubbles, bool cancelable, dom_node *related, dom_string *prev_value, dom_string *new_value, dom_string *attr_name, unsigned short attr_change);
extern dom_exception dom_mutation_event_init_ns(dom_mutation_event *evt, dom_string *ns, dom_string *type, bool bubbles, bool cancelable, dom_node *related, dom_string *prev_value, dom_string *new_value, dom_string *attr_name, unsigned short attr_change);
extern dom_exception dom_mutation_event_get_related_node(dom_mutation_event *evt, dom_node **result);
extern dom_exception dom_mutation_event_get_prev_value(dom_mutation_event *evt, dom_string **result);
extern dom_exception dom_mutation_event_get_new_value(dom_mutation_event *evt, dom_string **result);
extern dom_exception dom_mutation_event_get_attr_name(dom_mutation_event *evt, dom_string **result);
extern dom_exception dom_mutation_event_get_attr_change(dom_mutation_event *evt, dom_mutation_type *result);

extern dom_exception _dom_document_event_create_event(dom_document *de, dom_string *type, dom_event **evt);
#define dom_document_event_create_event(d, t, e) _dom_document_event_create_event((dom_document *)(d), (dom_string *)(t), (dom_event **)(e))

extern dom_exception _dom_document_event_can_dispatch(dom_document *de, dom_string *ns, dom_string *type, bool *can);
#define dom_document_event_can_dispatch(d, n, t, c) _dom_document_event_can_dispatch((dom_document *)(d), (dom_string *)(n), (dom_string *)(t), (bool *)(c))

/* =========================================================
 * 20. HTML element type enum
 * ========================================================= */
typedef enum dom_html_element_type {
    DOM_HTML_ELEMENT_TYPE__UNKNOWN = 0,
    DOM_HTML_ELEMENT_TYPE_A,
    DOM_HTML_ELEMENT_TYPE_ABBR,
    DOM_HTML_ELEMENT_TYPE_ACRONYM,
    DOM_HTML_ELEMENT_TYPE_ADDRESS,
    DOM_HTML_ELEMENT_TYPE_APPLET,
    DOM_HTML_ELEMENT_TYPE_AREA,
    DOM_HTML_ELEMENT_TYPE_ARTICLE,
    DOM_HTML_ELEMENT_TYPE_ASIDE,
    DOM_HTML_ELEMENT_TYPE_B,
    DOM_HTML_ELEMENT_TYPE_BASE,
    DOM_HTML_ELEMENT_TYPE_BASEFONT,
    DOM_HTML_ELEMENT_TYPE_BDI,
    DOM_HTML_ELEMENT_TYPE_BDO,
    DOM_HTML_ELEMENT_TYPE_BIG,
    DOM_HTML_ELEMENT_TYPE_BLOCKQUOTE,
    DOM_HTML_ELEMENT_TYPE_BODY,
    DOM_HTML_ELEMENT_TYPE_BR,
    DOM_HTML_ELEMENT_TYPE_BUTTON,
    DOM_HTML_ELEMENT_TYPE_CANVAS,
    DOM_HTML_ELEMENT_TYPE_CAPTION,
    DOM_HTML_ELEMENT_TYPE_CENTER,
    DOM_HTML_ELEMENT_TYPE_CITE,
    DOM_HTML_ELEMENT_TYPE_CODE,
    DOM_HTML_ELEMENT_TYPE_COL,
    DOM_HTML_ELEMENT_TYPE_COLGROUP,
    DOM_HTML_ELEMENT_TYPE_COMMAND,
    DOM_HTML_ELEMENT_TYPE_DATA,
    DOM_HTML_ELEMENT_TYPE_DATALIST,
    DOM_HTML_ELEMENT_TYPE_DD,
    DOM_HTML_ELEMENT_TYPE_DEL,
    DOM_HTML_ELEMENT_TYPE_DETAILS,
    DOM_HTML_ELEMENT_TYPE_DFN,
    DOM_HTML_ELEMENT_TYPE_DIALOG,
    DOM_HTML_ELEMENT_TYPE_DIR,
    DOM_HTML_ELEMENT_TYPE_DIV,
    DOM_HTML_ELEMENT_TYPE_DL,
    DOM_HTML_ELEMENT_TYPE_DT,
    DOM_HTML_ELEMENT_TYPE_EM,
    DOM_HTML_ELEMENT_TYPE_EMBED,
    DOM_HTML_ELEMENT_TYPE_FIELDSET,
    DOM_HTML_ELEMENT_TYPE_FIGCAPTION,
    DOM_HTML_ELEMENT_TYPE_FIGURE,
    DOM_HTML_ELEMENT_TYPE_FONT,
    DOM_HTML_ELEMENT_TYPE_FOOTER,
    DOM_HTML_ELEMENT_TYPE_FORM,
    DOM_HTML_ELEMENT_TYPE_FRAME,
    DOM_HTML_ELEMENT_TYPE_FRAMESET,
    DOM_HTML_ELEMENT_TYPE_H1,
    DOM_HTML_ELEMENT_TYPE_H2,
    DOM_HTML_ELEMENT_TYPE_H3,
    DOM_HTML_ELEMENT_TYPE_H4,
    DOM_HTML_ELEMENT_TYPE_H5,
    DOM_HTML_ELEMENT_TYPE_H6,
    DOM_HTML_ELEMENT_TYPE_HEAD,
    DOM_HTML_ELEMENT_TYPE_HEADER,
    DOM_HTML_ELEMENT_TYPE_HGROUP,
    DOM_HTML_ELEMENT_TYPE_HR,
    DOM_HTML_ELEMENT_TYPE_HTML,
    DOM_HTML_ELEMENT_TYPE_I,
    DOM_HTML_ELEMENT_TYPE_IFRAME,
    DOM_HTML_ELEMENT_TYPE_IMG,
    DOM_HTML_ELEMENT_TYPE_INPUT,
    DOM_HTML_ELEMENT_TYPE_INS,
    DOM_HTML_ELEMENT_TYPE_ISINDEX,
    DOM_HTML_ELEMENT_TYPE_KBD,
    DOM_HTML_ELEMENT_TYPE_KEYGEN,
    DOM_HTML_ELEMENT_TYPE_LABEL,
    DOM_HTML_ELEMENT_TYPE_LEGEND,
    DOM_HTML_ELEMENT_TYPE_LI,
    DOM_HTML_ELEMENT_TYPE_LINK,
    DOM_HTML_ELEMENT_TYPE_MAIN,
    DOM_HTML_ELEMENT_TYPE_MAP,
    DOM_HTML_ELEMENT_TYPE_MARK,
    DOM_HTML_ELEMENT_TYPE_MENU,
    DOM_HTML_ELEMENT_TYPE_META,
    DOM_HTML_ELEMENT_TYPE_METER,
    DOM_HTML_ELEMENT_TYPE_NAV,
    DOM_HTML_ELEMENT_TYPE_NOBR,
    DOM_HTML_ELEMENT_TYPE_NOEMBED,
    DOM_HTML_ELEMENT_TYPE_NOFRAMES,
    DOM_HTML_ELEMENT_TYPE_NOSCRIPT,
    DOM_HTML_ELEMENT_TYPE_OBJECT,
    DOM_HTML_ELEMENT_TYPE_OL,
    DOM_HTML_ELEMENT_TYPE_OPTGROUP,
    DOM_HTML_ELEMENT_TYPE_OPTION,
    DOM_HTML_ELEMENT_TYPE_OUTPUT,
    DOM_HTML_ELEMENT_TYPE_P,
    DOM_HTML_ELEMENT_TYPE_PARAM,
    DOM_HTML_ELEMENT_TYPE_PICTURE,
    DOM_HTML_ELEMENT_TYPE_PRE,
    DOM_HTML_ELEMENT_TYPE_PROGRESS,
    DOM_HTML_ELEMENT_TYPE_Q,
    DOM_HTML_ELEMENT_TYPE_RP,
    DOM_HTML_ELEMENT_TYPE_RT,
    DOM_HTML_ELEMENT_TYPE_RUBY,
    DOM_HTML_ELEMENT_TYPE_S,
    DOM_HTML_ELEMENT_TYPE_SAMP,
    DOM_HTML_ELEMENT_TYPE_SCRIPT,
    DOM_HTML_ELEMENT_TYPE_SECTION,
    DOM_HTML_ELEMENT_TYPE_SELECT,
    DOM_HTML_ELEMENT_TYPE_SMALL,
    DOM_HTML_ELEMENT_TYPE_SOURCE,
    DOM_HTML_ELEMENT_TYPE_SPAN,
    DOM_HTML_ELEMENT_TYPE_STRIKE,
    DOM_HTML_ELEMENT_TYPE_STRONG,
    DOM_HTML_ELEMENT_TYPE_STYLE,
    DOM_HTML_ELEMENT_TYPE_SUB,
    DOM_HTML_ELEMENT_TYPE_SUMMARY,
    DOM_HTML_ELEMENT_TYPE_SUP,
    DOM_HTML_ELEMENT_TYPE_TABLE,
    DOM_HTML_ELEMENT_TYPE_TBODY,
    DOM_HTML_ELEMENT_TYPE_TD,
    DOM_HTML_ELEMENT_TYPE_TEMPLATE,
    DOM_HTML_ELEMENT_TYPE_TEXTAREA,
    DOM_HTML_ELEMENT_TYPE_TFOOT,
    DOM_HTML_ELEMENT_TYPE_TH,
    DOM_HTML_ELEMENT_TYPE_THEAD,
    DOM_HTML_ELEMENT_TYPE_TIME,
    DOM_HTML_ELEMENT_TYPE_TITLE,
    DOM_HTML_ELEMENT_TYPE_TR,
    DOM_HTML_ELEMENT_TYPE_TRACK,
    DOM_HTML_ELEMENT_TYPE_TT,
    DOM_HTML_ELEMENT_TYPE_U,
    DOM_HTML_ELEMENT_TYPE_UL,
    DOM_HTML_ELEMENT_TYPE_VAR,
    DOM_HTML_ELEMENT_TYPE_VIDEO,
    DOM_HTML_ELEMENT_TYPE_WBR,
    DOM_HTML_ELEMENT_TYPE_COUNT
} dom_html_element_type;

/* =========================================================
 * 21. dom_html_element API
 * ========================================================= */
extern dom_exception dom_html_element_get_id(dom_html_element *ele, dom_string **result);
extern dom_exception dom_html_element_set_id(dom_html_element *ele, dom_string *id);
extern dom_exception dom_html_element_get_title(dom_html_element *ele, dom_string **result);
extern dom_exception dom_html_element_set_title(dom_html_element *ele, dom_string *title);
extern dom_exception dom_html_element_get_lang(dom_html_element *ele, dom_string **result);
extern dom_exception dom_html_element_set_lang(dom_html_element *ele, dom_string *lang);
extern dom_exception dom_html_element_get_dir(dom_html_element *ele, dom_string **result);
extern dom_exception dom_html_element_set_dir(dom_html_element *ele, dom_string *dir);
extern dom_exception dom_html_element_get_class_name(dom_html_element *ele, dom_string **result);
extern dom_exception dom_html_element_set_class_name(dom_html_element *ele, dom_string *class_name);
extern dom_exception dom_html_element_get_tag_type(dom_html_element *ele, dom_html_element_type *result);

#define dom_html_element_get_id(e, r)          (dom_html_element_get_id)((dom_html_element *)(e), (r))
#define dom_html_element_set_id(e, v)          (dom_html_element_set_id)((dom_html_element *)(e), (v))
#define dom_html_element_get_title(e, r)       (dom_html_element_get_title)((dom_html_element *)(e), (r))
#define dom_html_element_set_title(e, v)       (dom_html_element_set_title)((dom_html_element *)(e), (v))
#define dom_html_element_get_lang(e, r)        (dom_html_element_get_lang)((dom_html_element *)(e), (r))
#define dom_html_element_set_lang(e, v)        (dom_html_element_set_lang)((dom_html_element *)(e), (v))
#define dom_html_element_get_dir(e, r)         (dom_html_element_get_dir)((dom_html_element *)(e), (r))
#define dom_html_element_set_dir(e, v)         (dom_html_element_set_dir)((dom_html_element *)(e), (v))
#define dom_html_element_get_class_name(e, r)  (dom_html_element_get_class_name)((dom_html_element *)(e), (r))
#define dom_html_element_set_class_name(e, v)  (dom_html_element_set_class_name)((dom_html_element *)(e), (v))
#define dom_html_element_get_tag_type(e, r)    (dom_html_element_get_tag_type)((dom_html_element *)(e), (r))

/* =========================================================
 * 22. dom_html_document API
 * ========================================================= */
extern dom_exception dom_html_document_open(dom_html_document *doc);
extern dom_exception dom_html_document_close(dom_html_document *doc);
extern dom_exception dom_html_document_write(dom_html_document *doc, dom_string *text);
extern dom_exception dom_html_document_writeln(dom_html_document *doc, dom_string *text);
extern dom_exception dom_html_document_get_title(dom_html_document *doc, dom_string **result);
extern dom_exception dom_html_document_set_title(dom_html_document *doc, dom_string *title);
extern dom_exception dom_html_document_get_referrer(dom_html_document *doc, dom_string **result);
extern dom_exception dom_html_document_get_domain(dom_html_document *doc, dom_string **result);
extern dom_exception dom_html_document_get_url(dom_html_document *doc, dom_string **result);
extern dom_exception dom_html_document_get_body(dom_html_document *doc, dom_html_element **result);
extern dom_exception dom_html_document_set_body(dom_html_document *doc, dom_html_element *body);
extern dom_exception dom_html_document_get_images(dom_html_document *doc, dom_html_collection **result);
extern dom_exception dom_html_document_get_applets(dom_html_document *doc, dom_html_collection **result);
extern dom_exception dom_html_document_get_links(dom_html_document *doc, dom_html_collection **result);
extern dom_exception dom_html_document_get_forms(dom_html_document *doc, dom_html_collection **result);
extern dom_exception dom_html_document_get_anchors(dom_html_document *doc, dom_html_collection **result);
extern dom_exception dom_html_document_get_cookie(dom_html_document *doc, dom_string **result);
extern dom_exception dom_html_document_set_cookie(dom_html_document *doc, dom_string *cookie);
extern dom_exception dom_html_document_get_elements_by_name(dom_html_document *doc, dom_string *name, dom_nodelist **result);

#define dom_html_document_open(d)               (dom_html_document_open)((dom_html_document *)(d))
#define dom_html_document_close(d)              (dom_html_document_close)((dom_html_document *)(d))
#define dom_html_document_write(d, t)           (dom_html_document_write)((dom_html_document *)(d), (t))
#define dom_html_document_writeln(d, t)         (dom_html_document_writeln)((dom_html_document *)(d), (t))
#define dom_html_document_get_title(d, r)       (dom_html_document_get_title)((dom_html_document *)(d), (r))
#define dom_html_document_set_title(d, t)       (dom_html_document_set_title)((dom_html_document *)(d), (t))
#define dom_html_document_get_referrer(d, r)    (dom_html_document_get_referrer)((dom_html_document *)(d), (r))
#define dom_html_document_get_domain(d, r)      (dom_html_document_get_domain)((dom_html_document *)(d), (r))
#define dom_html_document_get_url(d, r)         (dom_html_document_get_url)((dom_html_document *)(d), (r))
#define dom_html_document_get_body(d, r)        (dom_html_document_get_body)((dom_html_document *)(d), (r))
#define dom_html_document_set_body(d, b)        (dom_html_document_set_body)((dom_html_document *)(d), (b))
#define dom_html_document_get_images(d, r)      (dom_html_document_get_images)((dom_html_document *)(d), (r))
#define dom_html_document_get_applets(d, r)     (dom_html_document_get_applets)((dom_html_document *)(d), (r))
#define dom_html_document_get_links(d, r)       (dom_html_document_get_links)((dom_html_document *)(d), (r))
#define dom_html_document_get_forms(d, r)       (dom_html_document_get_forms)((dom_html_document *)(d), (r))
#define dom_html_document_get_anchors(d, r)     (dom_html_document_get_anchors)((dom_html_document *)(d), (r))
#define dom_html_document_get_cookie(d, r)      (dom_html_document_get_cookie)((dom_html_document *)(d), (r))
#define dom_html_document_set_cookie(d, c)      (dom_html_document_set_cookie)((dom_html_document *)(d), (c))
#define dom_html_document_get_elements_by_name(d, n, r) (dom_html_document_get_elements_by_name)((dom_html_document *)(d), (n), (r))

/* =========================================================
 * 23. dom_html_collection API
 * ========================================================= */
extern void          dom_html_collection_ref(dom_html_collection *col);
extern void          dom_html_collection_unref(dom_html_collection *col);
extern dom_exception dom_html_collection_get_length(dom_html_collection *col, unsigned long *result);
extern dom_exception dom_html_collection_item(dom_html_collection *col, unsigned long index, dom_node **result);
extern dom_exception dom_html_collection_named_item(dom_html_collection *col, dom_string *name, dom_node **result);

/* =========================================================
 * 24. dom_html_options_collection API
 * ========================================================= */
extern void          dom_html_options_collection_ref(dom_html_options_collection *col);
extern void          dom_html_options_collection_unref(dom_html_options_collection *col);
extern dom_exception dom_html_options_collection_get_length(dom_html_options_collection *col, unsigned long *result);
extern dom_exception dom_html_options_collection_set_length(dom_html_options_collection *col, unsigned long length);
extern dom_exception dom_html_options_collection_item(dom_html_options_collection *col, unsigned long index, dom_node **result);
extern dom_exception dom_html_options_collection_named_item(dom_html_options_collection *col, dom_string *name, dom_node **result);

/* =========================================================
 * 25. Individual HTML element APIs
 * ========================================================= */

/* --- dom_html_input_element --- */
extern dom_exception dom_html_input_element_get_default_value(dom_html_input_element *ele, dom_string **result);
extern dom_exception dom_html_input_element_set_default_value(dom_html_input_element *ele, dom_string *value);
extern dom_exception dom_html_input_element_get_default_checked(dom_html_input_element *ele, bool *result);
extern dom_exception dom_html_input_element_set_default_checked(dom_html_input_element *ele, bool checked);
extern dom_exception dom_html_input_element_get_form(dom_html_input_element *ele, dom_html_form_element **result);
extern dom_exception dom_html_input_element_get_accept(dom_html_input_element *ele, dom_string **result);
extern dom_exception dom_html_input_element_set_accept(dom_html_input_element *ele, dom_string *accept);
extern dom_exception dom_html_input_element_get_access_key(dom_html_input_element *ele, dom_string **result);
extern dom_exception dom_html_input_element_set_access_key(dom_html_input_element *ele, dom_string *access_key);
extern dom_exception dom_html_input_element_get_align(dom_html_input_element *ele, dom_string **result);
extern dom_exception dom_html_input_element_set_align(dom_html_input_element *ele, dom_string *align);
extern dom_exception dom_html_input_element_get_alt(dom_html_input_element *ele, dom_string **result);
extern dom_exception dom_html_input_element_set_alt(dom_html_input_element *ele, dom_string *alt);
extern dom_exception dom_html_input_element_get_checked(dom_html_input_element *ele, bool *result);
extern dom_exception dom_html_input_element_set_checked(dom_html_input_element *ele, bool checked);
extern dom_exception dom_html_input_element_get_disabled(dom_html_input_element *ele, bool *result);
extern dom_exception dom_html_input_element_set_disabled(dom_html_input_element *ele, bool disabled);
extern dom_exception dom_html_input_element_get_max_length(dom_html_input_element *ele, long *result);
extern dom_exception dom_html_input_element_set_max_length(dom_html_input_element *ele, long max_length);
extern dom_exception dom_html_input_element_get_name(dom_html_input_element *ele, dom_string **result);
extern dom_exception dom_html_input_element_set_name(dom_html_input_element *ele, dom_string *name);
extern dom_exception dom_html_input_element_get_read_only(dom_html_input_element *ele, bool *result);
extern dom_exception dom_html_input_element_set_read_only(dom_html_input_element *ele, bool read_only);
extern dom_exception dom_html_input_element_get_size(dom_html_input_element *ele, dom_string **result);
extern dom_exception dom_html_input_element_set_size(dom_html_input_element *ele, dom_string *size);
extern dom_exception dom_html_input_element_get_src(dom_html_input_element *ele, dom_string **result);
extern dom_exception dom_html_input_element_set_src(dom_html_input_element *ele, dom_string *src);
extern dom_exception dom_html_input_element_get_tab_index(dom_html_input_element *ele, long *result);
extern dom_exception dom_html_input_element_set_tab_index(dom_html_input_element *ele, long tab_index);
extern dom_exception dom_html_input_element_get_type(dom_html_input_element *ele, dom_string **result);
extern dom_exception dom_html_input_element_set_type(dom_html_input_element *ele, dom_string *type);
extern dom_exception dom_html_input_element_get_use_map(dom_html_input_element *ele, dom_string **result);
extern dom_exception dom_html_input_element_set_use_map(dom_html_input_element *ele, dom_string *use_map);
extern dom_exception dom_html_input_element_get_value(dom_html_input_element *ele, dom_string **result);
extern dom_exception dom_html_input_element_set_value(dom_html_input_element *ele, dom_string *value);
extern dom_exception dom_html_input_element_blur(dom_html_input_element *ele);
extern dom_exception dom_html_input_element_click(dom_html_input_element *ele);
extern dom_exception dom_html_input_element_focus(dom_html_input_element *ele);
extern dom_exception dom_html_input_element_select(dom_html_input_element *ele);

/* --- dom_html_select_element --- */
extern dom_exception dom_html_select_element_get_type(dom_html_select_element *ele, dom_string **result);
extern dom_exception dom_html_select_element_get_selected_index(dom_html_select_element *ele, long *result);
extern dom_exception dom_html_select_element_set_selected_index(dom_html_select_element *ele, long index);
extern dom_exception dom_html_select_element_get_value(dom_html_select_element *ele, dom_string **result);
extern dom_exception dom_html_select_element_set_value(dom_html_select_element *ele, dom_string *value);
extern dom_exception dom_html_select_element_get_length(dom_html_select_element *ele, unsigned long *result);
extern dom_exception dom_html_select_element_get_form(dom_html_select_element *ele, dom_html_form_element **result);
extern dom_exception dom_html_select_element_get_options(dom_html_select_element *ele, dom_html_options_collection **result);
extern dom_exception dom_html_select_element_get_disabled(dom_html_select_element *ele, bool *result);
extern dom_exception dom_html_select_element_set_disabled(dom_html_select_element *ele, bool disabled);
extern dom_exception dom_html_select_element_get_multiple(dom_html_select_element *ele, bool *result);
extern dom_exception dom_html_select_element_set_multiple(dom_html_select_element *ele, bool multiple);
extern dom_exception dom_html_select_element_get_name(dom_html_select_element *ele, dom_string **result);
extern dom_exception dom_html_select_element_set_name(dom_html_select_element *ele, dom_string *name);
extern dom_exception dom_html_select_element_get_size(dom_html_select_element *ele, long *result);
extern dom_exception dom_html_select_element_set_size(dom_html_select_element *ele, long size);
extern dom_exception dom_html_select_element_get_tab_index(dom_html_select_element *ele, long *result);
extern dom_exception dom_html_select_element_set_tab_index(dom_html_select_element *ele, long tab_index);
extern dom_exception dom_html_select_element_add(dom_html_select_element *ele, dom_html_element *element, dom_html_element *before);
extern dom_exception dom_html_select_element_remove(dom_html_select_element *ele, long index);
extern dom_exception dom_html_select_element_blur(dom_html_select_element *ele);
extern dom_exception dom_html_select_element_focus(dom_html_select_element *ele);

/* --- dom_html_option_element --- */
extern dom_exception dom_html_option_element_get_form(dom_html_option_element *ele, dom_html_form_element **result);
extern dom_exception dom_html_option_element_get_default_selected(dom_html_option_element *ele, bool *result);
extern dom_exception dom_html_option_element_set_default_selected(dom_html_option_element *ele, bool selected);
extern dom_exception dom_html_option_element_get_text(dom_html_option_element *ele, dom_string **result);
extern dom_exception dom_html_option_element_get_index(dom_html_option_element *ele, long *result);
extern dom_exception dom_html_option_element_get_disabled(dom_html_option_element *ele, bool *result);
extern dom_exception dom_html_option_element_set_disabled(dom_html_option_element *ele, bool disabled);
extern dom_exception dom_html_option_element_get_label(dom_html_option_element *ele, dom_string **result);
extern dom_exception dom_html_option_element_set_label(dom_html_option_element *ele, dom_string *label);
extern dom_exception dom_html_option_element_get_selected(dom_html_option_element *ele, bool *result);
extern dom_exception dom_html_option_element_set_selected(dom_html_option_element *ele, bool selected);
extern dom_exception dom_html_option_element_get_value(dom_html_option_element *ele, dom_string **result);
extern dom_exception dom_html_option_element_set_value(dom_html_option_element *ele, dom_string *value);

/* --- dom_html_text_area_element --- */
extern dom_exception dom_html_text_area_element_get_default_value(dom_html_text_area_element *ele, dom_string **result);
extern dom_exception dom_html_text_area_element_set_default_value(dom_html_text_area_element *ele, dom_string *value);
extern dom_exception dom_html_text_area_element_get_form(dom_html_text_area_element *ele, dom_html_form_element **result);
extern dom_exception dom_html_text_area_element_get_access_key(dom_html_text_area_element *ele, dom_string **result);
extern dom_exception dom_html_text_area_element_set_access_key(dom_html_text_area_element *ele, dom_string *access_key);
extern dom_exception dom_html_text_area_element_get_cols(dom_html_text_area_element *ele, long *result);
extern dom_exception dom_html_text_area_element_set_cols(dom_html_text_area_element *ele, long cols);
extern dom_exception dom_html_text_area_element_get_disabled(dom_html_text_area_element *ele, bool *result);
extern dom_exception dom_html_text_area_element_set_disabled(dom_html_text_area_element *ele, bool disabled);
extern dom_exception dom_html_text_area_element_get_name(dom_html_text_area_element *ele, dom_string **result);
extern dom_exception dom_html_text_area_element_set_name(dom_html_text_area_element *ele, dom_string *name);
extern dom_exception dom_html_text_area_element_get_read_only(dom_html_text_area_element *ele, bool *result);
extern dom_exception dom_html_text_area_element_set_read_only(dom_html_text_area_element *ele, bool read_only);
extern dom_exception dom_html_text_area_element_get_rows(dom_html_text_area_element *ele, long *result);
extern dom_exception dom_html_text_area_element_set_rows(dom_html_text_area_element *ele, long rows);
extern dom_exception dom_html_text_area_element_get_tab_index(dom_html_text_area_element *ele, long *result);
extern dom_exception dom_html_text_area_element_set_tab_index(dom_html_text_area_element *ele, long tab_index);
extern dom_exception dom_html_text_area_element_get_type(dom_html_text_area_element *ele, dom_string **result);
extern dom_exception dom_html_text_area_element_get_value(dom_html_text_area_element *ele, dom_string **result);
extern dom_exception dom_html_text_area_element_set_value(dom_html_text_area_element *ele, dom_string *value);
extern dom_exception dom_html_text_area_element_blur(dom_html_text_area_element *ele);
extern dom_exception dom_html_text_area_element_focus(dom_html_text_area_element *ele);
extern dom_exception dom_html_text_area_element_select(dom_html_text_area_element *ele);

/* --- dom_html_form_element --- */
extern dom_exception dom_html_form_element_get_name(dom_html_form_element *ele, dom_string **result);
extern dom_exception dom_html_form_element_set_name(dom_html_form_element *ele, dom_string *name);
extern dom_exception dom_html_form_element_get_accept_charset(dom_html_form_element *ele, dom_string **result);
extern dom_exception dom_html_form_element_set_accept_charset(dom_html_form_element *ele, dom_string *accept_charset);
extern dom_exception dom_html_form_element_get_action(dom_html_form_element *ele, dom_string **result);
extern dom_exception dom_html_form_element_set_action(dom_html_form_element *ele, dom_string *action);
extern dom_exception dom_html_form_element_get_enctype(dom_html_form_element *ele, dom_string **result);
extern dom_exception dom_html_form_element_set_enctype(dom_html_form_element *ele, dom_string *enctype);
extern dom_exception dom_html_form_element_get_method(dom_html_form_element *ele, dom_string **result);
extern dom_exception dom_html_form_element_set_method(dom_html_form_element *ele, dom_string *method);
extern dom_exception dom_html_form_element_get_target(dom_html_form_element *ele, dom_string **result);
extern dom_exception dom_html_form_element_set_target(dom_html_form_element *ele, dom_string *target);
extern dom_exception dom_html_form_element_get_length(dom_html_form_element *ele, unsigned long *result);
extern dom_exception dom_html_form_element_submit(dom_html_form_element *ele);
extern dom_exception dom_html_form_element_reset(dom_html_form_element *ele);
extern dom_exception dom_html_form_element_get_elements(dom_html_form_element *ele, dom_html_collection **result);

/* --- dom_html_anchor_element --- */
extern dom_exception dom_html_anchor_element_get_access_key(dom_html_anchor_element *ele, dom_string **result);
extern dom_exception dom_html_anchor_element_set_access_key(dom_html_anchor_element *ele, dom_string *access_key);
extern dom_exception dom_html_anchor_element_get_charset(dom_html_anchor_element *ele, dom_string **result);
extern dom_exception dom_html_anchor_element_set_charset(dom_html_anchor_element *ele, dom_string *charset);
extern dom_exception dom_html_anchor_element_get_coords(dom_html_anchor_element *ele, dom_string **result);
extern dom_exception dom_html_anchor_element_set_coords(dom_html_anchor_element *ele, dom_string *coords);
extern dom_exception dom_html_anchor_element_get_href(dom_html_anchor_element *ele, dom_string **result);
extern dom_exception dom_html_anchor_element_set_href(dom_html_anchor_element *ele, dom_string *href);
extern dom_exception dom_html_anchor_element_get_hreflang(dom_html_anchor_element *ele, dom_string **result);
extern dom_exception dom_html_anchor_element_set_hreflang(dom_html_anchor_element *ele, dom_string *hreflang);
extern dom_exception dom_html_anchor_element_get_name(dom_html_anchor_element *ele, dom_string **result);
extern dom_exception dom_html_anchor_element_set_name(dom_html_anchor_element *ele, dom_string *name);
extern dom_exception dom_html_anchor_element_get_rel(dom_html_anchor_element *ele, dom_string **result);
extern dom_exception dom_html_anchor_element_set_rel(dom_html_anchor_element *ele, dom_string *rel);
extern dom_exception dom_html_anchor_element_get_rev(dom_html_anchor_element *ele, dom_string **result);
extern dom_exception dom_html_anchor_element_set_rev(dom_html_anchor_element *ele, dom_string *rev);
extern dom_exception dom_html_anchor_element_get_shape(dom_html_anchor_element *ele, dom_string **result);
extern dom_exception dom_html_anchor_element_set_shape(dom_html_anchor_element *ele, dom_string *shape);
extern dom_exception dom_html_anchor_element_get_tab_index(dom_html_anchor_element *ele, long *result);
extern dom_exception dom_html_anchor_element_set_tab_index(dom_html_anchor_element *ele, long tab_index);
extern dom_exception dom_html_anchor_element_get_target(dom_html_anchor_element *ele, dom_string **result);
extern dom_exception dom_html_anchor_element_set_target(dom_html_anchor_element *ele, dom_string *target);
extern dom_exception dom_html_anchor_element_get_type(dom_html_anchor_element *ele, dom_string **result);
extern dom_exception dom_html_anchor_element_set_type(dom_html_anchor_element *ele, dom_string *type);
extern dom_exception dom_html_anchor_element_blur(dom_html_anchor_element *ele);
extern dom_exception dom_html_anchor_element_focus(dom_html_anchor_element *ele);

/* --- dom_html_image_element --- */
extern dom_exception dom_html_image_element_get_name(dom_html_image_element *ele, dom_string **result);
extern dom_exception dom_html_image_element_set_name(dom_html_image_element *ele, dom_string *name);
extern dom_exception dom_html_image_element_get_align(dom_html_image_element *ele, dom_string **result);
extern dom_exception dom_html_image_element_set_align(dom_html_image_element *ele, dom_string *align);
extern dom_exception dom_html_image_element_get_alt(dom_html_image_element *ele, dom_string **result);
extern dom_exception dom_html_image_element_set_alt(dom_html_image_element *ele, dom_string *alt);
extern dom_exception dom_html_image_element_get_border(dom_html_image_element *ele, dom_string **result);
extern dom_exception dom_html_image_element_set_border(dom_html_image_element *ele, dom_string *border);
extern dom_exception dom_html_image_element_get_height(dom_html_image_element *ele, unsigned long *result);
extern dom_exception dom_html_image_element_set_height(dom_html_image_element *ele, unsigned long height);
extern dom_exception dom_html_image_element_get_hspace(dom_html_image_element *ele, long *result);
extern dom_exception dom_html_image_element_set_hspace(dom_html_image_element *ele, long hspace);
extern dom_exception dom_html_image_element_get_is_map(dom_html_image_element *ele, bool *result);
extern dom_exception dom_html_image_element_set_is_map(dom_html_image_element *ele, bool is_map);
extern dom_exception dom_html_image_element_get_long_desc(dom_html_image_element *ele, dom_string **result);
extern dom_exception dom_html_image_element_set_long_desc(dom_html_image_element *ele, dom_string *long_desc);
extern dom_exception dom_html_image_element_get_src(dom_html_image_element *ele, dom_string **result);
extern dom_exception dom_html_image_element_set_src(dom_html_image_element *ele, dom_string *src);
extern dom_exception dom_html_image_element_get_use_map(dom_html_image_element *ele, dom_string **result);
extern dom_exception dom_html_image_element_set_use_map(dom_html_image_element *ele, dom_string *use_map);
extern dom_exception dom_html_image_element_get_vspace(dom_html_image_element *ele, long *result);
extern dom_exception dom_html_image_element_set_vspace(dom_html_image_element *ele, long vspace);
extern dom_exception dom_html_image_element_get_width(dom_html_image_element *ele, unsigned long *result);
extern dom_exception dom_html_image_element_set_width(dom_html_image_element *ele, unsigned long width);

/* --- dom_html_table_element --- */
extern dom_exception dom_html_table_element_get_caption(dom_html_table_element *ele, dom_html_table_caption_element **result);
extern dom_exception dom_html_table_element_set_caption(dom_html_table_element *ele, dom_html_table_caption_element *caption);
extern dom_exception dom_html_table_element_get_t_head(dom_html_table_element *ele, dom_html_table_section_element **result);
extern dom_exception dom_html_table_element_set_t_head(dom_html_table_element *ele, dom_html_table_section_element *t_head);
extern dom_exception dom_html_table_element_get_t_foot(dom_html_table_element *ele, dom_html_table_section_element **result);
extern dom_exception dom_html_table_element_set_t_foot(dom_html_table_element *ele, dom_html_table_section_element *t_foot);
extern dom_exception dom_html_table_element_get_rows(dom_html_table_element *ele, dom_html_collection **result);
extern dom_exception dom_html_table_element_get_t_bodies(dom_html_table_element *ele, dom_html_collection **result);
extern dom_exception dom_html_table_element_get_align(dom_html_table_element *ele, dom_string **result);
extern dom_exception dom_html_table_element_set_align(dom_html_table_element *ele, dom_string *align);
extern dom_exception dom_html_table_element_get_bg_color(dom_html_table_element *ele, dom_string **result);
extern dom_exception dom_html_table_element_set_bg_color(dom_html_table_element *ele, dom_string *bg_color);
extern dom_exception dom_html_table_element_get_border(dom_html_table_element *ele, dom_string **result);
extern dom_exception dom_html_table_element_set_border(dom_html_table_element *ele, dom_string *border);
extern dom_exception dom_html_table_element_get_cell_padding(dom_html_table_element *ele, dom_string **result);
extern dom_exception dom_html_table_element_set_cell_padding(dom_html_table_element *ele, dom_string *cell_padding);
extern dom_exception dom_html_table_element_get_cell_spacing(dom_html_table_element *ele, dom_string **result);
extern dom_exception dom_html_table_element_set_cell_spacing(dom_html_table_element *ele, dom_string *cell_spacing);
extern dom_exception dom_html_table_element_get_frame(dom_html_table_element *ele, dom_string **result);
extern dom_exception dom_html_table_element_set_frame(dom_html_table_element *ele, dom_string *frame);
extern dom_exception dom_html_table_element_get_rules(dom_html_table_element *ele, dom_string **result);
extern dom_exception dom_html_table_element_set_rules(dom_html_table_element *ele, dom_string *rules);
extern dom_exception dom_html_table_element_get_summary(dom_html_table_element *ele, dom_string **result);
extern dom_exception dom_html_table_element_set_summary(dom_html_table_element *ele, dom_string *summary);
extern dom_exception dom_html_table_element_get_width(dom_html_table_element *ele, dom_string **result);
extern dom_exception dom_html_table_element_set_width(dom_html_table_element *ele, dom_string *width);
extern dom_exception dom_html_table_element_create_caption(dom_html_table_element *ele, dom_node **result);
extern dom_exception dom_html_table_element_delete_caption(dom_html_table_element *ele);
extern dom_exception dom_html_table_element_create_t_head(dom_html_table_element *ele, dom_node **result);
extern dom_exception dom_html_table_element_delete_t_head(dom_html_table_element *ele);
extern dom_exception dom_html_table_element_create_t_foot(dom_html_table_element *ele, dom_node **result);
extern dom_exception dom_html_table_element_delete_t_foot(dom_html_table_element *ele);
extern dom_exception dom_html_table_element_insert_row(dom_html_table_element *ele, long index, dom_node **result);
extern dom_exception dom_html_table_element_delete_row(dom_html_table_element *ele, long index);

/* --- dom_html_table_row_element --- */
extern dom_exception dom_html_table_row_element_get_row_index(dom_html_table_row_element *ele, long *result);
extern dom_exception dom_html_table_row_element_get_section_row_index(dom_html_table_row_element *ele, long *result);
extern dom_exception dom_html_table_row_element_get_cells(dom_html_table_row_element *ele, dom_html_collection **result);
extern dom_exception dom_html_table_row_element_get_align(dom_html_table_row_element *ele, dom_string **result);
extern dom_exception dom_html_table_row_element_set_align(dom_html_table_row_element *ele, dom_string *align);
extern dom_exception dom_html_table_row_element_get_bg_color(dom_html_table_row_element *ele, dom_string **result);
extern dom_exception dom_html_table_row_element_set_bg_color(dom_html_table_row_element *ele, dom_string *bg_color);
extern dom_exception dom_html_table_row_element_get_ch(dom_html_table_row_element *ele, dom_string **result);
extern dom_exception dom_html_table_row_element_set_ch(dom_html_table_row_element *ele, dom_string *ch);
extern dom_exception dom_html_table_row_element_get_ch_off(dom_html_table_row_element *ele, dom_string **result);
extern dom_exception dom_html_table_row_element_set_ch_off(dom_html_table_row_element *ele, dom_string *ch_off);
extern dom_exception dom_html_table_row_element_get_v_align(dom_html_table_row_element *ele, dom_string **result);
extern dom_exception dom_html_table_row_element_set_v_align(dom_html_table_row_element *ele, dom_string *v_align);
extern dom_exception dom_html_table_row_element_insert_cell(dom_html_table_row_element *ele, long index, dom_node **result);
extern dom_exception dom_html_table_row_element_delete_cell(dom_html_table_row_element *ele, long index);

/* --- dom_html_table_section_element --- */
extern dom_exception dom_html_table_section_element_get_align(dom_html_table_section_element *ele, dom_string **result);
extern dom_exception dom_html_table_section_element_set_align(dom_html_table_section_element *ele, dom_string *align);
extern dom_exception dom_html_table_section_element_get_ch(dom_html_table_section_element *ele, dom_string **result);
extern dom_exception dom_html_table_section_element_set_ch(dom_html_table_section_element *ele, dom_string *ch);
extern dom_exception dom_html_table_section_element_get_ch_off(dom_html_table_section_element *ele, dom_string **result);
extern dom_exception dom_html_table_section_element_set_ch_off(dom_html_table_section_element *ele, dom_string *ch_off);
extern dom_exception dom_html_table_section_element_get_v_align(dom_html_table_section_element *ele, dom_string **result);
extern dom_exception dom_html_table_section_element_set_v_align(dom_html_table_section_element *ele, dom_string *v_align);
extern dom_exception dom_html_table_section_element_get_rows(dom_html_table_section_element *ele, dom_html_collection **result);
extern dom_exception dom_html_table_section_element_insert_row(dom_html_table_section_element *ele, long index, dom_node **result);
extern dom_exception dom_html_table_section_element_delete_row(dom_html_table_section_element *ele, long index);

/* --- dom_html_table_cell_element --- */
extern dom_exception dom_html_table_cell_element_get_cell_index(dom_html_table_cell_element *ele, long *result);
extern dom_exception dom_html_table_cell_element_get_abbr(dom_html_table_cell_element *ele, dom_string **result);
extern dom_exception dom_html_table_cell_element_set_abbr(dom_html_table_cell_element *ele, dom_string *abbr);
extern dom_exception dom_html_table_cell_element_get_align(dom_html_table_cell_element *ele, dom_string **result);
extern dom_exception dom_html_table_cell_element_set_align(dom_html_table_cell_element *ele, dom_string *align);
extern dom_exception dom_html_table_cell_element_get_axis(dom_html_table_cell_element *ele, dom_string **result);
extern dom_exception dom_html_table_cell_element_set_axis(dom_html_table_cell_element *ele, dom_string *axis);
extern dom_exception dom_html_table_cell_element_get_bg_color(dom_html_table_cell_element *ele, dom_string **result);
extern dom_exception dom_html_table_cell_element_set_bg_color(dom_html_table_cell_element *ele, dom_string *bg_color);
extern dom_exception dom_html_table_cell_element_get_ch(dom_html_table_cell_element *ele, dom_string **result);
extern dom_exception dom_html_table_cell_element_set_ch(dom_html_table_cell_element *ele, dom_string *ch);
extern dom_exception dom_html_table_cell_element_get_ch_off(dom_html_table_cell_element *ele, dom_string **result);
extern dom_exception dom_html_table_cell_element_set_ch_off(dom_html_table_cell_element *ele, dom_string *ch_off);
extern dom_exception dom_html_table_cell_element_get_col_span(dom_html_table_cell_element *ele, long *result);
extern dom_exception dom_html_table_cell_element_set_col_span(dom_html_table_cell_element *ele, long col_span);
extern dom_exception dom_html_table_cell_element_get_headers(dom_html_table_cell_element *ele, dom_string **result);
extern dom_exception dom_html_table_cell_element_set_headers(dom_html_table_cell_element *ele, dom_string *headers);
extern dom_exception dom_html_table_cell_element_get_height(dom_html_table_cell_element *ele, dom_string **result);
extern dom_exception dom_html_table_cell_element_set_height(dom_html_table_cell_element *ele, dom_string *height);
extern dom_exception dom_html_table_cell_element_get_no_wrap(dom_html_table_cell_element *ele, bool *result);
extern dom_exception dom_html_table_cell_element_set_no_wrap(dom_html_table_cell_element *ele, bool no_wrap);
extern dom_exception dom_html_table_cell_element_get_row_span(dom_html_table_cell_element *ele, long *result);
extern dom_exception dom_html_table_cell_element_set_row_span(dom_html_table_cell_element *ele, long row_span);
extern dom_exception dom_html_table_cell_element_get_scope(dom_html_table_cell_element *ele, dom_string **result);
extern dom_exception dom_html_table_cell_element_set_scope(dom_html_table_cell_element *ele, dom_string *scope);
extern dom_exception dom_html_table_cell_element_get_v_align(dom_html_table_cell_element *ele, dom_string **result);
extern dom_exception dom_html_table_cell_element_set_v_align(dom_html_table_cell_element *ele, dom_string *v_align);
extern dom_exception dom_html_table_cell_element_get_width(dom_html_table_cell_element *ele, dom_string **result);
extern dom_exception dom_html_table_cell_element_set_width(dom_html_table_cell_element *ele, dom_string *width);

/* --- dom_html_link_element --- */
extern dom_exception dom_html_link_element_get_disabled(dom_html_link_element *ele, bool *result);
extern dom_exception dom_html_link_element_set_disabled(dom_html_link_element *ele, bool disabled);
extern dom_exception dom_html_link_element_get_charset(dom_html_link_element *ele, dom_string **result);
extern dom_exception dom_html_link_element_set_charset(dom_html_link_element *ele, dom_string *charset);
extern dom_exception dom_html_link_element_get_href(dom_html_link_element *ele, dom_string **result);
extern dom_exception dom_html_link_element_set_href(dom_html_link_element *ele, dom_string *href);
extern dom_exception dom_html_link_element_get_hreflang(dom_html_link_element *ele, dom_string **result);
extern dom_exception dom_html_link_element_set_hreflang(dom_html_link_element *ele, dom_string *hreflang);
extern dom_exception dom_html_link_element_get_media(dom_html_link_element *ele, dom_string **result);
extern dom_exception dom_html_link_element_set_media(dom_html_link_element *ele, dom_string *media);
extern dom_exception dom_html_link_element_get_rel(dom_html_link_element *ele, dom_string **result);
extern dom_exception dom_html_link_element_set_rel(dom_html_link_element *ele, dom_string *rel);
extern dom_exception dom_html_link_element_get_rev(dom_html_link_element *ele, dom_string **result);
extern dom_exception dom_html_link_element_set_rev(dom_html_link_element *ele, dom_string *rev);
extern dom_exception dom_html_link_element_get_target(dom_html_link_element *ele, dom_string **result);
extern dom_exception dom_html_link_element_set_target(dom_html_link_element *ele, dom_string *target);
extern dom_exception dom_html_link_element_get_type(dom_html_link_element *ele, dom_string **result);
extern dom_exception dom_html_link_element_set_type(dom_html_link_element *ele, dom_string *type);

/* --- dom_html_style_element --- */
extern dom_exception dom_html_style_element_get_disabled(dom_html_style_element *ele, bool *result);
extern dom_exception dom_html_style_element_set_disabled(dom_html_style_element *ele, bool disabled);
extern dom_exception dom_html_style_element_get_media(dom_html_style_element *ele, dom_string **result);
extern dom_exception dom_html_style_element_set_media(dom_html_style_element *ele, dom_string *media);
extern dom_exception dom_html_style_element_get_type(dom_html_style_element *ele, dom_string **result);
extern dom_exception dom_html_style_element_set_type(dom_html_style_element *ele, dom_string *type);

/* --- dom_html_script_element --- */
extern dom_exception dom_html_script_element_get_text(dom_html_script_element *ele, dom_string **result);
extern dom_exception dom_html_script_element_set_text(dom_html_script_element *ele, dom_string *text);
extern dom_exception dom_html_script_element_get_html_for(dom_html_script_element *ele, dom_string **result);
extern dom_exception dom_html_script_element_set_html_for(dom_html_script_element *ele, dom_string *html_for);
extern dom_exception dom_html_script_element_get_event(dom_html_script_element *ele, dom_string **result);
extern dom_exception dom_html_script_element_set_event(dom_html_script_element *ele, dom_string *event);
extern dom_exception dom_html_script_element_get_charset(dom_html_script_element *ele, dom_string **result);
extern dom_exception dom_html_script_element_set_charset(dom_html_script_element *ele, dom_string *charset);
extern dom_exception dom_html_script_element_get_defer(dom_html_script_element *ele, bool *result);
extern dom_exception dom_html_script_element_set_defer(dom_html_script_element *ele, bool defer);
extern dom_exception dom_html_script_element_get_src(dom_html_script_element *ele, dom_string **result);
extern dom_exception dom_html_script_element_set_src(dom_html_script_element *ele, dom_string *src);
extern dom_exception dom_html_script_element_get_type(dom_html_script_element *ele, dom_string **result);
extern dom_exception dom_html_script_element_set_type(dom_html_script_element *ele, dom_string *type);
extern dom_exception dom_html_script_element_get_async(dom_html_script_element *ele, bool *result);
extern dom_exception dom_html_script_element_set_async(dom_html_script_element *ele, bool async_val);
extern dom_exception dom_html_script_element_get_flags(dom_html_script_element *ele, unsigned int *result);

/* --- dom_html_body_element --- */
extern dom_exception dom_html_body_element_get_a_link(dom_html_body_element *ele, dom_string **result);
extern dom_exception dom_html_body_element_set_a_link(dom_html_body_element *ele, dom_string *a_link);
extern dom_exception dom_html_body_element_get_background(dom_html_body_element *ele, dom_string **result);
extern dom_exception dom_html_body_element_set_background(dom_html_body_element *ele, dom_string *background);
extern dom_exception dom_html_body_element_get_bg_color(dom_html_body_element *ele, dom_string **result);
extern dom_exception dom_html_body_element_set_bg_color(dom_html_body_element *ele, dom_string *bg_color);
extern dom_exception dom_html_body_element_get_link(dom_html_body_element *ele, dom_string **result);
extern dom_exception dom_html_body_element_set_link(dom_html_body_element *ele, dom_string *link);
extern dom_exception dom_html_body_element_get_text(dom_html_body_element *ele, dom_string **result);
extern dom_exception dom_html_body_element_set_text(dom_html_body_element *ele, dom_string *text);
extern dom_exception dom_html_body_element_get_v_link(dom_html_body_element *ele, dom_string **result);
extern dom_exception dom_html_body_element_set_v_link(dom_html_body_element *ele, dom_string *v_link);

/* --- dom_html_meta_element --- */
extern dom_exception dom_html_meta_element_get_content(dom_html_meta_element *ele, dom_string **result);
extern dom_exception dom_html_meta_element_set_content(dom_html_meta_element *ele, dom_string *content);
extern dom_exception dom_html_meta_element_get_http_equiv(dom_html_meta_element *ele, dom_string **result);
extern dom_exception dom_html_meta_element_set_http_equiv(dom_html_meta_element *ele, dom_string *http_equiv);
extern dom_exception dom_html_meta_element_get_name(dom_html_meta_element *ele, dom_string **result);
extern dom_exception dom_html_meta_element_set_name(dom_html_meta_element *ele, dom_string *name);
extern dom_exception dom_html_meta_element_get_scheme(dom_html_meta_element *ele, dom_string **result);
extern dom_exception dom_html_meta_element_set_scheme(dom_html_meta_element *ele, dom_string *scheme);

/* --- dom_html_base_element --- */
extern dom_exception dom_html_base_element_get_href(dom_html_base_element *ele, dom_string **result);
extern dom_exception dom_html_base_element_set_href(dom_html_base_element *ele, dom_string *href);
extern dom_exception dom_html_base_element_get_target(dom_html_base_element *ele, dom_string **result);
extern dom_exception dom_html_base_element_set_target(dom_html_base_element *ele, dom_string *target);

/* --- dom_html_iframe_element --- */
extern dom_exception dom_html_iframe_element_get_align(dom_html_iframe_element *ele, dom_string **result);
extern dom_exception dom_html_iframe_element_set_align(dom_html_iframe_element *ele, dom_string *align);
extern dom_exception dom_html_iframe_element_get_frame_border(dom_html_iframe_element *ele, dom_string **result);
extern dom_exception dom_html_iframe_element_set_frame_border(dom_html_iframe_element *ele, dom_string *frame_border);
extern dom_exception dom_html_iframe_element_get_height(dom_html_iframe_element *ele, dom_string **result);
extern dom_exception dom_html_iframe_element_set_height(dom_html_iframe_element *ele, dom_string *height);
extern dom_exception dom_html_iframe_element_get_long_desc(dom_html_iframe_element *ele, dom_string **result);
extern dom_exception dom_html_iframe_element_set_long_desc(dom_html_iframe_element *ele, dom_string *long_desc);
extern dom_exception dom_html_iframe_element_get_margin_height(dom_html_iframe_element *ele, dom_string **result);
extern dom_exception dom_html_iframe_element_set_margin_height(dom_html_iframe_element *ele, dom_string *margin_height);
extern dom_exception dom_html_iframe_element_get_margin_width(dom_html_iframe_element *ele, dom_string **result);
extern dom_exception dom_html_iframe_element_set_margin_width(dom_html_iframe_element *ele, dom_string *margin_width);
extern dom_exception dom_html_iframe_element_get_name(dom_html_iframe_element *ele, dom_string **result);
extern dom_exception dom_html_iframe_element_set_name(dom_html_iframe_element *ele, dom_string *name);
extern dom_exception dom_html_iframe_element_get_scrolling(dom_html_iframe_element *ele, dom_string **result);
extern dom_exception dom_html_iframe_element_set_scrolling(dom_html_iframe_element *ele, dom_string *scrolling);
extern dom_exception dom_html_iframe_element_get_src(dom_html_iframe_element *ele, dom_string **result);
extern dom_exception dom_html_iframe_element_set_src(dom_html_iframe_element *ele, dom_string *src);
extern dom_exception dom_html_iframe_element_get_width(dom_html_iframe_element *ele, dom_string **result);
extern dom_exception dom_html_iframe_element_set_width(dom_html_iframe_element *ele, dom_string *width);
extern dom_exception dom_html_iframe_element_get_content_document(dom_html_iframe_element *ele, dom_document **result);

/* --- dom_html_frame_element --- */
extern dom_exception dom_html_frame_element_get_frame_border(dom_html_frame_element *ele, dom_string **result);
extern dom_exception dom_html_frame_element_set_frame_border(dom_html_frame_element *ele, dom_string *frame_border);
extern dom_exception dom_html_frame_element_get_long_desc(dom_html_frame_element *ele, dom_string **result);
extern dom_exception dom_html_frame_element_set_long_desc(dom_html_frame_element *ele, dom_string *long_desc);
extern dom_exception dom_html_frame_element_get_margin_height(dom_html_frame_element *ele, dom_string **result);
extern dom_exception dom_html_frame_element_set_margin_height(dom_html_frame_element *ele, dom_string *margin_height);
extern dom_exception dom_html_frame_element_get_margin_width(dom_html_frame_element *ele, dom_string **result);
extern dom_exception dom_html_frame_element_set_margin_width(dom_html_frame_element *ele, dom_string *margin_width);
extern dom_exception dom_html_frame_element_get_name(dom_html_frame_element *ele, dom_string **result);
extern dom_exception dom_html_frame_element_set_name(dom_html_frame_element *ele, dom_string *name);
extern dom_exception dom_html_frame_element_get_no_resize(dom_html_frame_element *ele, bool *result);
extern dom_exception dom_html_frame_element_set_no_resize(dom_html_frame_element *ele, bool no_resize);
extern dom_exception dom_html_frame_element_get_scrolling(dom_html_frame_element *ele, dom_string **result);
extern dom_exception dom_html_frame_element_set_scrolling(dom_html_frame_element *ele, dom_string *scrolling);
extern dom_exception dom_html_frame_element_get_src(dom_html_frame_element *ele, dom_string **result);
extern dom_exception dom_html_frame_element_set_src(dom_html_frame_element *ele, dom_string *src);
extern dom_exception dom_html_frame_element_get_content_document(dom_html_frame_element *ele, dom_document **result);

/* --- dom_html_object_element --- */
extern dom_exception dom_html_object_element_get_form(dom_html_object_element *ele, dom_html_form_element **result);
extern dom_exception dom_html_object_element_get_code(dom_html_object_element *ele, dom_string **result);
extern dom_exception dom_html_object_element_set_code(dom_html_object_element *ele, dom_string *code);
extern dom_exception dom_html_object_element_get_align(dom_html_object_element *ele, dom_string **result);
extern dom_exception dom_html_object_element_set_align(dom_html_object_element *ele, dom_string *align);
extern dom_exception dom_html_object_element_get_archive(dom_html_object_element *ele, dom_string **result);
extern dom_exception dom_html_object_element_set_archive(dom_html_object_element *ele, dom_string *archive);
extern dom_exception dom_html_object_element_get_border(dom_html_object_element *ele, dom_string **result);
extern dom_exception dom_html_object_element_set_border(dom_html_object_element *ele, dom_string *border);
extern dom_exception dom_html_object_element_get_code_base(dom_html_object_element *ele, dom_string **result);
extern dom_exception dom_html_object_element_set_code_base(dom_html_object_element *ele, dom_string *code_base);
extern dom_exception dom_html_object_element_get_code_type(dom_html_object_element *ele, dom_string **result);
extern dom_exception dom_html_object_element_set_code_type(dom_html_object_element *ele, dom_string *code_type);
extern dom_exception dom_html_object_element_get_data(dom_html_object_element *ele, dom_string **result);
extern dom_exception dom_html_object_element_set_data(dom_html_object_element *ele, dom_string *data);
extern dom_exception dom_html_object_element_get_declare(dom_html_object_element *ele, bool *result);
extern dom_exception dom_html_object_element_set_declare(dom_html_object_element *ele, bool declare);
extern dom_exception dom_html_object_element_get_height(dom_html_object_element *ele, dom_string **result);
extern dom_exception dom_html_object_element_set_height(dom_html_object_element *ele, dom_string *height);
extern dom_exception dom_html_object_element_get_hspace(dom_html_object_element *ele, long *result);
extern dom_exception dom_html_object_element_set_hspace(dom_html_object_element *ele, long hspace);
extern dom_exception dom_html_object_element_get_name(dom_html_object_element *ele, dom_string **result);
extern dom_exception dom_html_object_element_set_name(dom_html_object_element *ele, dom_string *name);
extern dom_exception dom_html_object_element_get_standby(dom_html_object_element *ele, dom_string **result);
extern dom_exception dom_html_object_element_set_standby(dom_html_object_element *ele, dom_string *standby);
extern dom_exception dom_html_object_element_get_tab_index(dom_html_object_element *ele, long *result);
extern dom_exception dom_html_object_element_set_tab_index(dom_html_object_element *ele, long tab_index);
extern dom_exception dom_html_object_element_get_type(dom_html_object_element *ele, dom_string **result);
extern dom_exception dom_html_object_element_set_type(dom_html_object_element *ele, dom_string *type);
extern dom_exception dom_html_object_element_get_use_map(dom_html_object_element *ele, dom_string **result);
extern dom_exception dom_html_object_element_set_use_map(dom_html_object_element *ele, dom_string *use_map);
extern dom_exception dom_html_object_element_get_vspace(dom_html_object_element *ele, long *result);
extern dom_exception dom_html_object_element_set_vspace(dom_html_object_element *ele, long vspace);
extern dom_exception dom_html_object_element_get_width(dom_html_object_element *ele, dom_string **result);
extern dom_exception dom_html_object_element_set_width(dom_html_object_element *ele, dom_string *width);
extern dom_exception dom_html_object_element_get_content_document(dom_html_object_element *ele, dom_document **result);

/* --- dom_html_button_element --- */
extern dom_exception dom_html_button_element_get_form(dom_html_button_element *ele, dom_html_form_element **result);
extern dom_exception dom_html_button_element_get_access_key(dom_html_button_element *ele, dom_string **result);
extern dom_exception dom_html_button_element_set_access_key(dom_html_button_element *ele, dom_string *access_key);
extern dom_exception dom_html_button_element_get_disabled(dom_html_button_element *ele, bool *result);
extern dom_exception dom_html_button_element_set_disabled(dom_html_button_element *ele, bool disabled);
extern dom_exception dom_html_button_element_get_name(dom_html_button_element *ele, dom_string **result);
extern dom_exception dom_html_button_element_set_name(dom_html_button_element *ele, dom_string *name);
extern dom_exception dom_html_button_element_get_tab_index(dom_html_button_element *ele, long *result);
extern dom_exception dom_html_button_element_set_tab_index(dom_html_button_element *ele, long tab_index);
extern dom_exception dom_html_button_element_get_type(dom_html_button_element *ele, dom_string **result);
extern dom_exception dom_html_button_element_set_type(dom_html_button_element *ele, dom_string *type);
extern dom_exception dom_html_button_element_get_value(dom_html_button_element *ele, dom_string **result);
extern dom_exception dom_html_button_element_set_value(dom_html_button_element *ele, dom_string *value);

/* --- dom_html_label_element --- */
extern dom_exception dom_html_label_element_get_form(dom_html_label_element *ele, dom_html_form_element **result);
extern dom_exception dom_html_label_element_get_access_key(dom_html_label_element *ele, dom_string **result);
extern dom_exception dom_html_label_element_set_access_key(dom_html_label_element *ele, dom_string *access_key);
extern dom_exception dom_html_label_element_get_html_for(dom_html_label_element *ele, dom_string **result);
extern dom_exception dom_html_label_element_set_html_for(dom_html_label_element *ele, dom_string *html_for);

/* --- dom_html_legend_element --- */
extern dom_exception dom_html_legend_element_get_form(dom_html_legend_element *ele, dom_html_form_element **result);
extern dom_exception dom_html_legend_element_get_access_key(dom_html_legend_element *ele, dom_string **result);
extern dom_exception dom_html_legend_element_set_access_key(dom_html_legend_element *ele, dom_string *access_key);
extern dom_exception dom_html_legend_element_get_align(dom_html_legend_element *ele, dom_string **result);
extern dom_exception dom_html_legend_element_set_align(dom_html_legend_element *ele, dom_string *align);

/* --- dom_html_br_element --- */
extern dom_exception dom_html_br_element_get_clear(dom_html_br_element *ele, dom_string **result);
extern dom_exception dom_html_br_element_set_clear(dom_html_br_element *ele, dom_string *clear);

/* --- dom_html_hr_element --- */
extern dom_exception dom_html_hr_element_get_align(dom_html_hr_element *ele, dom_string **result);
extern dom_exception dom_html_hr_element_set_align(dom_html_hr_element *ele, dom_string *align);
extern dom_exception dom_html_hr_element_get_no_shade(dom_html_hr_element *ele, bool *result);
extern dom_exception dom_html_hr_element_set_no_shade(dom_html_hr_element *ele, bool no_shade);
extern dom_exception dom_html_hr_element_get_size(dom_html_hr_element *ele, dom_string **result);
extern dom_exception dom_html_hr_element_set_size(dom_html_hr_element *ele, dom_string *size);
extern dom_exception dom_html_hr_element_get_width(dom_html_hr_element *ele, dom_string **result);
extern dom_exception dom_html_hr_element_set_width(dom_html_hr_element *ele, dom_string *width);

/* --- dom_html_canvas_element --- */
extern dom_exception dom_html_canvas_element_get_width(dom_html_canvas_element *ele, unsigned long *result);
extern dom_exception dom_html_canvas_element_set_width(dom_html_canvas_element *ele, unsigned long width);
extern dom_exception dom_html_canvas_element_get_height(dom_html_canvas_element *ele, unsigned long *result);
extern dom_exception dom_html_canvas_element_set_height(dom_html_canvas_element *ele, unsigned long height);

/* --- dom_html_font_element --- */
extern dom_exception dom_html_font_element_get_color(dom_html_font_element *ele, dom_string **result);
extern dom_exception dom_html_font_element_set_color(dom_html_font_element *ele, dom_string *color);
extern dom_exception dom_html_font_element_get_face(dom_html_font_element *ele, dom_string **result);
extern dom_exception dom_html_font_element_set_face(dom_html_font_element *ele, dom_string *face);
extern dom_exception dom_html_font_element_get_size(dom_html_font_element *ele, dom_string **result);
extern dom_exception dom_html_font_element_set_size(dom_html_font_element *ele, dom_string *size);

/* --- dom_html_map_element --- */
extern dom_exception dom_html_map_element_get_areas(dom_html_map_element *ele, dom_html_collection **result);
extern dom_exception dom_html_map_element_get_name(dom_html_map_element *ele, dom_string **result);
extern dom_exception dom_html_map_element_set_name(dom_html_map_element *ele, dom_string *name);

/* --- dom_html_area_element --- */
extern dom_exception dom_html_area_element_get_access_key(dom_html_area_element *ele, dom_string **result);
extern dom_exception dom_html_area_element_set_access_key(dom_html_area_element *ele, dom_string *access_key);
extern dom_exception dom_html_area_element_get_alt(dom_html_area_element *ele, dom_string **result);
extern dom_exception dom_html_area_element_set_alt(dom_html_area_element *ele, dom_string *alt);
extern dom_exception dom_html_area_element_get_coords(dom_html_area_element *ele, dom_string **result);
extern dom_exception dom_html_area_element_set_coords(dom_html_area_element *ele, dom_string *coords);
extern dom_exception dom_html_area_element_get_href(dom_html_area_element *ele, dom_string **result);
extern dom_exception dom_html_area_element_set_href(dom_html_area_element *ele, dom_string *href);
extern dom_exception dom_html_area_element_get_no_href(dom_html_area_element *ele, bool *result);
extern dom_exception dom_html_area_element_set_no_href(dom_html_area_element *ele, bool no_href);
extern dom_exception dom_html_area_element_get_shape(dom_html_area_element *ele, dom_string **result);
extern dom_exception dom_html_area_element_set_shape(dom_html_area_element *ele, dom_string *shape);
extern dom_exception dom_html_area_element_get_tab_index(dom_html_area_element *ele, long *result);
extern dom_exception dom_html_area_element_set_tab_index(dom_html_area_element *ele, long tab_index);
extern dom_exception dom_html_area_element_get_target(dom_html_area_element *ele, dom_string **result);
extern dom_exception dom_html_area_element_set_target(dom_html_area_element *ele, dom_string *target);

/* --- dom_html_opt_group_element --- */
extern dom_exception dom_html_opt_group_element_get_disabled(dom_html_opt_group_element *ele, bool *result);
extern dom_exception dom_html_opt_group_element_set_disabled(dom_html_opt_group_element *ele, bool disabled);
extern dom_exception dom_html_opt_group_element_get_label(dom_html_opt_group_element *ele, dom_string **result);
extern dom_exception dom_html_opt_group_element_set_label(dom_html_opt_group_element *ele, dom_string *label);

/* --- Remaining simple elements --- */
extern dom_exception dom_html_applet_element_get_align(dom_html_applet_element *ele, dom_string **result);
extern dom_exception dom_html_applet_element_set_align(dom_html_applet_element *ele, dom_string *align);

extern dom_exception dom_html_div_element_get_align(dom_html_div_element *ele, dom_string **result);
extern dom_exception dom_html_div_element_set_align(dom_html_div_element *ele, dom_string *align);

extern dom_exception dom_html_dlist_element_get_compact(dom_html_dlist_element *ele, bool *result);
extern dom_exception dom_html_dlist_element_set_compact(dom_html_dlist_element *ele, bool compact);

extern dom_exception dom_html_directory_element_get_compact(dom_html_directory_element *ele, bool *result);
extern dom_exception dom_html_directory_element_set_compact(dom_html_directory_element *ele, bool compact);

extern dom_exception dom_html_menu_element_get_compact(dom_html_menu_element *ele, bool *result);
extern dom_exception dom_html_menu_element_set_compact(dom_html_menu_element *ele, bool compact);

extern dom_exception dom_html_field_set_element_get_form(dom_html_field_set_element *ele, dom_html_form_element **result);

extern dom_exception dom_html_heading_element_get_align(dom_html_heading_element *ele, dom_string **result);
extern dom_exception dom_html_heading_element_set_align(dom_html_heading_element *ele, dom_string *align);

extern dom_exception dom_html_html_element_get_version(dom_html_html_element *ele, dom_string **result);
extern dom_exception dom_html_html_element_set_version(dom_html_html_element *ele, dom_string *version);

extern dom_exception dom_html_isindex_element_get_form(dom_html_isindex_element *ele, dom_html_form_element **result);
extern dom_exception dom_html_isindex_element_get_prompt(dom_html_isindex_element *ele, dom_string **result);
extern dom_exception dom_html_isindex_element_set_prompt(dom_html_isindex_element *ele, dom_string *prompt);

extern dom_exception dom_html_li_element_get_type(dom_html_li_element *ele, dom_string **result);
extern dom_exception dom_html_li_element_set_type(dom_html_li_element *ele, dom_string *type);
extern dom_exception dom_html_li_element_get_value(dom_html_li_element *ele, long *result);
extern dom_exception dom_html_li_element_set_value(dom_html_li_element *ele, long value);

extern dom_exception dom_html_mod_element_get_cite(dom_html_mod_element *ele, dom_string **result);
extern dom_exception dom_html_mod_element_set_cite(dom_html_mod_element *ele, dom_string *cite);
extern dom_exception dom_html_mod_element_get_date_time(dom_html_mod_element *ele, dom_string **result);
extern dom_exception dom_html_mod_element_set_date_time(dom_html_mod_element *ele, dom_string *date_time);

extern dom_exception dom_html_olist_element_get_compact(dom_html_olist_element *ele, bool *result);
extern dom_exception dom_html_olist_element_set_compact(dom_html_olist_element *ele, bool compact);
extern dom_exception dom_html_olist_element_get_start(dom_html_olist_element *ele, long *result);
extern dom_exception dom_html_olist_element_set_start(dom_html_olist_element *ele, long start);
extern dom_exception dom_html_olist_element_get_type(dom_html_olist_element *ele, dom_string **result);
extern dom_exception dom_html_olist_element_set_type(dom_html_olist_element *ele, dom_string *type);

extern dom_exception dom_html_paragraph_element_get_align(dom_html_paragraph_element *ele, dom_string **result);
extern dom_exception dom_html_paragraph_element_set_align(dom_html_paragraph_element *ele, dom_string *align);

extern dom_exception dom_html_param_element_get_name(dom_html_param_element *ele, dom_string **result);
extern dom_exception dom_html_param_element_set_name(dom_html_param_element *ele, dom_string *name);
extern dom_exception dom_html_param_element_get_type(dom_html_param_element *ele, dom_string **result);
extern dom_exception dom_html_param_element_set_type(dom_html_param_element *ele, dom_string *type);
extern dom_exception dom_html_param_element_get_value(dom_html_param_element *ele, dom_string **result);
extern dom_exception dom_html_param_element_set_value(dom_html_param_element *ele, dom_string *value);
extern dom_exception dom_html_param_element_get_value_type(dom_html_param_element *ele, dom_string **result);
extern dom_exception dom_html_param_element_set_value_type(dom_html_param_element *ele, dom_string *value_type);

extern dom_exception dom_html_pre_element_get_width(dom_html_pre_element *ele, long *result);
extern dom_exception dom_html_pre_element_set_width(dom_html_pre_element *ele, long width);

extern dom_exception dom_html_quote_element_get_cite(dom_html_quote_element *ele, dom_string **result);
extern dom_exception dom_html_quote_element_set_cite(dom_html_quote_element *ele, dom_string *cite);

extern dom_exception dom_html_title_element_get_text(dom_html_title_element *ele, dom_string **result);
extern dom_exception dom_html_title_element_set_text(dom_html_title_element *ele, dom_string *text);

extern dom_exception dom_html_u_list_element_get_compact(dom_html_u_list_element *ele, bool *result);
extern dom_exception dom_html_u_list_element_set_compact(dom_html_u_list_element *ele, bool compact);
extern dom_exception dom_html_u_list_element_get_type(dom_html_u_list_element *ele, dom_string **result);
extern dom_exception dom_html_u_list_element_set_type(dom_html_u_list_element *ele, dom_string *type);

/* --- dom_html_head_element --- */
extern dom_exception dom_html_head_element_get_profile(dom_html_head_element *ele, dom_string **result);
extern dom_exception dom_html_head_element_set_profile(dom_html_head_element *ele, dom_string *profile);

/* =========================================================
 * 26. dom_html_script_element_flags enum
 * ========================================================= */
typedef enum {
    DOM_HTML_SCRIPT_ELEMENT_FLAG_PARSER_INSERTED          = (1 << 0),
    DOM_HTML_SCRIPT_ELEMENT_FLAG_ALREADY_STARTED          = (1 << 1),
    DOM_HTML_SCRIPT_ELEMENT_FLAG_FROM_EXTERNAL            = (1 << 2),
    DOM_HTML_SCRIPT_ELEMENT_FLAG_NON_BLOCKING             = (1 << 3),
    DOM_HTML_SCRIPT_ELEMENT_FLAG_READY_TO_BE_PARSER_EXECUTED = (1 << 4)
} dom_html_script_element_flags;

/* =========================================================
 * 27. dom_attr_type enum
 * ========================================================= */
typedef enum {
    DOM_ATTR_UNSET   = 0,
    DOM_ATTR_STRING  = 1,
    DOM_ATTR_BOOL    = 2,
    DOM_ATTR_SHORT   = 3,
    DOM_ATTR_INTEGER = 4
} dom_attr_type;

/* =========================================================
 * 28. Hubbub parser types
 * ========================================================= */
typedef enum {
    DOM_HUBBUB_OK       = 0,
    DOM_HUBBUB_NOMEM    = 1,
    DOM_HUBBUB_BADPARM  = 2,
    DOM_HUBBUB_DOM      = 3,
    DOM_HUBBUB_ERR                  = (1 << 16),
    DOM_HUBBUB_ERR_PAUSED           = (1 << 16) + 1
} dom_hubbub_error;

typedef enum {
    DOM_HUBBUB_ENCODING_SOURCE_HEADER   = 0,
    DOM_HUBBUB_ENCODING_SOURCE_DETECTED = 1,
    DOM_HUBBUB_ENCODING_SOURCE_META     = 2
} dom_hubbub_encoding_source;

typedef dom_hubbub_error (*dom_script)(void *ctx, dom_node *node);

typedef struct dom_hubbub_parser_params {
    const char *enc;
    bool        fix_enc;
    bool        enable_script;
    dom_script  script;
    void       *ctx;
    dom_msg     msg;
    void       *msg_ctx;
} dom_hubbub_parser_params;

extern dom_hubbub_error dom_hubbub_parser_create(dom_hubbub_parser_params *params, dom_hubbub_parser **parser, dom_document **document);
extern void             dom_hubbub_parser_destroy(dom_hubbub_parser *parser);
extern dom_hubbub_error dom_hubbub_parser_parse_chunk(dom_hubbub_parser *parser, const unsigned char *data, size_t len);
extern dom_hubbub_error dom_hubbub_parser_completed(dom_hubbub_parser *parser);
extern dom_hubbub_error dom_hubbub_parser_pause(dom_hubbub_parser *parser, bool pause);
extern dom_hubbub_error dom_hubbub_parser_get_encoding(dom_hubbub_parser *parser, dom_hubbub_encoding_source *source, const char **encoding);

/* =========================================================
 * 29. Walk types
 * ========================================================= */
typedef enum {
    DOM_WALK_STAGE_ENTER = 0,
    DOM_WALK_STAGE_LEAVE = 1
} dom_walk_stage;

typedef enum {
    DOM_WALK_ENABLE_ENTER = (1 << 0),
    DOM_WALK_ENABLE_LEAVE = (1 << 1),
    DOM_WALK_ENABLE_ALL   = (1 << 0) | (1 << 1)
} dom_walk_enable;

typedef enum {
    DOM_WALK_CMD_CONTINUE = 0,
    DOM_WALK_CMD_ABORT    = 1,
    DOM_WALK_CMD_SKIP     = 2
} dom_walk_cmd;

typedef dom_walk_cmd (*dom_walk_cb)(dom_walk_stage stage, dom_node *node, void *pw);

extern dom_exception libdom_treewalk(dom_node *root, dom_walk_enable enable, dom_walk_cb callback, void *pw);

/* =========================================================
 * 30. Namespace enum
 * ========================================================= */
typedef enum dom_namespace {
    DOM_NAMESPACE_NULL  = 0,
    DOM_NAMESPACE_HTML  = 1,
    DOM_NAMESPACE_MATHML = 2,
    DOM_NAMESPACE_SVG   = 3,
    DOM_NAMESPACE_XLINK = 4,
    DOM_NAMESPACE_XML   = 5,
    DOM_NAMESPACE_XMLNS = 6,
    DOM_NAMESPACE_COUNT = 7
} dom_namespace;

extern dom_string *dom_namespaces[7];
extern dom_exception dom_namespace_finalise(void);

#endif /* MACOS9_DOM_DOM_H */
