/*
 * macos9_svg_inline.c -- fixes195 inline SVG renderer (V1).
 *
 * Walks an inline <svg> DOM subtree and paints shapes through the
 * NetSurf plotter table. See macos9_svg_inline.h for full coverage
 * notes.
 *
 * Architecture overview:
 *
 *   html_redraw_box (redraw.c) sees box->flags & SVG_INLINE and
 *   dispatches here with (x, y, w, h) = the box's content rect.
 *
 *   svg__paint_root() reads viewBox + width/height attrs, builds a
 *   svg_ctx { box rect, scale factors, current fill/stroke,
 *   plotter table }, then walks the SVG element's children with
 *   svg__paint_subtree(). <g> children recurse the walker with
 *   merged style; shape elements paint and don't recurse.
 *
 *   Each shape emits one or more plotter calls. Coordinates are
 *   transformed from SVG space to screen space by svg__map_x /
 *   svg__map_y.
 *
 * The path mini-language is parsed by svg__path_emit() which feeds
 * MOVE/LINE/BEZIER/CLOSE tokens into a per-path float buffer (kept
 * on the stack with a fixed cap so the parser doesn't allocate on
 * the per-element heap). The buffer is then handed to
 * ctx->plot->path().
 */

#include "frontends/macos9/macos9.h"
#include "frontends/macos9/macos9_svg_inline.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <dom/dom.h>

#include "utils/corestrings.h"
#include "utils/errors.h"
#include "netsurf/plotters.h"
#include "netsurf/plot_style.h"
#include "content/handlers/html/box.h"

/* ----------------------------------------------------------------- */
/* Constants                                                         */
/* ----------------------------------------------------------------- */

/* Maximum coordinates in one <path> primitive. Each PLOTTER_PATH_*
 * token uses 1..7 floats (opcode + up to 6 control values). For
 * complex paths we may exceed this; the emitter caps at the buffer
 * and silently truncates. 1024 floats is enough for ~150 cubic
 * segments which covers the typical SVG icon size class. */
#define MACOS9_SVG_PATH_MAX 1024

/* Maximum nesting depth for <g> recursion. SVG icons rarely exceed
 * 3-4 levels; 16 is a generous safety cap. */
#define MACOS9_SVG_GROUP_MAX 16

/* Bezier subdivision for ellipse approximation. SVG circles and
 * ellipses become 4 cubic Bezier arcs. The plotter then samples
 * each Bezier with 8 line segments (see plotters.c plot_path
 * BEZIER handler). */
#define KAPPA 0.5522847498307933f


/* ----------------------------------------------------------------- */
/* Painter state                                                     */
/* ----------------------------------------------------------------- */

struct svg_paint_state {
	/* current fill / stroke colours (NetSurf colour format:
	 * 0xAABBGGRR with top byte 0 = opaque, 0x01 = transparent). */
	colour fill;
	colour stroke;
	/* stroke width in SVG units (no scale applied; the plotter
	 * gets the scaled width via pstyle->stroke_width). */
	float stroke_width;
	/* whether fill / stroke are explicitly set (vs inherited
	 * default).  When unset, fill defaults to black and stroke to
	 * none, per SVG. */
	int fill_present;
	int stroke_present;
};

struct svg_ctx {
	/* box rect in screen coords. */
	int box_x;
	int box_y;
	int box_w;
	int box_h;
	/* SVG viewBox. */
	float vb_x;
	float vb_y;
	float vb_w;
	float vb_h;
	/* derived scale factors: screen_dx = (svg_x - vb_x) * scale_x. */
	float scale_x;
	float scale_y;
	/* plotter table. */
	const struct redraw_context *plot_ctx;
};


/* ----------------------------------------------------------------- */
/* Number / colour parsers                                           */
/* ----------------------------------------------------------------- */

/* Locale-independent positive float scan. Stops at the first non-
 * numeric byte. Consumed bytes are written to *consumed when not
 * NULL. Handles leading '+' / '-', optional fractional part, no
 * scientific notation (SVG paths almost never use it; ignoring keeps
 * the parser tight for CW8). */
