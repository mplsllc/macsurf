/*
 * Copyright 2004 James Bursa <bursa@users.sourceforge.net>
 * Copyright 2003 Phil Mellor <monkeyson@users.sourceforge.net>
 * Copyright 2004 John Tytgat <joty@netsurf-browser.org>
 * Copyright 2005-9 John-Mark Bell <jmb@netsurf-browser.org>
 * Copyright 2009 Paul Blokus <paul_pl@users.sourceforge.net>
 * Copyright 2010 Michael Drake <tlsa@netsurf-browser.org>
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 *
 * NetSurf is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * NetSurf is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * \file
 * Form handling functions (implementation).
 */

#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <dom/dom.h>

#include "utils/corestrings.h"
#include "utils/log.h"
#include "utils/messages.h"
#include "utils/talloc.h"
#include "utils/url.h"
#include "utils/utf8.h"
#include "utils/ascii.h"
#include "netsurf/browser_window.h"
#include "netsurf/inttypes.h"
#include "netsurf/mouse.h"
#include "netsurf/plotters.h"
#include "netsurf/misc.h"
#include "content/fetch.h"
#include "content/hlcache.h"
#include "css/utils.h"
#include "desktop/knockout.h"
#include "desktop/scrollbar.h"
#include "desktop/textarea.h"

#include "html/html.h"
#include "html/private.h"
#include "html/layout.h"
#include "html/box.h"
#include "html/box_inspect.h"
#include "html/font.h"
#include "html/form_internal.h"

#define MAX_SELECT_HEIGHT 210
#define SELECT_LINE_SPACING 0.2
#define SELECT_BORDER_WIDTH 1
#define SELECT_SELECTED_COLOUR 0xDB9370

struct form_select_menu {
	int line_height;
	int width, height;
	struct scrollbar *scrollbar;
	int f_size;
	bool scroll_capture;
	select_menu_redraw_callback callback;
	void *client_data;
	struct content *c;
};

/* MacSurf: positional init for CW8 C89.
 * plot_style_t order: stroke_type, stroke_width, stroke_colour,
 *                     fill_type, fill_colour. */
static plot_style_t plot_style_fill_selected = {
	PLOT_OP_TYPE_NONE, 0, 0,
	PLOT_OP_TYPE_SOLID, SELECT_SELECTED_COLOUR
};

/* plot_font_style_t order: families, family, size, weight, flags,
 *                          background, foreground. */
static plot_font_style_t plot_fstyle_entry = {
	NULL,
	PLOT_FONT_FAMILY_SANS_SERIF,
	0,
	400,
	FONTF_NONE,
	0xffffff,
	0x000000
};


/**
 * Convert a string from UTF-8 to the specified charset
 * As a final fallback, this will attempt to convert to ISO-8859-1.
 *
 * \todo Return charset used?
 *
 * \param item String to convert
 * \param len Length of string to convert
 * \param charset Destination charset
 * \param fallback Fallback charset (may be NULL),
 *                 used iff converting to charset fails
 * \return Pointer to converted string (on heap, caller frees), or NULL
 */
static char *
form_encode_item(const char *item,
		 uint32_t len,
		 const char *charset,
		 const char *fallback)
{
	nserror err;
	char *ret = NULL;
	char cset[256];

	if (!item || !charset)
		return NULL;

	snprintf(cset, sizeof cset, "%s//TRANSLIT", charset);

	err = utf8_to_enc(item, cset, 0, &ret);
	if (err == NSERROR_BAD_ENCODING) {
		/* charset not understood, try without transliteration */
		snprintf(cset, sizeof cset, "%s", charset);
		err = utf8_to_enc(item, cset, len, &ret);

		if (err == NSERROR_BAD_ENCODING) {
			/* nope, try fallback charset (if any) */
			if (fallback) {
				snprintf(cset, sizeof cset,
						"%s//TRANSLIT", fallback);
				err = utf8_to_enc(item, cset, 0, &ret);

				if (err == NSERROR_BAD_ENCODING) {
					/* and without transliteration */
					snprintf(cset, sizeof cset,
							"%s", fallback);
					err = utf8_to_enc(item, cset, 0, &ret);
				}
			}

			if (err == NSERROR_BAD_ENCODING) {
				/* that also failed, use 8859-1 */
				err = utf8_to_enc(item, "ISO-8859-1//TRANSLIT",
						0, &ret);
				if (err == NSERROR_BAD_ENCODING) {
					/* and without transliteration */
					err = utf8_to_enc(item, "ISO-8859-1",
							0, &ret);
				}
			}
		}
	}
	if (err == NSERROR_NOMEM) {
		return NULL;
	}

	return ret;
}


/**
 * string allocation size for numeric values in multipart data
 */
#define FETCH_DATA_INT_VALUE_SIZE 20


/**
 * append split key name and integer value to a multipart data list
 *
 * \param name key name
 * \param ksfx key name suffix
 * \param value The value to encode
 * \param fetch_data_next_ptr The multipart data list to append to.
 */
static nserror
fetch_data_list_add_sname(const char *name,
			  const char *ksfx,
			  int value,
			  struct fetch_multipart_data ***fetch_data_next_ptr)
{
	struct fetch_multipart_data *fetch_data;
	int keysize;

	fetch_data = calloc(1, sizeof(*fetch_data));
	if (fetch_data == NULL) {
		NSLOG(netsurf, INFO, "failed allocation for fetch data");
		return NSERROR_NOMEM;
	}

	/* key name */
	keysize = snprintf(fetch_data->name, 0, "%s%s", name, ksfx);
	fetch_data->name = malloc(keysize + 1); /* allow for null */
	if (fetch_data->name == NULL) {
		free(fetch_data);
		NSLOG(netsurf, INFO,
		      "keyname allocation failure for %s%s", name, ksfx);
		return NSERROR_NOMEM;
	}
	snprintf(fetch_data->name, keysize + 1, "%s%s", name, ksfx);

	/* value */
	fetch_data->value = malloc(FETCH_DATA_INT_VALUE_SIZE);
	if (fetch_data->value == NULL) {
		free(fetch_data->name);
		free(fetch_data);
		NSLOG(netsurf, INFO, "value allocation failure");
		return NSERROR_NOMEM;
	}
	snprintf(fetch_data->value, FETCH_DATA_INT_VALUE_SIZE, "%d", value);

	/* link into list */
	**fetch_data_next_ptr = fetch_data;
	*fetch_data_next_ptr = &fetch_data->next;

	return NSERROR_OK;
}


/**
 * append DOM string name/value pair to a multipart data list
 *
 * \param name key name
 * \param value the value to associate with the key
 * \param rawfile the raw file value to associate with the key.
 * \param form_charset The form character set
 * \param docu_charset The document character set for fallback
 * \param fetch_data_next_ptr The multipart data list being constructed.
 * \return NSERROR_OK on success or appropriate error code.
 */
static nserror
fetch_data_list_add(dom_string *name,
		    dom_string *value,
		    const char *rawfile,
		    const char *form_charset,
		    const char *docu_charset,
		    struct fetch_multipart_data ***fetch_data_next_ptr)
{
	struct fetch_multipart_data *fetch_data;

	assert(name != NULL);

	fetch_data = calloc(1, sizeof(*fetch_data));
	if (fetch_data == NULL) {
		NSLOG(netsurf, INFO, "failed allocation for fetch data");
		return NSERROR_NOMEM;
	}

	fetch_data->name = form_encode_item(dom_string_data(name),
					    dom_string_byte_length(name),
					    form_charset,
					    docu_charset);
	if (fetch_data->name == NULL) {
		NSLOG(netsurf, INFO, "Could not encode name for fetch data");
		free(fetch_data);
		return NSERROR_NOMEM;
	}

	if (value == NULL) {
		fetch_data->value = strdup("");
	} else {
		fetch_data->value = form_encode_item(dom_string_data(value),
						     dom_string_byte_length(value),
						     form_charset,
						     docu_charset);
	}
	if (fetch_data->value == NULL) {
		NSLOG(netsurf, INFO, "Could not encode value for fetch data");
		free(fetch_data->name);
		free(fetch_data);
		return NSERROR_NOMEM;
	}

