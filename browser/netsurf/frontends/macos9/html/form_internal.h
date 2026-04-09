/*
 * MacSurf stub -- html/form_internal.h
 * Minimal C89-compatible stub for CodeWarrior 8 compilation.
 * Licensed under GPL v2.
 */

#ifndef NETSURF_HTML_FORM_INTERNAL_H
#define NETSURF_HTML_FORM_INTERNAL_H

struct form;
struct box;

typedef enum {
	GADGET_HIDDEN = 0,
	GADGET_TEXTBOX,
	GADGET_RADIO,
	GADGET_CHECKBOX,
	GADGET_SELECT,
	GADGET_TEXTAREA,
	GADGET_IMAGE,
	GADGET_PASSWORD,
	GADGET_SUBMIT,
	GADGET_RESET,
	GADGET_FILE,
	GADGET_BUTTON
} form_control_type;

/* Minimal struct definition — only fields accessed by browser_window.c */
struct form_control {
	void *node;
	form_control_type type;
	struct form *form;
	char *name;
	char *value;
};

#endif