static float svg__atof(const char *s, size_t *consumed)
{
	float v;
	int sign;
	int saw_digit;
	size_t i;

	if (s == NULL) {
		if (consumed != NULL) *consumed = 0;
		return 0.0f;
	}

	v = 0.0f;
	sign = 1;
	saw_digit = 0;
	i = 0;
	if (s[i] == '+') i++;
	else if (s[i] == '-') { sign = -1; i++; }
	while (s[i] >= '0' && s[i] <= '9') {
		v = v * 10.0f + (float)(s[i] - '0');
		saw_digit = 1;
		i++;
	}
	if (s[i] == '.') {
		float frac = 0.0f;
		float div = 1.0f;
		i++;
		while (s[i] >= '0' && s[i] <= '9') {
			frac = frac * 10.0f + (float)(s[i] - '0');
			div *= 10.0f;
			saw_digit = 1;
			i++;
		}
		v += frac / div;
	}
	if (s[i] == 'e' || s[i] == 'E') {
		int esign = 1;
		int exp = 0;
		i++;
		if (s[i] == '+') i++;
		else if (s[i] == '-') { esign = -1; i++; }
		while (s[i] >= '0' && s[i] <= '9') {
			exp = exp * 10 + (s[i] - '0');
			i++;
		}
		/* simple integer-exponent scale */
		while (exp > 0) {
			if (esign > 0) v *= 10.0f;
			else v /= 10.0f;
			exp--;
		}
	}
	if (consumed != NULL) {
		*consumed = saw_digit ? i : 0;
	}
	return sign * v;
}

static int svg__hex_nibble(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
	if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
	return -1;
}

/* Parse a CSS-ish colour into NetSurf colour (0xAABBGGRR). Returns
 * 1 on success, 0 on failure. "none" produces colour=0 and the
 * caller-supplied none_flag=1. */
struct svg__named { const char *name; colour col; };

static int svg__parse_colour(const char *s, colour *out, int *none_flag)
{
	static const struct svg__named NAMED[] = {
		/* CSS Level 1 colours, ample for icon use. */
		{ "black",   0xFF000000 },
		{ "silver",  0xFFC0C0C0 },
		{ "gray",    0xFF808080 },
		{ "grey",    0xFF808080 },
		{ "white",   0xFFFFFFFF },
		{ "maroon",  0xFF000080 },
		{ "red",     0xFF0000FF },
		{ "purple",  0xFF800080 },
		{ "fuchsia", 0xFFFF00FF },
		{ "magenta", 0xFFFF00FF },
		{ "green",   0xFF008000 },
		{ "lime",    0xFF00FF00 },
		{ "olive",   0xFF008080 },
		{ "yellow",  0xFF00FFFF },
		{ "navy",    0xFF800000 },
		{ "blue",    0xFFFF0000 },
		{ "teal",    0xFF808000 },
		{ "aqua",    0xFFFFFF00 },
		{ "cyan",    0xFFFFFF00 },
		{ "orange",  0xFF00A5FF },
		{ "transparent", 0x01000000 }
	};
	static const size_t N_NAMED = sizeof(NAMED) / sizeof(NAMED[0]);
	size_t i;
	size_t len;

	if (none_flag != NULL) *none_flag = 0;
	if (out == NULL || s == NULL) return 0;

	/* skip leading whitespace */
	while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;

	if (s[0] == '#') {
		const char *p = s + 1;
		size_t hex_len = 0;
		int r, g, b;
		while ((p[hex_len] >= '0' && p[hex_len] <= '9') ||
				(p[hex_len] >= 'a' && p[hex_len] <= 'f') ||
				(p[hex_len] >= 'A' && p[hex_len] <= 'F')) {
			hex_len++;
			if (hex_len > 8) break;
		}
		if (hex_len == 3) {
			r = svg__hex_nibble(p[0]); r = (r << 4) | r;
			g = svg__hex_nibble(p[1]); g = (g << 4) | g;
			b = svg__hex_nibble(p[2]); b = (b << 4) | b;
		} else if (hex_len == 6) {
			r = (svg__hex_nibble(p[0]) << 4) | svg__hex_nibble(p[1]);
			g = (svg__hex_nibble(p[2]) << 4) | svg__hex_nibble(p[3]);
			b = (svg__hex_nibble(p[4]) << 4) | svg__hex_nibble(p[5]);
		} else {
			return 0;
		}
		*out = (colour)((0xFF << 24) |
				((b & 0xFF) << 16) |
				((g & 0xFF) << 8) |
				(r & 0xFF));
		return 1;
	}

	if (s[0] == 'r' && s[1] == 'g' && s[2] == 'b' && s[3] == '(') {
		const char *p = s + 4;
		float c[3];
		int k;
		size_t consumed;
		for (k = 0; k < 3; k++) {
			while (*p == ' ' || *p == ',') p++;
			c[k] = svg__atof(p, &consumed);
			if (consumed == 0) return 0;
			p += consumed;
			if (*p == '%') {
				c[k] = c[k] * 255.0f / 100.0f;
				p++;
			}
		}
		*out = (colour)((0xFF << 24) |
				(((int)c[2] & 0xFF) << 16) |
				(((int)c[1] & 0xFF) << 8) |
				((int)c[0] & 0xFF));
		return 1;
	}

	if (s[0] == 'n' && s[1] == 'o' && s[2] == 'n' && s[3] == 'e') {
		if (none_flag != NULL) *none_flag = 1;
		*out = 0;
		return 1;
	}

	/* url(#...) gradient reference -- not supported in V1.
	 * Fall back to black so the shape is at least visible. */
	if (s[0] == 'u' && s[1] == 'r' && s[2] == 'l' && s[3] == '(') {
		*out = 0xFF000000;
		return 1;
	}

	/* Named colour. */
	len = 0;
	while (s[len] != '\0' && s[len] != ' ' && s[len] != ';' &&
			s[len] != ',' && s[len] != '\t' &&
			s[len] != '\n' && s[len] != '\r') len++;
	for (i = 0; i < N_NAMED; i++) {
		const char *n = NAMED[i].name;
		size_t nl = strlen(n);
		size_t j;
		if (nl != len) continue;
		for (j = 0; j < nl; j++) {
			char a = s[j];
			char b = n[j];
			if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
			if (a != b) break;
		}
		if (j == nl) {
			*out = NAMED[i].col;
			return 1;
		}
	}
	return 0;
}