	/* deal with raw file name */
	if (rawfile != NULL) {
		fetch_data->file = true;
		fetch_data->rawfile = strdup(rawfile);
		if (fetch_data->rawfile == NULL) {
			NSLOG(netsurf, INFO,
			      "Could not encode rawfile value for fetch data");
			free(fetch_data->value);
			free(fetch_data->name);
			free(fetch_data);
			return NSERROR_NOMEM;
		}
	}

	/* link into list */
	**fetch_data_next_ptr = fetch_data;
	*fetch_data_next_ptr = &fetch_data->next;

	return NSERROR_OK;
}


/**
 * process form HTMLTextAreaElement into multipart data.
 *
 * \param text_area_element The form select DOM element to convert.
 * \param form_charset The form character set
 * \param doc_charset The document character set for fallback
 * \param fetch_data_next_ptr The multipart data list being constructed.
 * \return NSERROR_OK on success or appropriate error code.
 */
static nserror
form_dom_to_data_textarea(dom_html_text_area_element *text_area_element,
			  const char *form_charset,
			  const char *doc_charset,
			  struct fetch_multipart_data ***fetch_data_next_ptr)
{
	dom_exception exp; /* the result from DOM operations */
	bool element_disabled;
	dom_string *inputname;
	dom_string *inputvalue;
	nserror res;

	/* check if element is disabled */
	exp = dom_html_text_area_element_get_disabled(text_area_element,
						      &element_disabled);
	if (exp != DOM_NO_ERR) {
		NSLOG(netsurf, INFO,
		      "Could not get text area disabled property. exp %d", exp);
		return NSERROR_DOM;
	}

	if (element_disabled) {
		/* allow enumeration to continue after disabled element */
		return NSERROR_OK;
	}

	/* obtain name property */
	exp = dom_html_text_area_element_get_name(text_area_element,
						  &inputname);
	if (exp != DOM_NO_ERR) {
		NSLOG(netsurf, INFO,
		      "Could not get text area name property. exp %d", exp);
		return NSERROR_DOM;
	}

	if (inputname == NULL) {
		/* allow enumeration to continue after element with no name */
		return NSERROR_OK;
	}

	/* obtain text area value */
	exp = dom_html_text_area_element_get_value(text_area_element,
						   &inputvalue);
	if (exp != DOM_NO_ERR) {
		NSLOG(netsurf, INFO,
		      "Could not get text area content. exp %d", exp);
		dom_string_unref(inputname);
		return NSERROR_DOM;
	}

	/* add key/value pair to fetch data list */
	res = fetch_data_list_add(inputname,
				  inputvalue,
				  NULL,
				  form_charset,
				  doc_charset,
				  fetch_data_next_ptr);

	dom_string_unref(inputvalue);
	dom_string_unref(inputname);

	return res;
}


static nserror
form_dom_to_data_select_option(dom_html_option_element *option_element,
			      dom_string *keyname,
			      const char *form_charset,
			      const char *docu_charset,
			      struct fetch_multipart_data ***fetch_data_next_ptr)
{
	nserror res;
	dom_exception exp; /* the result from DOM operations */
	dom_string *value;
	bool selected;

	exp = dom_html_option_element_get_selected(option_element, &selected);
	if (exp != DOM_NO_ERR) {
		NSLOG(netsurf, INFO, "Could not get option selected property");
		return NSERROR_DOM;
	}

	if (!selected) {
		/* unselected options do not add fetch data entries */
		return NSERROR_OK;
	}

	exp = dom_html_option_element_get_value(option_element, &value);
	if (exp != DOM_NO_ERR) {
		NSLOG(netsurf, INFO, "Could not get option value");
		return NSERROR_DOM;
	}

	/* add key/value pair to fetch data list */
	res = fetch_data_list_add(keyname,
				  value,
				  NULL,
				  form_charset,
				  docu_charset,
				  fetch_data_next_ptr);

	dom_string_unref(value);

	return res;
}


/**
 * process form HTMLSelectElement into multipart data.
 *
 * \param select_element The form select DOM element to convert.
 * \param form_charset The form character set
 * \param doc_charset The document character set for fallback
 * \param fetch_data_next_ptr The multipart data list being constructed.
 * \return NSERROR_OK on success or appropriate error code.
 */
static nserror
form_dom_to_data_select(dom_html_select_element *select_element,
			const char *form_charset,
			const char *doc_charset,
			struct fetch_multipart_data ***fetch_data_next_ptr)
{
	nserror res = NSERROR_OK;
	dom_exception exp; /* the result from DOM operations */
	bool element_disabled;
	dom_string *inputname;
	dom_html_options_collection *options = NULL;
	uint32_t options_count;
	uint32_t option_index;
	dom_node *option_element = NULL;

	/* check if element is disabled */
	exp = dom_html_select_element_get_disabled(select_element,
						   &element_disabled);
	if (exp != DOM_NO_ERR) {
		NSLOG(netsurf, INFO,
		      "Could not get select disabled property. exp %d", exp);
		return NSERROR_DOM;
	}

	if (element_disabled) {
		/* allow enumeration to continue after disabled element */
		return NSERROR_OK;
	}

	/* obtain name property */
	exp = dom_html_select_element_get_name(select_element, &inputname);
	if (exp != DOM_NO_ERR) {
		NSLOG(netsurf, INFO,
		      "Could not get select name property. exp %d", exp);
		return NSERROR_DOM;
	}

	if (inputname == NULL) {
		/* allow enumeration to continue after element with no name */
		return NSERROR_OK;
	}

	/* get options collection */
	exp = dom_html_select_element_get_options(select_element, &options);
	if (exp != DOM_NO_ERR) {
		NSLOG(netsurf, INFO,
		      "Could not get select options collection");
		dom_string_unref(inputname);
		return NSERROR_DOM;
	}

	/* get options collection length */
	exp = dom_html_options_collection_get_length(options, &options_count);
	if (exp != DOM_NO_ERR) {
		NSLOG(netsurf, INFO,
		      "Could not get select options collection length");
		dom_html_options_collection_unref(options);
		dom_string_unref(inputname);
		return NSERROR_DOM;
	}

	/* iterate over options collection */
	for (option_index = 0; option_index < options_count; ++option_index) {
		exp = dom_html_options_collection_item(options,
						       option_index,
						       &option_element);
		if (exp != DOM_NO_ERR) {
			NSLOG(netsurf, INFO,
			      "Could not get options item %"PRId32, option_index);
			res = NSERROR_DOM;
		} else {
			res = form_dom_to_data_select_option(
				(dom_html_option_element *)option_element,
				inputname,
				form_charset,
				doc_charset,
				fetch_data_next_ptr);

			dom_node_unref(option_element);
		}

		if (res != NSERROR_OK) {
			break;
		}
	}

	dom_html_options_collection_unref(options);
	dom_string_unref(inputname);

	return res;
}


static nserror
form_dom_to_data_input_submit(dom_html_input_element *input_element,
			      dom_string *inputname,
			      const char *charset,
			      const char *document_charset,
			      dom_html_element **submit_button,
			      struct fetch_multipart_data ***fetch_data_next_ptr)
{
	dom_exception exp; /* the result from DOM operations */
	dom_string *inputvalue;
	nserror res;

	if (*submit_button == NULL) {
		/* caller specified no button so use this one */
		*submit_button = (dom_html_element *)input_element;
	} else if (*submit_button != (dom_html_element *)input_element) {
		return NSERROR_OK;
	}

	/* matched button used to submit form */
	exp = dom_html_input_element_get_value(input_element, &inputvalue);
	if (exp != DOM_NO_ERR) {
		NSLOG(netsurf, INFO, "Could not get submit button value");
		return NSERROR_DOM;
	}

	/* add key/value pair to fetch data list */
	res = fetch_data_list_add(inputname,
				  inputvalue,
				  NULL,
				  charset,
				  document_charset,
				  fetch_data_next_ptr);

	dom_string_unref(inputvalue);

	return res;
}


