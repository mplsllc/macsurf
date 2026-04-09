/*
 * MacSurf stub -- html/html.h
 * Minimal C89-compatible stub for CodeWarrior 8 compilation.
 * Licensed under GPL v2.
 */

#ifndef NETSURF_HTML_HTML_H
#define NETSURF_HTML_HTML_H

#include "utils/errors.h"

struct hlcache_handle;
struct form_control;
struct lwc_string;
struct box;
struct dom_node;

struct html_stylesheet {
	struct dom_node *node;
	struct hlcache_handle *sheet;
	unsigned char modified;
	unsigned char unused;
};

struct content_html_object {
	void *parent;
	struct content_html_object *next;
	struct hlcache_handle *content;
	struct box *box;
	unsigned int permitted_types;
	unsigned char background;
};

/* Index of first non-base stylesheet in html_get_stylesheets() array */
#ifndef STYLESHEET_START
#define STYLESHEET_START 0
#endif

nserror html_init(void);
int html_get_id_offset(struct hlcache_handle *h,
		struct lwc_string *frag_id, int *x, int *y);
void html_set_file_gadget_filename(struct hlcache_handle *hl,
		struct form_control *gadget, const char *fn);
struct content_html_object *html_get_objects(struct hlcache_handle *h,
		unsigned int *n);
struct html_stylesheet *html_get_stylesheets(struct hlcache_handle *h,
		unsigned int *n);
const char *html_get_base_target(struct hlcache_handle *h);

#endif