/* ----------------------------------------------------------------- */
/* DOM attribute helpers                                             */
/* ----------------------------------------------------------------- */

/* Read an attribute as a C string. Returns NULL if missing. Caller
 * must dom_string_unref the returned dom_string via the secondary
 * out param when done. */
static const char *svg__attr(dom_node *node, const char *name,
		dom_string **out_dom)
{
	dom_string *ds_name = NULL;
	dom_string *ds_val = NULL;
	dom_exception err;

	*out_dom = NULL;
	if (dom_string_create((const uint8_t *)name, strlen(name),
			&ds_name) != DOM_NO_ERR || ds_name == NULL) {
		return NULL;
	}
	err = dom_element_get_attribute(node, ds_name, &ds_val);
	dom_string_unref(ds_name);
	if (err != DOM_NO_ERR || ds_val == NULL) {
		return NULL;
	}
	*out_dom = ds_val;
	return (const char *)dom_string_data(ds_val);
}

static float svg__attr_float(dom_node *node, const char *name,
		float fallback)
{
	dom_string *ds = NULL;
	const char *s = svg__attr(node, name, &ds);
	float v;
	size_t consumed;
	if (s == NULL) return fallback;
	v = svg__atof(s, &consumed);
	dom_string_unref(ds);
	return (consumed == 0) ? fallback : v;
}


/* ----------------------------------------------------------------- */
/* Coordinate transform                                              */
/* ----------------------------------------------------------------- */

static float svg__map_x(const struct svg_ctx *c, float sx)
{
	return (float)c->box_x + (sx - c->vb_x) * c->scale_x;
}

static float svg__map_y(const struct svg_ctx *c, float sy)
{
	return (float)c->box_y + (sy - c->vb_y) * c->scale_y;
}


/* ----------------------------------------------------------------- */
/* Style merge                                                       */
/* ----------------------------------------------------------------- */

/* Read fill / stroke / stroke-width attributes on a single element
 * and merge into the supplied paint state. Inherited values stay
 * unchanged when an attribute is absent. The `style="..."` attribute
 * is parsed as a flat key:value list for fill/stroke/stroke-width
 * but does not recurse into CSS shorthand. */