static nserror
form_dom_to_data_input_image(dom_html_input_element *input_element,
			     dom_string *inputname,
			     const char *charset,
			     const char *document_charset,
			     dom_html_element **submit_button,
			     struct fetch_multipart_data ***fetch_data_next_ptr)
{
	nserror res;
	dom_exception exp; /* the result from DOM operations */
	struct image_input_coords *coords;
	char *basename;

	/* Only use an image input if it was the thing which activated us */
	if (*submit_button != (dom_html_element *)input_element) {
		return NSERROR_OK;
	}

	exp = dom_node_get_user_data((dom_node *)input_element,
				     corestring_dom___ns_key_image_coords_node_data,
				     &coords);
	if (exp != DOM_NO_ERR) {
		NSLOG(netsurf, INFO, "Could not get image XY data");
		return NSERROR_DOM;
	}

	if (coords == NULL) {
		NSLOG(netsurf, INFO, "No XY data on the image input");
		return NSERROR_DOM;
	}

	/* encode input name once */
	basename = form_encode_item(dom_string_data(inputname),
				    dom_string_byte_length(inputname),
				    charset,
				    document_charset);
	if (basename == NULL) {
		NSLOG(netsurf, INFO, "Could not encode basename");
		return NSERROR_NOMEM;
	}

	res = fetch_data_list_add_sname(basename, ".x",
					coords->x,
					fetch_data_next_ptr);

	if (res == NSERROR_OK) {
		res = fetch_data_list_add_sname(basename, ".y",
						coords->y,
						fetch_data_next_ptr);
	}

	free(basename);

	return res;
}


static nserror
form_dom_to_data_input_checkbox(dom_html_input_element *input_element,
				dom_string *inputname,
				const char *charset,
				const char *document_charset,
				struct fetch_multipart_data ***fetch_data_next_ptr)
{
	nserror res;
	dom_exception exp; /* the result from DOM operations */
	bool checked;
	dom_string *inputvalue;

	exp = dom_html_input_element_get_checked(input_element, &checked);
	if (exp != DOM_NO_ERR) {
		NSLOG(netsurf, INFO,
		      "Could not get input element checked");
		return NSERROR_DOM;
	}

	if (!checked) {
		/* unchecked items do not generate a data entry */
		return NSERROR_OK;
	}

	exp = dom_html_input_element_get_value(input_element, &inputvalue);
	if (exp != DOM_NO_ERR) {
		NSLOG(netsurf, INFO,
		      "Could not get input element value");
		return NSERROR_DOM;
	}

	/* ensure a default value */
	if (inputvalue == NULL) {
		inputvalue = dom_string_ref(corestring_dom_on);
	}

	/* add key/value pair to fetch data list */
	res = fetch_data_list_add(inputname,
				  inputvalue,
				  NULL,
				  charset,
				  document_charset,
				  fetch_data_next_ptr);

	dom_string_unref(inputvalue);

	return res;
}


static nserror
form_dom_to_data_input_file(dom_html_input_element *input_element,
			    dom_string *inputname,
			    const char *charset,
			    const char *document_charset,
			    struct fetch_multipart_data ***fetch_data_next_ptr)
{
	nserror res;
	dom_exception exp; /* the result from DOM operations */
	dom_string *inputvalue;
	const char *rawfile = NULL;

	exp = dom_html_input_element_get_value(input_element, &inputvalue);
	if (exp != DOM_NO_ERR) {
		NSLOG(netsurf, INFO, "Could not get file value");
		return NSERROR_DOM;
	}

	exp = dom_node_get_user_data((dom_node *)input_element,
				     corestring_dom___ns_key_file_name_node_data,
				     &rawfile);
	if (exp != DOM_NO_ERR) {
		NSLOG(netsurf, INFO, "Could not get file rawname");
		return NSERROR_DOM;
	}

	if (rawfile == NULL) {
		rawfile = "";
	}

	/* add key/value pair to fetch data list */
	res = fetch_data_list_add(inputname,
				  inputvalue,
				  rawfile,
				  charset,
				  document_charset,
				  fetch_data_next_ptr);

	dom_string_unref(inputvalue);

	return res;
}


static nserror
form_dom_to_data_input_text(dom_html_input_element *input_element,
			    dom_string *inputname,
			    const char *charset,
			    const char *document_charset,
			    struct fetch_multipart_data ***fetch_data_next_ptr)
{
	nserror res;
	dom_exception exp; /* the result from DOM operations */
	dom_string *inputvalue;

	exp = dom_html_input_element_get_value(input_element, &inputvalue);
	if (exp != DOM_NO_ERR) {
		NSLOG(netsurf, INFO, "Could not get input value");
		return NSERROR_DOM;
	}

	/* add key/value pair to fetch data list */
	res = fetch_data_list_add(inputname,
				  inputvalue,
				  NULL,
				  charset,
				  document_charset,
				  fetch_data_next_ptr);

	dom_string_unref(inputvalue);

	return res;
}


/**
 * process form input element into multipart data.
 *
 * \param input_element The form input DOM element to convert.
 * \param charset The form character set
 * \param document_charset The document character set for fallback
 * \param submit_button The DOM element of the button submitting the form
 * \param had_submit A boolean value indicating if the submit button
 *                   has already been processed in the form element enumeration.
 * \param fetch_data_next_ptr The multipart data list being constructed.
 * \return NSERROR_OK on success or appropriate error code.
 */
static nserror
form_dom_to_data_input(dom_html_input_element *input_element,
		       const char *charset,
		       const char *document_charset,
		       dom_html_element **submit_button,
		       struct fetch_multipart_data ***fetch_data_next_ptr)
{
	dom_exception exp; /* the result from DOM operations */
	bool element_disabled;
	dom_string *inputname;
	dom_string *inputtype;
	nserror res;

	/* check if element is disabled */
	exp = dom_html_input_element_get_disabled(input_element,
						  &element_disabled);
	if (exp != DOM_NO_ERR) {
		NSLOG(netsurf, INFO,
		      "Could not get input disabled property. exp %d", exp);
		return NSERROR_DOM;
	}

	if (element_disabled) {
		/* disabled element requires no more processing */
		return NSERROR_OK;
	}

	/* obtain name property */
	exp = dom_html_input_element_get_name(input_element, &inputname);
	if (exp != DOM_NO_ERR) {
		NSLOG(netsurf, INFO,
		      "Could not get input name property. exp %d", exp);
		return NSERROR_DOM;
	}

	if (inputname == NULL) {
		/* element with no name is not converted */
		return NSERROR_OK;
	}

	/* get input type */
	exp = dom_html_input_element_get_type(input_element, &inputtype);
	if (exp != DOM_NO_ERR) {
		NSLOG(netsurf, INFO, "Could not get input element type");
		dom_string_unref(inputname);
		return NSERROR_DOM;
	}

	/* process according to input element type */
	if (dom_string_caseless_isequal(inputtype, corestring_dom_submit)) {

		res = form_dom_to_data_input_submit(input_element,
						    inputname,
						    charset,
						    document_charset,
						    submit_button,
						    fetch_data_next_ptr);

	} else if (dom_string_caseless_isequal(inputtype,
					       corestring_dom_image)) {

		res = form_dom_to_data_input_image(input_element,
						   inputname,
						   charset,
						   document_charset,
						   submit_button,
						   fetch_data_next_ptr);

	} else if (dom_string_caseless_isequal(inputtype,
					       corestring_dom_radio) ||
		   dom_string_caseless_isequal(inputtype,
					       corestring_dom_checkbox)) {

		res = form_dom_to_data_input_checkbox(input_element,
						      inputname,
						      charset,
						      document_charset,
						      fetch_data_next_ptr);

	} else if (dom_string_caseless_isequal(inputtype,
					       corestring_dom_file)) {

		res = form_dom_to_data_input_file(input_element,
						  inputname,
						  charset,
						  document_charset,
						  fetch_data_next_ptr);

	} else if (dom_string_caseless_isequal(inputtype,
					       corestring_dom_reset) ||
		   dom_string_caseless_isequal(inputtype,
					       corestring_dom_button)) {
		/* Skip these */
		NSLOG(netsurf, INFO, "Skipping RESET and BUTTON");
		res = NSERROR_OK;

	} else {
		/* Everything else is treated as text values */
		res = form_dom_to_data_input_text(input_element,
						  inputname,
						  charset,
						  document_charset,
						  fetch_data_next_ptr);

	}