static void svg__update_style(dom_node *node, struct svg_paint_state *st)
{
	dom_string *ds = NULL;
	const char *v;
	colour col;
	int none;

	/* fill attribute */
	v = svg__attr(node, "fill", &ds);
	if (v != NULL) {
		if (svg__parse_colour(v, &col, &none)) {
			st->fill = col;
			st->fill_present = none ? 0 : 1;
		}
		dom_string_unref(ds);
	}

	/* stroke attribute */
	v = svg__attr(node, "stroke", &ds);
	if (v != NULL) {
		if (svg__parse_colour(v, &col, &none)) {
			st->stroke = col;
			st->stroke_present = none ? 0 : 1;
		}
		dom_string_unref(ds);
	}

	/* stroke-width attribute */
	v = svg__attr(node, "stroke-width", &ds);
	if (v != NULL) {
		size_t consumed;
		float w = svg__atof(v, &consumed);
		if (consumed > 0 && w > 0.0f) st->stroke_width = w;
		dom_string_unref(ds);
	}

	/* style="fill:X; stroke:Y; stroke-width:Z" minimal parse */
	v = svg__attr(node, "style", &ds);
	if (v != NULL) {
		const char *p = v;
		while (*p) {
			const char *key_start;
			const char *key_end;
			const char *val_start;
			const char *val_end;
			while (*p == ' ' || *p == ';' || *p == '\t') p++;
			if (*p == '\0') break;
			key_start = p;
			while (*p && *p != ':' && *p != ';') p++;
			key_end = p;
			if (*p != ':') continue;
			p++;
			while (*p == ' ' || *p == '\t') p++;
			val_start = p;
			while (*p && *p != ';') p++;
			val_end = p;
			/* dispatch on key */
			{
				size_t kl = (size_t)(key_end - key_start);
				if (kl == 4 && strncmp(key_start, "fill", 4) == 0) {
					if (svg__parse_colour(val_start, &col,
							&none)) {
						st->fill = col;
						st->fill_present = none ? 0 : 1;
					}
				} else if (kl == 6 &&
						strncmp(key_start, "stroke", 6) == 0) {
					if (svg__parse_colour(val_start, &col,
							&none)) {
						st->stroke = col;
						st->stroke_present = none ? 0 : 1;
					}
				} else if (kl == 12 &&
						strncmp(key_start,
							"stroke-width",
							12) == 0) {
					size_t consumed;
					float w = svg__atof(val_start,
							&consumed);
					if (consumed > 0 && w > 0.0f) {
						st->stroke_width = w;
					}
				}
			}
			(void)val_end;
		}
		dom_string_unref(ds);
	}
}


/* ----------------------------------------------------------------- */
/* Plot helpers                                                      */
/* ----------------------------------------------------------------- */

static void svg__init_plot_style(plot_style_t *p,
		const struct svg_paint_state *st,
		const struct svg_ctx *c)
{
	memset(p, 0, sizeof(*p));
	if (st->fill_present) {
		p->fill_type = PLOT_OP_TYPE_SOLID;
		p->fill_colour = st->fill;
	} else {
		p->fill_type = PLOT_OP_TYPE_NONE;
	}
	if (st->stroke_present && st->stroke_width > 0.0f) {
		float sw = st->stroke_width *
			(c->scale_x < c->scale_y ? c->scale_x : c->scale_y);
		p->stroke_type = PLOT_OP_TYPE_SOLID;
		p->stroke_colour = st->stroke;
		if (sw < 1.0f) sw = 1.0f;
		p->stroke_width = (plot_style_fixed)sw << PLOT_STYLE_RADIX;
	} else {
		p->stroke_type = PLOT_OP_TYPE_NONE;
	}
	p->opacity = (plot_style_fixed)1 << PLOT_STYLE_RADIX; /* opaque */
	/* transform_b identity, transform=0 (no transform). */
	p->transform_b = 0x01000100;
}


/* ----------------------------------------------------------------- */
/* Shape painters                                                    */
/* ----------------------------------------------------------------- */

static void svg__paint_rect(dom_node *node,
		const struct svg_ctx *c,
		const struct svg_paint_state *st)
{
	float x = svg__attr_float(node, "x", 0.0f);
	float y = svg__attr_float(node, "y", 0.0f);
	float w = svg__attr_float(node, "width", 0.0f);
	float h = svg__attr_float(node, "height", 0.0f);
	struct rect r;
	plot_style_t pstyle;

	if (w <= 0.0f || h <= 0.0f) return;

	svg__init_plot_style(&pstyle, st, c);
	r.x0 = (int)svg__map_x(c, x);
	r.y0 = (int)svg__map_y(c, y);
	r.x1 = (int)svg__map_x(c, x + w);
	r.y1 = (int)svg__map_y(c, y + h);
	c->plot_ctx->plot->rectangle(c->plot_ctx, &pstyle, &r);
}

static void svg__paint_line(dom_node *node,
		const struct svg_ctx *c,
		const struct svg_paint_state *st)
{
	float x1 = svg__attr_float(node, "x1", 0.0f);
	float y1 = svg__attr_float(node, "y1", 0.0f);
	float x2 = svg__attr_float(node, "x2", 0.0f);
	float y2 = svg__attr_float(node, "y2", 0.0f);
	struct rect r;
	plot_style_t pstyle;

	if (!st->stroke_present) return;
	svg__init_plot_style(&pstyle, st, c);
	r.x0 = (int)svg__map_x(c, x1);
	r.y0 = (int)svg__map_y(c, y1);
	r.x1 = (int)svg__map_x(c, x2);
	r.y1 = (int)svg__map_y(c, y2);
	c->plot_ctx->plot->line(c->plot_ctx, &pstyle, &r);
}

/* Emit a closed elliptical path centred on (cx, cy) with radii
 * (rx, ry) via 4 cubic Bezier segments. KAPPA is the standard
 * approximation constant. */
static int svg__emit_ellipse_path(float *out, int cap,
		float cx, float cy, float rx, float ry)
{
	float k_x;
	float k_y;
	int n;
	if (cap < 25) return 0;
	k_x = rx * KAPPA;
	k_y = ry * KAPPA;
	n = 0;
	out[n++] = (float)PLOTTER_PATH_MOVE;
	out[n++] = cx + rx; out[n++] = cy;
	/* right -> bottom */
	out[n++] = (float)PLOTTER_PATH_BEZIER;
	out[n++] = cx + rx; out[n++] = cy + k_y;
	out[n++] = cx + k_x; out[n++] = cy + ry;
	out[n++] = cx; out[n++] = cy + ry;
	/* bottom -> left */
	out[n++] = (float)PLOTTER_PATH_BEZIER;
	out[n++] = cx - k_x; out[n++] = cy + ry;
	out[n++] = cx - rx; out[n++] = cy + k_y;
	out[n++] = cx - rx; out[n++] = cy;
	/* left -> top */
	out[n++] = (float)PLOTTER_PATH_BEZIER;
	out[n++] = cx - rx; out[n++] = cy - k_y;
	out[n++] = cx - k_x; out[n++] = cy - ry;
	out[n++] = cx; out[n++] = cy - ry;
	/* top -> right */
	out[n++] = (float)PLOTTER_PATH_BEZIER;
	out[n++] = cx + k_x; out[n++] = cy - ry;
	out[n++] = cx + rx; out[n++] = cy - k_y;
	out[n++] = cx + rx; out[n++] = cy;
	out[n++] = (float)PLOTTER_PATH_CLOSE;
	return n;
}

static void svg__paint_ellipse_like(dom_node *node,
		const struct svg_ctx *c,
		const struct svg_paint_state *st,
		int as_circle)
{
	float cx = svg__attr_float(node, "cx", 0.0f);
	float cy = svg__attr_float(node, "cy", 0.0f);
	float rx, ry;
	float buf[32];
	int n;
	plot_style_t pstyle;
	int i;

	if (as_circle) {
		float r = svg__attr_float(node, "r", 0.0f);
		rx = r;
		ry = r;
	} else {
		rx = svg__attr_float(node, "rx", 0.0f);
		ry = svg__attr_float(node, "ry", 0.0f);
	}
	if (rx <= 0.0f || ry <= 0.0f) return;

	n = svg__emit_ellipse_path(buf, (int)(sizeof(buf) / sizeof(buf[0])),
			cx, cy, rx, ry);
	if (n == 0) return;

	/* Map coords to screen space. Walk the buffer tokens. */
	{
		float tmp[32];
		int j = 0;
		i = 0;
		while (i < n) {
			float op = buf[i++];
			tmp[j++] = op;
			if (op == (float)PLOTTER_PATH_MOVE ||
					op == (float)PLOTTER_PATH_LINE) {
				tmp[j++] = svg__map_x(c, buf[i++]);
				tmp[j++] = svg__map_y(c, buf[i++]);
			} else if (op == (float)PLOTTER_PATH_BEZIER) {
				int k;
				for (k = 0; k < 3; k++) {
					tmp[j++] = svg__map_x(c, buf[i++]);
					tmp[j++] = svg__map_y(c, buf[i++]);
				}
			}
			/* CLOSE has no args */
		}
		svg__init_plot_style(&pstyle, st, c);
		c->plot_ctx->plot->path(c->plot_ctx, &pstyle, tmp,
				(unsigned int)j, NULL);
	}
}

static void svg__paint_circle(dom_node *node, const struct svg_ctx *c,
		const struct svg_paint_state *st)
{
	svg__paint_ellipse_like(node, c, st, 1);
}

static void svg__paint_ellipse(dom_node *node, const struct svg_ctx *c,
		const struct svg_paint_state *st)
{
	svg__paint_ellipse_like(node, c, st, 0);
}


/* ----------------------------------------------------------------- */
/* <path d="..."> parser                                             */
/* ----------------------------------------------------------------- */

/* Skip whitespace, commas. */
static const char *svg__path_ws(const char *p)
{
	while (*p == ' ' || *p == '\t' || *p == '\n' ||
			*p == '\r' || *p == ',') p++;
	return p;
}