	dom_string_unref(inputtype);
	dom_string_unref(inputname);

	return res;
}


/**
 * process form HTMLButtonElement into multipart data.
 *
 * https://html.spec.whatwg.org/multipage/form-elements.html#the-button-element
 *
 * \param button_element The form button DOM element to convert.
 * \param form_charset The form character set
 * \param doc_charset The document character set for fallback
 * \param submit_button The DOM element of the button submitting the form
 * \param fetch_data_next_ptr The multipart data list being constructed.
 * \return NSERROR_OK on success or appropriate error code.
 */
static nserror
form_dom_to_data_button(dom_html_button_element *button_element,
			const char *form_charset,
			const char *doc_charset,
			dom_html_element **submit_button,
			struct fetch_multipart_data ***fetch_data_next_ptr)
{
	dom_exception exp; /* the result from DOM operations */
	bool element_disabled;
	dom_string *inputname;
	dom_string *inputvalue;
	dom_string *inputtype;
	nserror res = NSERROR_OK;

	/* check if element is disabled */
	exp = dom_html_button_element_get_disabled(button_element,
						   &element_disabled);
	if (exp != DOM_NO_ERR) {
		NSLOG(netsurf, INFO,
		      "Unable to get disabled property. exp %d", exp);
		return NSERROR_DOM;
	}

	if (element_disabled) {
		/* allow enumeration to continue after disabled element */
		return NSERROR_OK;
	}

	/* get the type attribute */
	exp = dom_html_button_element_get_type(button_element, &inputtype);
	if (exp != DOM_NO_ERR) {
		NSLOG(netsurf, INFO, "Could not get button element type");
		return NSERROR_DOM;
	}

	/* If the type attribute is "reset" or "button" the element is
	 *  barred from constraint validation. Specification says
	 *  default and invalid values result in submit which will
	 *  be considered.
	 */
	if (dom_string_caseless_isequal(inputtype, corestring_dom_reset)) {
		/* multipart data entry not required for reset type */
		dom_string_unref(inputtype);
		return NSERROR_OK;
	}
	if (dom_string_caseless_isequal(inputtype, corestring_dom_button)) {
		/* multipart data entry not required for button type */
		dom_string_unref(inputtype);
		return NSERROR_OK;
	}
	dom_string_unref(inputtype);

	/* only submision button generates an element */
	if (*submit_button == NULL) {
		/* no submission button selected yet so use this one */
		*submit_button = (dom_html_element *)button_element;
	}
	if (*submit_button != (dom_html_element *)button_element) {
		return NSERROR_OK;
	}

	/* obtain name property */
	exp = dom_html_button_element_get_name(button_element, &inputname);
	if (exp != DOM_NO_ERR) {
		NSLOG(netsurf, INFO,
		      "Could not get button name property. exp %d", exp);
		return NSERROR_DOM;
	}

	if (inputname == NULL) {
		/* allow enumeration to continue after element with no name */
		return NSERROR_OK;
	}

	/* get button value and add to fetch data list */
	exp = dom_html_button_element_get_value(button_element, &inputvalue);
	if (exp != DOM_NO_ERR) {
		NSLOG(netsurf, INFO, "Could not get submit button value");
		res = NSERROR_DOM;
	} else {
		res = fetch_data_list_add(inputname,
					  inputvalue,
					  NULL,
					  form_charset,
					  doc_charset,
					  fetch_data_next_ptr);

		dom_string_unref(inputvalue);
	}

	dom_string_unref(inputname);

	return res;
}


/**
 * Find an acceptable character set encoding with which to submit the form
 *
 * \param form  The form
 * \return Pointer to charset name (on heap, caller should free) or NULL
 */
static char *form_acceptable_charset(struct form *form)
{
	char *temp, *c;

	if (!form->accept_charsets) {
		/* no accept-charsets attribute for this form */
		if (form->document_charset) {
			/* document charset present, so use it */
			return strdup(form->document_charset);
		} else {
			/* no document charset, so default to 8859-1 */
			return strdup("ISO-8859-1");
		}
	}

	/* make temporary copy of accept-charsets attribute */
	temp = strdup(form->accept_charsets);
	if (!temp)
		return NULL;

	/* make it upper case */
	for (c = temp; *c; c++) {
		*c = ascii_to_upper(*c);
	}

	/* is UTF-8 specified? */
	c = strstr(temp, "UTF-8");
	if (c) {
		free(temp);
		return strdup("UTF-8");
	}

	/* dispense with temporary copy */
	free(temp);

	/* according to RFC2070, the accept-charsets attribute of the
	 * form element contains a space and/or comma separated list */
	c = form->accept_charsets;

	/** \todo an improvement would be to choose an encoding
	 * acceptable to the server which covers as much of the input
	 * values as possible. Additionally, we need to handle the
	 * case where none of the acceptable encodings cover all the
	 * textual input values.  For now, we just extract the first
	 * element of the charset list
	 */
	while (*c && !ascii_is_space(*c)) {
		if (*c == ',')
			break;
		c++;
	}

	return strndup(form->accept_charsets, c - form->accept_charsets);
}


/**
 * Construct multipart data list from 'successful' controls via the DOM.
 *
 * All text strings in the successful controls list will be in the charset most
 * appropriate for submission. Therefore, no utf8_to_* processing should be
 * performed upon them.
 *
 * \todo The chosen charset needs to be made available such that it can be
 * included in the submission request (e.g. in the fetch's Content-Type header)
 *
 * See HTML 4.01 section 17.13.2.
 *
 * \note care is taken to abort even if the error is recoverable as it
 *       is not desirable to submit incomplete form data.
 *
 * \param[in] form form to search for successful controls
 * \param[in] submit_button control used to submit the form, if any
 * \param[out] fetch_data_out updated to point to linked list of
 *                             fetch_multipart_data, NULL if no controls
 * \return NSERROR_OK on success or appropriate error code
 */
static nserror
form_dom_to_data(struct form *form,
		 struct form_control *submit_control,
		 struct fetch_multipart_data **fetch_data_out)
{
	nserror res = NSERROR_OK;
	char *charset; /* form characterset */
	dom_exception exp; /* the result from DOM operations */
	dom_html_collection *elements = NULL; /* the dom form elements */
	uint32_t element_count; /* the number of elements in the DOM form */
	uint32_t element_idx; /* the index of thr enumerated element */
	dom_node *element = NULL; /* the DOM form element */
	dom_string *nodename = NULL; /* the DOM node name of the element */
	struct fetch_multipart_data *fetch_data = NULL; /* fetch data list */
	struct fetch_multipart_data **fetch_data_next = &fetch_data;
	dom_html_element *submit_button;

	/* obtain the submit_button DOM node from the control */
	if (submit_control != NULL) {
		submit_button = submit_control->node;
	} else {
		submit_button = NULL;
	}

	/** \todo Replace this call with something DOMish */
	charset = form_acceptable_charset(form);
	if (charset == NULL) {
		NSLOG(netsurf, INFO, "failed to find charset");
		return NSERROR_NOMEM;
	}

	/* obtain the form elements and count */
	exp = dom_html_form_element_get_elements(form->node, &elements);
	if (exp != DOM_NO_ERR) {
		NSLOG(netsurf, INFO, "Could not get form elements");
		free(charset);
		return NSERROR_DOM;
	}

	exp = dom_html_collection_get_length(elements, &element_count);
	if (exp != DOM_NO_ERR) {
		NSLOG(netsurf, INFO, "Could not get form element count");
		res = NSERROR_DOM;
		goto form_dom_to_data_error;
	}

	for (element_idx = 0; element_idx < element_count; element_idx++) {
		/* obtain a form element */
		exp = dom_html_collection_item(elements, element_idx, &element);
		if (exp != DOM_NO_ERR) {
			NSLOG(netsurf, INFO,
			      "retrieving form element %"PRIu32" failed with %d",
			      element_idx, exp);
			res = NSERROR_DOM;
			goto form_dom_to_data_error;
		}

		/* node name from element */
		exp = dom_node_get_node_name(element, &nodename);
		if (exp != DOM_NO_ERR) {
			NSLOG(netsurf, INFO,
			      "getting element node name %"PRIu32" failed with %d",
			      element_idx, exp);
			dom_node_unref(element);
			res = NSERROR_DOM;
			goto form_dom_to_data_error;
		}

		if (dom_string_isequal(nodename, corestring_dom_TEXTAREA)) {
			/* Form element is HTMLTextAreaElement */
			res = form_dom_to_data_textarea(
				(dom_html_text_area_element *)element,
				charset,
				form->document_charset,
				&fetch_data_next);

		} else if (dom_string_isequal(nodename, corestring_dom_SELECT)) {
			/* Form element is HTMLSelectElement */
			res = form_dom_to_data_select(
				(dom_html_select_element *)element,
				charset,
				form->document_charset,
				&fetch_data_next);

		} else if (dom_string_isequal(nodename, corestring_dom_INPUT)) {
			/* Form element is HTMLInputElement */
			res = form_dom_to_data_input(
				(dom_html_input_element *)element,
				charset,
				form->document_charset,
				&submit_button,
				&fetch_data_next);

		} else if (dom_string_isequal(nodename, corestring_dom_BUTTON)) {
			/* Form element is HTMLButtonElement */
			res = form_dom_to_data_button(
				(dom_html_button_element *)element,
				charset,
				form->document_charset,
				&submit_button,
				&fetch_data_next);

		} else {
			/* Form element is not handled */
			NSLOG(netsurf, INFO,
			      "Unhandled element type: %*s",
			      (int)dom_string_byte_length(nodename),
			      dom_string_data(nodename));
			res = NSERROR_DOM;

		}

		dom_string_unref(nodename);
		dom_node_unref(element);

		/* abort form element enumeration on error */
		if (res != NSERROR_OK) {
			goto form_dom_to_data_error;
		}
	}

	*fetch_data_out = fetch_data;
	dom_html_collection_unref(elements);
	free(charset);

	return NSERROR_OK;

form_dom_to_data_error:
	fetch_multipart_data_destroy(fetch_data);
	dom_html_collection_unref(elements);
	free(charset);

	return res;
}

/**
 * Encode controls using application/x-www-form-urlencoded.
 *
 * \param[in] form form to which successful controls relate
 * \param[in] control linked list of fetch_multipart_data
 * \param[out] encoded_out URL-encoded form data
 * \return NSERROR_OK on success and \a encoded_out updated else appropriate error code
 */
static nserror
form_url_encode(struct form *form,
		struct fetch_multipart_data *control,
		char **encoded_out)
{
	char *name, *value;
	char *s, *s2;
	unsigned int len, len1, len_init;
	nserror res;

	s = malloc(1);

	if (s == NULL) {
		return NSERROR_NOMEM;
	}

	s[0] = '\0';
	len_init = len = 0;

	for (; control; control = control->next) {
		res = url_escape(control->name, true, NULL, &name);
		if (res != NSERROR_OK) {
			free(s);
			return res;
		}

		res = url_escape(control->value, true, NULL, &value);
		if (res != NSERROR_OK) {
			free(name);
			free(s);
			return res;
		}

		/* resize string to allow for new key/value pair,
		 *  equals, amphersand and terminator
		 */
		len1 = len + strlen(name) + strlen(value) + 2;
		s2 = realloc(s, len1 + 1);
		if (s2 == NULL) {
			free(value);
			free(name);
			free(s);
			return NSERROR_NOMEM;
		}
		s = s2;

		snprintf(s + len, (len1 + 1) - len, "%s=%s&", name, value);
		len = len1;
		free(name);
		free(value);
	}

	if (len > len_init) {
		/* Replace trailing '&' */
		s[len - 1] = '\0';
	}

	*encoded_out = s;

	return NSERROR_OK;
}


/**
 * Callback for the select menus scroll
 */
static void
form_select_menu_scroll_callback(void *client_data,
				 struct scrollbar_msg_data *scrollbar_data)
{
	struct form_control *control = client_data;
	struct form_select_menu *menu = control->data.select.menu;
	html_content *html = (html_content *)menu->c;

	switch (scrollbar_data->msg) {
		case SCROLLBAR_MSG_MOVED:
			menu->callback(menu->client_data,
					0, 0,
					menu->width,
					menu->height);
			break;
		case SCROLLBAR_MSG_SCROLL_START:
		{
			struct rect rect;
			rect.x0 = scrollbar_data->x0;
			rect.y0 = scrollbar_data->y0;
			rect.x1 = scrollbar_data->x1;
			rect.y1 = scrollbar_data->y1;

			browser_window_set_drag_type(html->bw,
					DRAGGING_CONTENT_SCROLLBAR, &rect);

			menu->scroll_capture = true;
		}
			break;
		case SCROLLBAR_MSG_SCROLL_FINISHED:
			menu->scroll_capture = false;

			browser_window_set_drag_type(html->bw,
					DRAGGING_NONE, NULL);
			break;
		default:
			break;
	}
}


/**
 * Process a selection from a form select menu.
 *
 * \param  html The html content handle for the form
 * \param  control  form control with menu
 * \param  item	    index of item selected from the menu
 * \return NSERROR_OK or appropriate error code.
 */
static nserror
form__select_process_selection(html_content *html,
			       struct form_control *control,
			       int item)
{
	struct box *inline_box;
	struct form_option *o;
	int count;
	nserror ret = NSERROR_OK;

	assert(control != NULL);
	assert(html != NULL);

	/**
	 * \todo Even though the form code is effectively part of the html
	 *        content handler, poking around inside contents is not good
	 */

	inline_box = control->box->children->children;

	for (count = 0, o = control->data.select.items;
			o != NULL;
			count++, o = o->next) {
		if (!control->data.select.multiple && o->selected) {
			o->selected = false;
			dom_html_option_element_set_selected(o->node, false);
		}

		if (count == item) {
			if (control->data.select.multiple) {
				if (o->selected) {
					o->selected = false;
					dom_html_option_element_set_selected(
							o->node, false);
					control->data.select.num_selected--;
				} else {
					o->selected = true;
					dom_html_option_element_set_selected(
							o->node, true);
					control->data.select.num_selected++;
				}
			} else {
				dom_html_option_element_set_selected(
						o->node, true);
				o->selected = true;
			}
		}

		if (o->selected) {
			control->data.select.current = o;
		}
	}

	talloc_free(inline_box->text);
	inline_box->text = 0;

	if (control->data.select.num_selected == 0) {
		inline_box->text = talloc_strdup(html->bctx,
				messages_get("Form_None"));
	} else if (control->data.select.num_selected == 1) {
		inline_box->text = talloc_strdup(html->bctx,
				control->data.select.current->text);
	} else {
		inline_box->text = talloc_strdup(html->bctx,
				messages_get("Form_Many"));
	}

	if (!inline_box->text) {
		ret = NSERROR_NOMEM;
		inline_box->length = 0;
	} else {
		inline_box->length = strlen(inline_box->text);
	}
	inline_box->width = control->box->width;

	html__redraw_a_box(html, control->box);

	return ret;
}


/**
 * Handle a click on the area of the currently opened select menu.
 *
 * \param control the select menu which received the click
 * \param x X coordinate of click
 * \param y Y coordinate of click
 */