static const char *svg__read_num(const char *p, float *out)
{
	size_t consumed;
	p = svg__path_ws(p);
	*out = svg__atof(p, &consumed);
	return p + consumed;
}

/* Parse <path d="..."> into a buffer of plotter-path floats.
 * Returns the number of floats written.  Supported commands:
 *   M m  move
 *   L l  line
 *   H h  horizontal line
 *   V v  vertical line
 *   C c  cubic Bezier
 *   Q q  quadratic Bezier (converted to cubic)
 *   Z z  close
 * Unsupported (silently terminates path):  S s T t A a. */
static int svg__path_parse(const char *d, const struct svg_ctx *c,
		float *out, int cap)
{
	float cx = 0.0f;
	float cy = 0.0f;
	float sx = 0.0f;        /* subpath start, for Z */
	float sy = 0.0f;
	int n = 0;
	char cmd = 0;
	const char *p;

	if (d == NULL) return 0;
	p = d;

	while (*p) {
		float v;
		p = svg__path_ws(p);
		if (*p == '\0') break;
		if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
			cmd = *p;
			p++;
			p = svg__path_ws(p);
		}

		switch (cmd) {
		case 'M': case 'm': {
			float x, y;
			p = svg__read_num(p, &x);
			p = svg__read_num(p, &y);
			if (cmd == 'm') { x += cx; y += cy; }
			if (n + 3 > cap) return n;
			out[n++] = (float)PLOTTER_PATH_MOVE;
			out[n++] = svg__map_x(c, x);
			out[n++] = svg__map_y(c, y);
			cx = x; cy = y;
			sx = x; sy = y;
			/* Subsequent pairs are implicit lineto. */
			cmd = (cmd == 'M') ? 'L' : 'l';
			break;
		}
		case 'L': case 'l': {
			float x, y;
			p = svg__read_num(p, &x);
			p = svg__read_num(p, &y);
			if (cmd == 'l') { x += cx; y += cy; }
			if (n + 3 > cap) return n;
			out[n++] = (float)PLOTTER_PATH_LINE;
			out[n++] = svg__map_x(c, x);
			out[n++] = svg__map_y(c, y);
			cx = x; cy = y;
			break;
		}
		case 'H': case 'h': {
			float x;
			p = svg__read_num(p, &v);
			x = (cmd == 'h') ? cx + v : v;
			if (n + 3 > cap) return n;
			out[n++] = (float)PLOTTER_PATH_LINE;
			out[n++] = svg__map_x(c, x);
			out[n++] = svg__map_y(c, cy);
			cx = x;
			break;
		}
		case 'V': case 'v': {
			float y;
			p = svg__read_num(p, &v);
			y = (cmd == 'v') ? cy + v : v;
			if (n + 3 > cap) return n;
			out[n++] = (float)PLOTTER_PATH_LINE;
			out[n++] = svg__map_x(c, cx);
			out[n++] = svg__map_y(c, y);
			cy = y;
			break;
		}
		case 'C': case 'c': {
			float x1, y1, x2, y2, x, y;
			p = svg__read_num(p, &x1);
			p = svg__read_num(p, &y1);
			p = svg__read_num(p, &x2);
			p = svg__read_num(p, &y2);
			p = svg__read_num(p, &x);
			p = svg__read_num(p, &y);
			if (cmd == 'c') {
				x1 += cx; y1 += cy;
				x2 += cx; y2 += cy;
				x  += cx; y  += cy;
			}
			if (n + 7 > cap) return n;
			out[n++] = (float)PLOTTER_PATH_BEZIER;
			out[n++] = svg__map_x(c, x1);
			out[n++] = svg__map_y(c, y1);
			out[n++] = svg__map_x(c, x2);
			out[n++] = svg__map_y(c, y2);
			out[n++] = svg__map_x(c, x);
			out[n++] = svg__map_y(c, y);
			cx = x; cy = y;
			break;
		}
		case 'Q': case 'q': {
			/* Quadratic (x1, y1, x, y) -> cubic
			 * (c1 = cur + 2/3*(q - cur),
			 *  c2 = end + 2/3*(q - end)). */
			float qx, qy, x, y;
			float c1x, c1y, c2x, c2y;
			p = svg__read_num(p, &qx);
			p = svg__read_num(p, &qy);
			p = svg__read_num(p, &x);
			p = svg__read_num(p, &y);
			if (cmd == 'q') {
				qx += cx; qy += cy;
				x  += cx; y  += cy;
			}
			c1x = cx + (2.0f / 3.0f) * (qx - cx);
			c1y = cy + (2.0f / 3.0f) * (qy - cy);
			c2x = x  + (2.0f / 3.0f) * (qx - x);
			c2y = y  + (2.0f / 3.0f) * (qy - y);
			if (n + 7 > cap) return n;
			out[n++] = (float)PLOTTER_PATH_BEZIER;
			out[n++] = svg__map_x(c, c1x);
			out[n++] = svg__map_y(c, c1y);
			out[n++] = svg__map_x(c, c2x);
			out[n++] = svg__map_y(c, c2y);
			out[n++] = svg__map_x(c, x);
			out[n++] = svg__map_y(c, y);
			cx = x; cy = y;
			break;
		}
		case 'Z': case 'z': {
			if (n + 3 > cap) return n;
			out[n++] = (float)PLOTTER_PATH_LINE;
			out[n++] = svg__map_x(c, sx);
			out[n++] = svg__map_y(c, sy);
			out[n] = (float)PLOTTER_PATH_CLOSE;
			n++;
			cx = sx; cy = sy;
			/* Z carries no args; loop expects a fresh command. */
			cmd = 0;
			break;
		}
		default:
			/* Unsupported command (S, T, A) -- stop parsing
			 * but emit what we have. */
			return n;
		}
	}
	return n;
}