static void form_select_menu_clicked(struct form_control *control, int x, int y)
{
	struct form_select_menu *menu = control->data.select.menu;
	struct form_option *option;
	html_content *html = (html_content *)menu->c;
	int line_height, line_height_with_spacing;
	int item_bottom_y;
	int scroll, i;

	scroll = scrollbar_get_offset(menu->scrollbar);

	line_height = menu->line_height;
	line_height_with_spacing = line_height +
			line_height * SELECT_LINE_SPACING;

	option = control->data.select.items;
	item_bottom_y = line_height_with_spacing;
	i = 0;
	while (option && item_bottom_y < scroll + y) {
		item_bottom_y += line_height_with_spacing;
		option = option->next;
		i++;
	}

	if (option != NULL) {
		form__select_process_selection(html, control, i);
	}

	menu->callback(menu->client_data, 0, 0, menu->width, menu->height);
}


/* exported interface documented in html/form_internal.h */
void form_add_control(struct form *form, struct form_control *control)
{
	if (form == NULL) {
		return;
	}

	control->form = form;

	if (form->controls != NULL) {
		assert(form->last_control);

		form->last_control->next = control;
		control->prev = form->last_control;
		control->next = NULL;
		form->last_control = control;
	} else {
		form->controls = form->last_control = control;
	}
}


/* exported interface documented in html/form_internal.h */
void form_free_control(struct form_control *control)
{
	struct form_control *c;
	assert(control != NULL);

	NSLOG(netsurf, INFO, "Control:%p name:%p value:%p initial:%p",
	      control, control->name, control->value, control->initial_value);
	free(control->name);
	free(control->value);
	free(control->initial_value);
	if (control->last_synced_value != NULL) {
		free(control->last_synced_value);
	}

	if (control->type == GADGET_SELECT) {
		struct form_option *option, *next;

		for (option = control->data.select.items; option;
				option = next) {
			next = option->next;
			NSLOG(netsurf, INFO,
			      "select option:%p text:%p value:%p", option,
			      option->text, option->value);
			free(option->text);
			free(option->value);
			free(option);
		}
		if (control->data.select.menu != NULL) {
			form_free_select_menu(control);
		}
	}

	if (control->type == GADGET_TEXTAREA ||
			control->type == GADGET_TEXTBOX ||
			control->type == GADGET_PASSWORD) {

		if (control->data.text.initial != NULL) {
			dom_string_unref(control->data.text.initial);
		}

		if (control->data.text.ta != NULL) {
			textarea_destroy(control->data.text.ta);
		}
	}

	/* unlink the control from the form */
	if (control->form != NULL) {
		for (c = control->form->controls; c != NULL; c = c->next) {
			if (c->next == control) {
				c->next = control->next;
				if (control->form->last_control == control)
					control->form->last_control = c;
				break;
			}
			if (c == control) {
				/* can only happen if control was first control */
				control->form->controls = control->next;
				if (control->form->last_control == control)
					control->form->controls =
						control->form->last_control = NULL;
				break;
			}
		}
	}

	if (control->node_value != NULL) {
		dom_string_unref(control->node_value);
	}

	free(control);
}


/* exported interface documented in html/form_internal.h */
bool form_add_option(struct form_control *control, char *value, char *text,
		     bool selected, void *node)
{
	struct form_option *option;

	assert(control);
	assert(control->type == GADGET_SELECT);

	option = calloc(1, sizeof *option);
	if (!option)
		return false;

	option->value = value;
	option->text = text;

	/* add to linked list */
	if (control->data.select.items == 0)
		control->data.select.items = option;
	else
		control->data.select.last_item->next = option;
	control->data.select.last_item = option;

	/* set selected */
	if (selected && (control->data.select.num_selected == 0 ||
			control->data.select.multiple)) {
		option->selected = option->initial_selected = true;
		control->data.select.num_selected++;
		control->data.select.current = option;
	}

	control->data.select.num_items++;

	option->node = node;

	return true;
}


/* exported interface documented in html/form_internal.h */
nserror
form_open_select_menu(void *client_data,
		      struct form_control *control,
		      select_menu_redraw_callback callback,
		      struct content *c)
{
	int line_height_with_spacing;
	struct box *box;
	plot_font_style_t fstyle;
	int total_height;
	struct form_select_menu *menu;
	html_content *html = (html_content *)c;
	nserror res;

	/* if the menu is opened for the first time */
	if (control->data.select.menu == NULL) {

		menu = calloc(1, sizeof (struct form_select_menu));
		if (menu == NULL) {
			return NSERROR_NOMEM;
		}

		control->data.select.menu = menu;

		box = control->box;

		menu->width = box->width +
			box->border[RIGHT].width + box->padding[RIGHT] +
			box->border[LEFT].width + box->padding[LEFT];

		font_plot_style_from_css(&html->unit_len_ctx,
				control->box->style, &fstyle);
		menu->f_size = fstyle.size;

		menu->line_height = FIXTOINT(FDIV((FMUL(FLTTOFIX(1.2),
				FMUL(html->unit_len_ctx.device_dpi,
				INTTOFIX(fstyle.size / PLOT_STYLE_SCALE)))),
				F_72));

		line_height_with_spacing = menu->line_height +
				menu->line_height *
				SELECT_LINE_SPACING;

		total_height = control->data.select.num_items *
				line_height_with_spacing;
		menu->height = total_height;

		if (menu->height > MAX_SELECT_HEIGHT) {
			menu->height = MAX_SELECT_HEIGHT;
		}

		menu->client_data = client_data;
		menu->callback = callback;
		res = scrollbar_create(false,
				       menu->height,
				       total_height,
				       menu->height,
				       control,
				       form_select_menu_scroll_callback,
				       &(menu->scrollbar));
		if (res != NSERROR_OK) {
			control->data.select.menu = NULL;
			free(menu);
			return res;
		}
		menu->c = c;
	} else {
		menu = control->data.select.menu;
	}

	menu->callback(client_data, 0, 0, menu->width, menu->height);

	return NSERROR_OK;
}


/* exported interface documented in html/form_internal.h */
void form_free_select_menu(struct form_control *control)
{
	if (control->data.select.menu->scrollbar != NULL)
		scrollbar_destroy(control->data.select.menu->scrollbar);
	free(control->data.select.menu);
	control->data.select.menu = NULL;
}


/* exported interface documented in html/form_internal.h */
bool
form_redraw_select_menu(struct form_control *control,
			int x, int y,
			float scale,
			const struct rect *clip,
			const struct redraw_context *ctx)
{
	struct box *box;
	struct form_select_menu *menu = control->data.select.menu;
	struct form_option *option;
	int line_height, line_height_with_spacing;
	int width, height;
	int x0, y0, x1, scrollbar_x, y1, y2, y3;
	int item_y;
	int text_pos_offset, text_x;
	int scrollbar_width = SCROLLBAR_WIDTH;
	int i;
	int scroll;
	int x_cp, y_cp;
	struct rect r;
	struct rect rect;
	nserror res;

	box = control->box;

	x_cp = x;
	y_cp = y;
	width = menu->width;
	height = menu->height;
	line_height = menu->line_height;

	line_height_with_spacing = line_height +
			line_height * SELECT_LINE_SPACING;
	scroll = scrollbar_get_offset(menu->scrollbar);

	if (scale != 1.0) {
		x *= scale;
		y *= scale;
		width *= scale;
		height *= scale;
		scrollbar_width *= scale;

		i = scroll / line_height_with_spacing;
		scroll -= i * line_height_with_spacing;
		line_height *= scale;
		line_height_with_spacing *= scale;
		scroll *= scale;
		scroll += i * line_height_with_spacing;
	}


	x0 = x;
	y0 = y;
	x1 = x + width - 1;
	y1 = y + height - 1;
	scrollbar_x = x1 - scrollbar_width;

	r.x0 = x0;
	r.y0 = y0;
	r.x1 = x1 + 1;
	r.y1 = y1 + 1;
	res = ctx->plot->clip(ctx, &r);
	if (res != NSERROR_OK) {
		return false;
	}

	rect.x0 = x0;
	rect.y0 = y0;
	rect.x1 = x1;
	rect.y1 = y1;
	res = ctx->plot->rectangle(ctx, plot_style_stroke_darkwbasec, &rect);
	if (res != NSERROR_OK) {
		return false;
	}

	x0 = x0 + SELECT_BORDER_WIDTH;
	y0 = y0 + SELECT_BORDER_WIDTH;
	x1 = x1 - SELECT_BORDER_WIDTH;
	y1 = y1 - SELECT_BORDER_WIDTH;
	height = height - 2 * SELECT_BORDER_WIDTH;

	r.x0 = x0;
	r.y0 = y0;
	r.x1 = x1 + 1;
	r.y1 = y1 + 1;
	res = ctx->plot->clip(ctx, &r);
	if (res != NSERROR_OK) {
		return false;
	}

	res = ctx->plot->rectangle(ctx, plot_style_fill_lightwbasec, &r);
	if (res != NSERROR_OK) {
		return false;
	}

	option = control->data.select.items;
	item_y = line_height_with_spacing;

	while (item_y < scroll) {
		option = option->next;
		item_y += line_height_with_spacing;
	}
	item_y -= line_height_with_spacing;
	text_pos_offset = y - scroll +
			(int) (line_height * (0.75 + SELECT_LINE_SPACING));
	text_x = x + (box->border[LEFT].width + box->padding[LEFT]) * scale;

	plot_fstyle_entry.size = menu->f_size;

	while (option && item_y - scroll < height) {

		if (option->selected) {
			y2 = y + item_y - scroll;
			y3 = y + item_y + line_height_with_spacing - scroll;

			rect.x0 = x0;
			rect.y0 = y0 > y2 ? y0 : y2;
			rect.x1 = scrollbar_x + 1;
			rect.y1 = y3 < y1 + 1 ? y3 : y1 + 1;
			res = ctx->plot->rectangle(ctx, &plot_style_fill_selected, &rect);
			if (res != NSERROR_OK) {
				return false;
			}
		}

		y2 = text_pos_offset + item_y;
		res = ctx->plot->text(ctx,
				      &plot_fstyle_entry,
				      text_x, y2,
				      option->text, strlen(option->text));
		if (res != NSERROR_OK) {
			return false;
		}

		item_y += line_height_with_spacing;
		option = option->next;
	}

	res = scrollbar_redraw(menu->scrollbar,
			       x_cp + menu->width - SCROLLBAR_WIDTH,
			       y_cp,
			       clip, scale, ctx);
	if (res != NSERROR_OK) {
		return false;
	}

	return true;
}