static void svg__paint_path(dom_node *node, const struct svg_ctx *c,
		const struct svg_paint_state *st)
{
	dom_string *ds = NULL;
	const char *d = svg__attr(node, "d", &ds);
	float buf[MACOS9_SVG_PATH_MAX];
	int n;
	plot_style_t pstyle;

	if (d == NULL) return;
	n = svg__path_parse(d, c, buf, MACOS9_SVG_PATH_MAX);
	dom_string_unref(ds);
	if (n == 0) return;

	svg__init_plot_style(&pstyle, st, c);
	c->plot_ctx->plot->path(c->plot_ctx, &pstyle, buf,
			(unsigned int)n, NULL);
}


/* ----------------------------------------------------------------- */
/* <polygon> / <polyline>                                            */
/* ----------------------------------------------------------------- */

static void svg__paint_poly(dom_node *node, const struct svg_ctx *c,
		const struct svg_paint_state *st, int closed)
{
	dom_string *ds = NULL;
	const char *pts = svg__attr(node, "points", &ds);
	float buf[MACOS9_SVG_PATH_MAX];
	int n = 0;
	plot_style_t pstyle;
	const char *p;
	float first_x = 0.0f, first_y = 0.0f;
	int got_first = 0;

	if (pts == NULL) return;
	p = pts;
	while (*p) {
		float x, y;
		p = svg__path_ws(p);
		if (*p == '\0') break;
		p = svg__read_num(p, &x);
		p = svg__read_num(p, &y);
		if (n + 3 > MACOS9_SVG_PATH_MAX) break;
		if (!got_first) {
			buf[n++] = (float)PLOTTER_PATH_MOVE;
			buf[n++] = svg__map_x(c, x);
			buf[n++] = svg__map_y(c, y);
			first_x = x;
			first_y = y;
			got_first = 1;
		} else {
			buf[n++] = (float)PLOTTER_PATH_LINE;
			buf[n++] = svg__map_x(c, x);
			buf[n++] = svg__map_y(c, y);
		}
	}
	dom_string_unref(ds);
	if (closed && got_first && n + 4 <= MACOS9_SVG_PATH_MAX) {
		buf[n++] = (float)PLOTTER_PATH_LINE;
		buf[n++] = svg__map_x(c, first_x);
		buf[n++] = svg__map_y(c, first_y);
		buf[n++] = (float)PLOTTER_PATH_CLOSE;
	}
	if (n == 0) return;

	svg__init_plot_style(&pstyle, st, c);
	c->plot_ctx->plot->path(c->plot_ctx, &pstyle, buf,
			(unsigned int)n, NULL);
}


/* ----------------------------------------------------------------- */
/* Walker                                                            */
/* ----------------------------------------------------------------- */