/* private interface described in html/form_internal.h */
bool
form_clip_inside_select_menu(struct form_control *control,
			     float scale,
			     const struct rect *clip)
{
	struct form_select_menu *menu = control->data.select.menu;
	int width, height;


	width = menu->width;
	height = menu->height;

	if (scale != 1.0) {
		width *= scale;
		height *= scale;
	}

	if (clip->x0 >= 0 &&
	    clip->x1 <= width &&
	    clip->y0 >= 0 &&
	    clip->y1 <= height)
		return true;

	return false;
}


/* exported interface documented in netsurf/form.h */
nserror form_select_process_selection(struct form_control *control, int item)
{
	assert(control != NULL);

	return form__select_process_selection(control->html, control, item);
}


/* exported interface documented in netsurf/form.h */
struct form_option *
form_select_get_option(struct form_control *control, int item)
{
	struct form_option *opt;

	opt = control->data.select.items;
	while ((opt != NULL) && (item > 0)) {
		opt = opt->next;
		item--;
	}
	return opt;
}


/* exported interface documented in netsurf/form.h */
char *form_control_get_name(struct form_control *control)
{
	return control->name;
}


/* exported interface documented in netsurf/form.h */
nserror form_control_bounding_rect(struct form_control *control, struct rect *r)
{
	box_bounds( control->box, r );
	return NSERROR_OK;
}


/* private interface described in html/form_internal.h */
const char *
form_select_mouse_action(struct form_control *control,
			 browser_mouse_state mouse,
			 int x, int y)
{
	struct form_select_menu *menu = control->data.select.menu;
	int x0, y0, x1, y1, scrollbar_x;
	const char *status = NULL;
	bool multiple = control->data.select.multiple;

	x0 = 0;
	y0 = 0;
	x1 = menu->width;
	y1 = menu->height;
	scrollbar_x = x1 - SCROLLBAR_WIDTH;

	if (menu->scroll_capture ||
			(x > scrollbar_x && x < x1 && y > y0 && y < y1)) {
		/* The scroll is currently capturing all events or the mouse
		 * event is taking place on the scrollbar widget area
		 */
		x -= scrollbar_x;
		return scrollbar_mouse_status_to_message(
				scrollbar_mouse_action(menu->scrollbar,
						mouse, x, y));
	}


	if (x > x0 && x < scrollbar_x && y > y0 && y < y1) {
		/* over option area */

		if (mouse & (BROWSER_MOUSE_CLICK_1 | BROWSER_MOUSE_CLICK_2))
			/* button 1 or 2 click */
			form_select_menu_clicked(control, x, y);

		if (!(mouse & BROWSER_MOUSE_CLICK_1 && !multiple))
			/* anything but a button 1 click over a single select
			   menu */
			status = messages_get(control->data.select.multiple ?
					"SelectMClick" : "SelectClick");

	} else if (!(mouse & (BROWSER_MOUSE_CLICK_1 | BROWSER_MOUSE_CLICK_2)))
		/* if not a button 1 or 2 click*/
		status = messages_get("SelectClose");

	return status;
}


/* private interface described in html/form_internal.h */
void
form_select_mouse_drag_end(struct form_control *control,
			   browser_mouse_state mouse,
			   int x, int y)
{
	int x0, y0, x1, y1;
	int box_x, box_y;
	struct box *box;
	struct form_select_menu *menu = control->data.select.menu;

	box = control->box;

	/* Get global coords of scrollbar */
	box_coords(box, &box_x, &box_y);
	box_x -= box->border[LEFT].width;
	box_y += box->height + box->border[BOTTOM].width +
			box->padding[BOTTOM] + box->padding[TOP];

	/* Get drag end coords relative to scrollbar */
	x = x - box_x;
	y = y - box_y;

	if (menu->scroll_capture) {
		x -= menu->width - SCROLLBAR_WIDTH;
		scrollbar_mouse_drag_end(menu->scrollbar, mouse, x, y);
		return;
	}

	x0 = 0;
	y0 = 0;
	x1 = menu->width;
	y1 = menu->height;


	if (x > x0 && x < x1 - SCROLLBAR_WIDTH && y >  y0 && y < y1) {
		/* handle drag end above the option area like a regular click */
		form_select_menu_clicked(control, x, y);
	}
}


/* private interface described in html/form_internal.h */
void form_select_get_dimensions(struct form_control *control,
		int *width, int *height)
{
	*width = control->data.select.menu->width;
	*height = control->data.select.menu->height;
}


/* private interface described in html/form_internal.h */
void form_select_menu_callback(void *client_data,
		int x, int y, int width, int height)
{
	html_content *html = client_data;
	int menu_x, menu_y;
	struct box *box;

	box = html->visible_select_menu->box;
	box_coords(box, &menu_x, &menu_y);

	menu_x -= box->border[LEFT].width;
	menu_y += box->height + box->border[BOTTOM].width +
			box->padding[BOTTOM] +
			box->padding[TOP];
	content__request_redraw((struct content *)html, menu_x + x, menu_y + y,
			width, height);
}


/* private interface described in html/form_internal.h */
void form_radio_set(struct form_control *radio)
{
	struct form_control *control;

	assert(radio);
	if (!radio->form)
		return;

	if (radio->selected)
		return;

	/* Clear selected state for other controls in
	 * the same radio button group */
	for (control = radio->form->controls;
	     control != NULL;
	     control = control->next) {
		/* Only interested in radio inputs */
		if (control->type != GADGET_RADIO)
			continue;

		/* Ignore ourself */
		if (control == radio)
			continue;

		/* Ignore inputs where:
		 *   a) this or the other control have no name attribute
		 *   b) this or the other control have an empty name attribute
		 *   c) the control names do not match
		 */
		if ((control->name == NULL) ||
		    (radio->name == NULL) ||
		    (control->name[0] == '\0') ||
		    (radio->name[0] == '\0') ||
		    strcmp(control->name, radio->name) != 0)
			continue;

		/* Other control is in the same radio button group: clear its
		 * selected state */
		if (control->selected) {
			control->selected = false;
			dom_html_input_element_set_checked(control->node, false);
			html__redraw_a_box(radio->html, control->box);
		}
	}

	radio->selected = true;
	dom_html_input_element_set_checked(radio->node, true);
	html__redraw_a_box(radio->html, radio->box);
}


/* private interface described in html/form_internal.h */
nserror
form_submit(nsurl *page_url,
	    struct browser_window *target,
	    struct form *form,
	    struct form_control *submit_button)
{
	nserror res;
	char *data = NULL; /* encoded form data */
	struct fetch_multipart_data *success = NULL; /* gcc is incapable of correctly reasoning about use and generates "maybe used uninitialised" warnings */
	nsurl *action_url;
	nsurl *query_url;

	assert(form != NULL);

	/* obtain list of controls from DOM */
	res = form_dom_to_data(form, submit_button, &success);
	if (res != NSERROR_OK) {
		return res;
	}

	/* Decompose action */
	res = nsurl_create(form->action, &action_url);
	if (res != NSERROR_OK) {
		fetch_multipart_data_destroy(success);
		return res;
	}

	switch (form->method) {
	case method_GET:
		res = form_url_encode(form, success, &data);
		if (res == NSERROR_OK) {
			/* Replace query segment */
			res = nsurl_replace_query(action_url, data, &query_url);
			if (res == NSERROR_OK) {
				res = browser_window_navigate(target,
							      query_url,
							      page_url,
							      BW_NAVIGATE_HISTORY,
							      NULL,
							      NULL,
							      NULL);

				nsurl_unref(query_url);
			}
			free(data);
		}
		break;

	case method_POST_URLENC:
		res = form_url_encode(form, success, &data);
		if (res == NSERROR_OK) {
			res = browser_window_navigate(target,
						      action_url,
						      page_url,
						      BW_NAVIGATE_HISTORY,
						      data,
						      NULL,
						      NULL);
			free(data);
		}
		break;

	case method_POST_MULTIPART:
		res = browser_window_navigate(target,
					      action_url,
					      page_url,
					      BW_NAVIGATE_HISTORY,
					      NULL,
					      success,
					      NULL);

		break;
	}

	nsurl_unref(action_url);
	fetch_multipart_data_destroy(success);

	return res;
}


/* exported interface documented in html/form_internal.h */
void form_gadget_update_value(struct form_control *control, char *value)
{
	switch (control->type) {
	case GADGET_HIDDEN:
	case GADGET_TEXTBOX:
	case GADGET_TEXTAREA:
	case GADGET_PASSWORD:
	case GADGET_FILE:
		if (control->value != NULL) {
			free(control->value);
		}
		control->value = value;
		if (control->node != NULL) {
			dom_exception err;
			dom_string *str;
			err = dom_string_create((uint8_t *)value,
						strlen(value), &str);
			if (err == DOM_NO_ERR) {
				if (control->type == GADGET_TEXTAREA)
					err = dom_html_text_area_element_set_value(
						(dom_html_text_area_element *)(control->node),
						str);
				else
					err = dom_html_input_element_set_value(
						(dom_html_input_element *)(control->node),
						str);
				dom_string_unref(str);
			}
		}
		break;
	default:
		/* Do nothing */
		break;
	}

	/* Finally, sync this with the DOM */
	form_gadget_sync_with_dom(control);
}


/* Exported API, see html/form_internal.h */
void
form_gadget_sync_with_dom(struct form_control *control)
{
	dom_exception exc;
	dom_string *value = NULL;
	bool changed_dom = false;

	if (control->syncing ||
	    (control->type != GADGET_TEXTBOX &&
	     control->type != GADGET_PASSWORD &&
	     control->type != GADGET_HIDDEN &&
	     control->type != GADGET_TEXTAREA)) {
		/* Not a control we support, or the control is already
		 * mid-sync so we don't want to disrupt that
		 */
		return;
	}

	control->syncing = true;

	/* If we've changed value, sync that toward the DOM */
	if ((control->last_synced_value == NULL &&
	     control->value != NULL &&
	     control->value[0] != '\0') ||
	    (control->last_synced_value != NULL &&
	     control->value != NULL &&
	     strcmp(control->value, control->last_synced_value) != 0)) {
		char *dup = strdup(control->value);
		if (dup == NULL) {
			goto out;
		}
		if (control->last_synced_value != NULL) {
			free(control->last_synced_value);
		}
		control->last_synced_value = dup;
		exc = dom_string_create((uint8_t *)(control->value),
					strlen(control->value), &value);
		if (exc != DOM_NO_ERR) {
			goto out;
		}
		if (control->node_value != NULL) {
			dom_string_unref(control->node_value);
		}
		control->node_value = value;
		value = NULL;
		if (control->type == GADGET_TEXTAREA) {
			exc = dom_html_text_area_element_set_value(control->node, control->node_value);
		} else {
			exc = dom_html_input_element_set_value(control->node, control->node_value);
		}
		if (exc != DOM_NO_ERR) {
			goto out;
		}
		changed_dom = true;
	}

	/* Now check if the DOM has changed since our last go */
	if (control->type == GADGET_TEXTAREA) {
		exc = dom_html_text_area_element_get_value(control->node, &value);
	} else {
		exc = dom_html_input_element_get_value(control->node, &value);
	}

	if (exc != DOM_NO_ERR) {
		/* Nothing much we can do here */
		goto out;
	}

	if (!dom_string_isequal(control->node_value, value)) {
		/* The DOM has changed */
		if (!changed_dom) {
			/* And it wasn't us */
			char *value_s = strndup(
				dom_string_data(value),
				dom_string_byte_length(value));
			char *dup = NULL;
			if (value_s == NULL) {
				goto out;
			}
			dup = strdup(value_s);
			if (dup == NULL) {
				free(value_s);
				goto out;
			}
			free(control->value);
			control->value = value_s;
			free(control->last_synced_value);
			control->last_synced_value = dup;
			if (control->type != GADGET_HIDDEN &&
			    control->data.text.ta != NULL) {
				textarea_set_text(control->data.text.ta,
						  value_s);
			}
		}
		control->node_value = value;
		value = NULL;
	}

out:
	if (value != NULL)
		dom_string_unref(value);
	control->syncing = false;
}


/* exported interface documented in html/form_internal.h */
struct form *
form_new(void *node,
	 const char *action,
	 const char *target,
	 form_method method,
	 const char *charset,
	 const char *doc_charset)
{
	struct form *form;

	form = calloc(1, sizeof *form);
	if (!form)
		return NULL;

	form->action = strdup(action != NULL ? action : "");
	if (form->action == NULL) {
		free(form);
		return NULL;
	}

	form->target = target != NULL ? strdup(target) : NULL;
	if (target != NULL && form->target == NULL) {
		free(form->action);
		free(form);
		return NULL;
	}

	form->method = method;

	form->accept_charsets = charset != NULL ? strdup(charset) : NULL;
	if (charset != NULL && form->accept_charsets == NULL) {
		free(form->target);
		free(form->action);
		free(form);
		return NULL;
	}

	form->document_charset = doc_charset != NULL ? strdup(doc_charset)
						     : NULL;
	if (doc_charset && form->document_charset == NULL) {
		free(form->accept_charsets);
		free(form->target);
		free(form->action);
		free(form);
		return NULL;
	}

	form->node = node;

	return form;
}


/* exported interface documented in html/form_internal.h */
void form_free(struct form *form)
{
	struct form_control *c, *d;

	for (c = form->controls; c != NULL; c = d) {
		d = c->next;

		form_free_control(c);
	}

	free(form->action);
	free(form->target);
	free(form->accept_charsets);
	free(form->document_charset);

	free(form);
}


/* exported interface documented in html/form_internal.h */
struct form_control *form_new_control(void *node, form_control_type type)
{
	struct form_control *control;

	control = calloc(1, sizeof *control);
	if (control == NULL)
		return NULL;

	control->node = node;
	control->type = type;

	return control;
}