static void svg__paint_subtree(dom_node *parent,
		const struct svg_ctx *c,
		struct svg_paint_state st,
		int depth)
{
	dom_node *child = NULL;
	dom_exception err;

	if (depth >= MACOS9_SVG_GROUP_MAX) return;

	err = dom_node_get_first_child(parent, &child);
	if (err != DOM_NO_ERR || child == NULL) return;

	while (child != NULL) {
		dom_node *next = NULL;
		dom_node_type ntype;
		dom_string *tag = NULL;

		if (dom_node_get_node_type(child, &ntype) == DOM_NO_ERR &&
				ntype == DOM_ELEMENT_NODE &&
				dom_element_get_tag_name(child, &tag) ==
					DOM_NO_ERR && tag != NULL) {
			struct svg_paint_state child_st = st;
			svg__update_style(child, &child_st);

			if (dom_string_caseless_lwc_isequal(tag,
					corestring_lwc_g)) {
				svg__paint_subtree(child, c, child_st,
						depth + 1);
			} else if (dom_string_caseless_lwc_isequal(tag,
					corestring_lwc_rect)) {
				svg__paint_rect(child, c, &child_st);
			} else if (dom_string_caseless_lwc_isequal(tag,
					corestring_lwc_circle)) {
				svg__paint_circle(child, c, &child_st);
			} else if (dom_string_caseless_lwc_isequal(tag,
					corestring_lwc_ellipse)) {
				svg__paint_ellipse(child, c, &child_st);
			} else if (dom_string_caseless_lwc_isequal(tag,
					corestring_lwc_line)) {
				svg__paint_line(child, c, &child_st);
			} else if (dom_string_caseless_lwc_isequal(tag,
					corestring_lwc_path)) {
				svg__paint_path(child, c, &child_st);
			} else if (dom_string_caseless_lwc_isequal(tag,
					corestring_lwc_polygon)) {
				svg__paint_poly(child, c, &child_st, 1);
			} else if (dom_string_caseless_lwc_isequal(tag,
					corestring_lwc_polyline)) {
				svg__paint_poly(child, c, &child_st, 0);
			}
			/* unknown tags are silently skipped */
		}

		if (tag != NULL) dom_string_unref(tag);

		err = dom_node_get_next_sibling(child, &next);
		dom_node_unref(child);
		if (err != DOM_NO_ERR) return;
		child = next;
	}
}


/* ----------------------------------------------------------------- */
/* Public entry                                                      */
/* ----------------------------------------------------------------- */

nserror macos9_svg_paint_inline(struct box *box,
		int x, int y, int w, int h,
		const struct redraw_context *ctx)
{
	struct svg_ctx c;
	struct svg_paint_state st;
	dom_string *ds_vb = NULL;
	const char *vb;
	float vb_w_default;
	float vb_h_default;

	if (box == NULL || box->node == NULL || ctx == NULL ||
			ctx->plot == NULL) {
		return NSERROR_OK;
	}
	if (w <= 0 || h <= 0) return NSERROR_OK;

	c.box_x = x;
	c.box_y = y;
	c.box_w = w;
	c.box_h = h;
	c.plot_ctx = ctx;

	/* Read width / height attributes for viewBox fallback. */
	vb_w_default = svg__attr_float((dom_node *)box->node,
			"width", (float)w);
	vb_h_default = svg__attr_float((dom_node *)box->node,
			"height", (float)h);

	c.vb_x = 0.0f;
	c.vb_y = 0.0f;
	c.vb_w = vb_w_default;
	c.vb_h = vb_h_default;

	/* viewBox="x y w h" overrides. */
	vb = svg__attr((dom_node *)box->node, "viewBox", &ds_vb);
	if (vb != NULL) {
		size_t consumed;
		float fx, fy, fw, fh;
		const char *p = vb;
		fx = svg__atof(p, &consumed); p += consumed;
		p = svg__path_ws(p);
		fy = svg__atof(p, &consumed); p += consumed;
		p = svg__path_ws(p);
		fw = svg__atof(p, &consumed); p += consumed;
		p = svg__path_ws(p);
		fh = svg__atof(p, &consumed); p += consumed;
		if (fw > 0.0f && fh > 0.0f) {
			c.vb_x = fx;
			c.vb_y = fy;
			c.vb_w = fw;
			c.vb_h = fh;
		}
		dom_string_unref(ds_vb);
	}
	if (c.vb_w <= 0.0f) c.vb_w = (float)w;
	if (c.vb_h <= 0.0f) c.vb_h = (float)h;
	c.scale_x = (float)w / c.vb_w;
	c.scale_y = (float)h / c.vb_h;

	/* Initial paint state. SVG defaults: fill = black, stroke = none. */
	st.fill = 0xFF000000;
	st.stroke = 0xFF000000;
	st.fill_present = 1;
	st.stroke_present = 0;
	st.stroke_width = 1.0f;

	/* Read style attributes set on the <svg> itself. */
	svg__update_style((dom_node *)box->node, &st);

	svg__paint_subtree((dom_node *)box->node, &c, st, 0);
	return NSERROR_OK;
}
