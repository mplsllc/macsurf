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
#include "frontends/macos9/macsurf_debug_log.h"

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

/* fixes201 — SVG V2 gradient table.
 *
 * Pre-walked from the <svg> subtree once per paint. <linearGradient>
 * and <radialGradient> children of <defs> (or anywhere in the tree
 * by SVG spec) are scanned for their `id` + ordered <stop> children.
 * Each stop carries an offset (0..1) and a colour resolved through
 * the standard svg__parse_colour path.
 *
 * V2 limit: when a shape references a gradient via fill="url(#id)"
 * the painter substitutes the FIRST stop's colour as a solid fill.
 * Real per-pixel gradient rasterisation requires building a temp
 * GWorld of the shape's bbox + clipping through a 1-bit shape mask
 * + interpolating along the gradient axis pixel-by-pixel; deferred
 * to a later round. The first-stop fallback gives a reasonable
 * representative colour because gradients usually start with the
 * iconic / dominant colour (mactrove's apple-rainbow starts at the
 * iconic Apple-logo green at offset 0, for example). */
#define MACOS9_SVG_MAX_STOPS 16
#define MACOS9_SVG_MAX_GRADIENTS 32
#define MACOS9_SVG_GRAD_ID_MAX 63

struct svg_gradient_stop {
	float offset;        /* 0..1 */
	colour color;        /* NetSurf colour (0xAABBGGRR) */
};

struct svg_gradient {
	char id[MACOS9_SVG_GRAD_ID_MAX + 1];
	int n_stops;
	struct svg_gradient_stop stops[MACOS9_SVG_MAX_STOPS];
};

struct svg_gradient_table {
	int n_gradients;
	struct svg_gradient gradients[MACOS9_SVG_MAX_GRADIENTS];
};

struct svg_ctx {
	/* box rect in screen coords. */
	int box_x;
	int box_y;
	int box_w;
	int box_h;
	/* SVG viewBox (kept for diagnostics; the matrix below is the
	 * load-bearing transform). */
	float vb_x;
	float vb_y;
	float vb_w;
	float vb_h;
	float scale_x;
	float scale_y;
	/* fixes201 — full affine matrix applied during point mapping.
	 * Combines the viewBox-to-screen mapping with any element
	 * transform="..." attribute encountered while walking into a
	 * subtree. Layout:
	 *   screen_x = m[0] * sx + m[2] * sy + m[4]
	 *   screen_y = m[1] * sx + m[3] * sy + m[5]
	 *
	 * At svg root: m = [scale_x, 0, 0, scale_y,
	 *                   box_x - vb_x*scale_x,
	 *                   box_y - vb_y*scale_y]
	 * For descendants of <g transform="X">: m = parent_m * X */
	float m[6];
	/* plotter table. */
	const struct redraw_context *plot_ctx;
	/* gradient table populated before painting (fixes201). */
	struct svg_gradient_table *grads;
};

/* fixes201 — 3x3 matrix multiply (only top two rows since the third
 * row is always [0 0 1] for affine 2D). out = a * b. */
static void svg__matrix_mul(const float *a, const float *b, float *out)
{
	float r0, r1, r2, r3, r4, r5;
	r0 = a[0] * b[0] + a[2] * b[1];
	r2 = a[0] * b[2] + a[2] * b[3];
	r4 = a[0] * b[4] + a[2] * b[5] + a[4];
	r1 = a[1] * b[0] + a[3] * b[1];
	r3 = a[1] * b[2] + a[3] * b[3];
	r5 = a[1] * b[4] + a[3] * b[5] + a[5];
	out[0] = r0;
	out[1] = r1;
	out[2] = r2;
	out[3] = r3;
	out[4] = r4;
	out[5] = r5;
}

/* fixes201 — set a matrix to identity. */
static void svg__matrix_identity(float *m)
{
	m[0] = 1.0f; m[1] = 0.0f;
	m[2] = 0.0f; m[3] = 1.0f;
	m[4] = 0.0f; m[5] = 0.0f;
}


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

/* fixes201 — both axes route through the affine matrix. Callers
 * MUST pass both x and y because a transform with rotation or skew
 * couples the axes:
 *   screen_x = m[0]*sx + m[2]*sy + m[4]
 *   screen_y = m[1]*sx + m[3]*sy + m[5]
 *
 * For a non-rotated, non-skewed transform (the common case), m[1]
 * and m[2] are zero, so the math reduces to scale_x*sx + tx — no
 * cost for the unrotated path. */
static float svg__map_x(const struct svg_ctx *c, float sx, float sy)
{
	return c->m[0] * sx + c->m[2] * sy + c->m[4];
}

static float svg__map_y(const struct svg_ctx *c, float sx, float sy)
{
	return c->m[1] * sx + c->m[3] * sy + c->m[5];
}


/* ----------------------------------------------------------------- */
/* fixes201 — SVG gradient table                                     */
/* ----------------------------------------------------------------- */

/* Look up a gradient by id. Returns NULL if not found. */
static const struct svg_gradient *svg__find_gradient(
		const struct svg_gradient_table *grads,
		const char *id, size_t id_len)
{
	int i;
	if (grads == NULL || id == NULL) return NULL;
	for (i = 0; i < grads->n_gradients; i++) {
		size_t glen = strlen(grads->gradients[i].id);
		if (glen != id_len) continue;
		if (memcmp(grads->gradients[i].id, id, id_len) == 0) {
			return &grads->gradients[i];
		}
	}
	return NULL;
}

/* If `s` starts with `url(#id)`, look up the gradient and write its
 * representative colour to *out. Returns 1 on success, 0 if not a
 * url() reference (caller should fall through to vanilla colour
 * parsing).
 *
 * V2 picks the FIRST stop as the representative colour. Most icon
 * gradients lead with their iconic/dominant colour at offset=0, so
 * this is a meaningful fallback while we have no real gradient
 * rasteriser. */
static int svg__try_url_colour(const char *s,
		const struct svg_gradient_table *grads,
		colour *out)
{
	const char *p = s;
	const char *id_start;
	const char *id_end;
	const struct svg_gradient *g;

	while (*p == ' ' || *p == '\t') p++;
	if (p[0] != 'u' || p[1] != 'r' || p[2] != 'l' || p[3] != '(')
		return 0;
	p += 4;
	while (*p == ' ' || *p == '\t') p++;
	if (*p == '#') p++;
	id_start = p;
	while (*p && *p != ')' && *p != ' ' && *p != '\t') p++;
	id_end = p;
	if (id_end == id_start) return 0;

	g = svg__find_gradient(grads, id_start,
			(size_t)(id_end - id_start));
	if (g != NULL && g->n_stops > 0) {
		*out = g->stops[0].color;
		return 1;
	}
	/* Unresolved url() — fall back to opaque black. */
	*out = 0xFF000000;
	return 1;
}

/* Scan a single <linearGradient> or <radialGradient> element. Reads
 * the `id` attribute + each <stop> child's offset + stop-color. */
static void svg__collect_gradient(dom_node *node,
		struct svg_gradient_table *grads)
{
	dom_string *ds_id = NULL;
	const char *id;
	dom_node *child = NULL;
	struct svg_gradient *g;
	size_t id_len;

	if (grads == NULL || grads->n_gradients >= MACOS9_SVG_MAX_GRADIENTS)
		return;
	id = svg__attr(node, "id", &ds_id);
	if (id == NULL) return;
	id_len = strlen(id);
	if (id_len == 0 || id_len > MACOS9_SVG_GRAD_ID_MAX) {
		dom_string_unref(ds_id);
		return;
	}

	g = &grads->gradients[grads->n_gradients];
	memcpy(g->id, id, id_len);
	g->id[id_len] = '\0';
	g->n_stops = 0;
	dom_string_unref(ds_id);

	if (dom_node_get_first_child(node, &child) != DOM_NO_ERR ||
			child == NULL) {
		grads->n_gradients++;
		return;
	}
	while (child != NULL) {
		dom_node *next = NULL;
		dom_node_type ntype;
		dom_string *tag = NULL;
		if (dom_node_get_node_type(child, &ntype) == DOM_NO_ERR &&
				ntype == DOM_ELEMENT_NODE &&
				dom_element_get_tag_name(child, &tag) ==
					DOM_NO_ERR && tag != NULL) {
			const char *tag_s =
				(const char *)dom_string_data(tag);
			size_t tag_len = dom_string_length(tag);
			/* match "stop" case-insensitively */
			if (tag_len == 4 &&
					(tag_s[0] == 's' || tag_s[0] == 'S') &&
					(tag_s[1] == 't' || tag_s[1] == 'T') &&
					(tag_s[2] == 'o' || tag_s[2] == 'O') &&
					(tag_s[3] == 'p' || tag_s[3] == 'P')) {
				if (g->n_stops < MACOS9_SVG_MAX_STOPS) {
					dom_string *ds_off = NULL;
					dom_string *ds_col = NULL;
					const char *off_s = svg__attr(child,
							"offset", &ds_off);
					const char *col_s = svg__attr(child,
							"stop-color", &ds_col);
					float offset = 0.0f;
					colour stopcol = 0xFF000000;
					int none = 0;
					if (off_s != NULL) {
						size_t consumed;
						offset = svg__atof(off_s,
								&consumed);
						/* Trailing % means
						 * percentage of [0..1]. */
						if (consumed > 0 &&
								off_s[consumed] ==
								'%') {
							offset /= 100.0f;
						}
						dom_string_unref(ds_off);
					}
					if (col_s != NULL) {
						(void)svg__parse_colour(col_s,
								&stopcol,
								&none);
						dom_string_unref(ds_col);
					} else {
						/* Also try the style="
						 * stop-color:..." form. */
						dom_string *ds_style = NULL;
						const char *style_s =
							svg__attr(child,
								"style",
								&ds_style);
						if (style_s != NULL) {
							const char *p =
							  strstr(style_s,
							  "stop-color:");
							if (p != NULL) {
								p += 11;
								while (*p ==
									' ' ||
									*p ==
									'\t')
									p++;
								(void)svg__parse_colour(
									p,
									&stopcol,
									&none);
							}
							dom_string_unref(
								ds_style);
						}
					}
					g->stops[g->n_stops].offset = offset;
					g->stops[g->n_stops].color = stopcol;
					g->n_stops++;
				}
			}
		}
		if (tag != NULL) dom_string_unref(tag);
		if (dom_node_get_next_sibling(child, &next) != DOM_NO_ERR) {
			dom_node_unref(child);
			break;
		}
		dom_node_unref(child);
		child = next;
	}
	grads->n_gradients++;
}

/* Walk the SVG subtree once before painting to collect every
 * <linearGradient> and <radialGradient> element (typically inside
 * <defs>, but the SVG spec allows them anywhere). */
static void svg__pre_walk_gradients(dom_node *parent,
		struct svg_gradient_table *grads, int depth)
{
	dom_node *child = NULL;
	if (depth > MACOS9_SVG_GROUP_MAX) return;
	if (dom_node_get_first_child(parent, &child) != DOM_NO_ERR ||
			child == NULL) return;
	while (child != NULL) {
		dom_node *next = NULL;
		dom_node_type ntype;
		dom_string *tag = NULL;
		if (dom_node_get_node_type(child, &ntype) == DOM_NO_ERR &&
				ntype == DOM_ELEMENT_NODE &&
				dom_element_get_tag_name(child, &tag) ==
					DOM_NO_ERR && tag != NULL) {
			const char *tag_s =
				(const char *)dom_string_data(tag);
			size_t tag_len = dom_string_length(tag);
			/* linearGradient / lineargradient (case-insensitive
			 * via Hubbub's foreign-content table normalisation).
			 * Both linearGradient and radialGradient share the
			 * V2 collector — we don't distinguish at the
			 * representative-colour level. */
			if ((tag_len == 14 || tag_len == 14) &&
					((tag_s[0] == 'l' || tag_s[0] == 'L') ||
					 (tag_s[0] == 'r' || tag_s[0] == 'R'))) {
				/* Compare tail-of-tag against "Gradient" / "gradient" */
				const char *suffix = tag_s + tag_len - 8;
				if (tag_len >= 8 &&
						(suffix[0] == 'G' || suffix[0] == 'g') &&
						(suffix[1] == 'r' || suffix[1] == 'R') &&
						(suffix[2] == 'a' || suffix[2] == 'A') &&
						(suffix[3] == 'd' || suffix[3] == 'D')) {
					svg__collect_gradient(child, grads);
				}
			}
			/* Recurse into <defs>, <g>, anything that might
			 * hold gradients. */
			svg__pre_walk_gradients(child, grads, depth + 1);
		}
		if (tag != NULL) dom_string_unref(tag);
		if (dom_node_get_next_sibling(child, &next) != DOM_NO_ERR) {
			dom_node_unref(child);
			break;
		}
		dom_node_unref(child);
		child = next;
	}
}


/* ----------------------------------------------------------------- */
/* Style merge                                                       */
/* ----------------------------------------------------------------- */

/* Read fill / stroke / stroke-width attributes on a single element
 * and merge into the supplied paint state. Inherited values stay
 * unchanged when an attribute is absent. The `style="..."` attribute
 * is parsed as a flat key:value list for fill/stroke/stroke-width
 * but does not recurse into CSS shorthand.
 *
 * fixes201: when the value matches `url(#id)`, look up the
 * corresponding linearGradient / radialGradient in the pre-walked
 * gradient table and substitute the first stop's colour. */
static void svg__update_style(dom_node *node, struct svg_paint_state *st,
		const struct svg_gradient_table *grads)
{
	dom_string *ds = NULL;
	const char *v;
	colour col;
	int none;

	/* fill attribute */
	v = svg__attr(node, "fill", &ds);
	if (v != NULL) {
		if (svg__try_url_colour(v, grads, &col)) {
			st->fill = col;
			st->fill_present = 1;
		} else if (svg__parse_colour(v, &col, &none)) {
			st->fill = col;
			st->fill_present = none ? 0 : 1;
		}
		dom_string_unref(ds);
	}

	/* stroke attribute */
	v = svg__attr(node, "stroke", &ds);
	if (v != NULL) {
		if (svg__try_url_colour(v, grads, &col)) {
			st->stroke = col;
			st->stroke_present = 1;
		} else if (svg__parse_colour(v, &col, &none)) {
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
					if (svg__try_url_colour(val_start,
							grads, &col)) {
						st->fill = col;
						st->fill_present = 1;
					} else if (svg__parse_colour(val_start,
							&col, &none)) {
						st->fill = col;
						st->fill_present = none ? 0 : 1;
					}
				} else if (kl == 6 &&
						strncmp(key_start, "stroke", 6) == 0) {
					if (svg__try_url_colour(val_start,
							grads, &col)) {
						st->stroke = col;
						st->stroke_present = 1;
					} else if (svg__parse_colour(val_start,
							&col, &none)) {
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
	int rotated_or_skewed;
	plot_style_t pstyle;

	if (w <= 0.0f || h <= 0.0f) return;

	svg__init_plot_style(&pstyle, st, c);

	/* fixes203 — detect rotation/skew in the affine matrix.
	 * For an axis-aligned matrix the off-diagonal entries m[1] and m[2]
	 * are both zero (pure scale + translate). Anything else means the
	 * source rectangle becomes a rotated parallelogram in screen
	 * space; struct rect / PaintRect cannot represent that, so emit
	 * the rectangle as a 4-corner closed polygon via plot->path
	 * instead. Use a small epsilon so floating-point noise from
	 * compose-then-multiply doesn't kick the polygon path
	 * unnecessarily. */
	{
		float ab = c->m[1] < 0.0f ? -c->m[1] : c->m[1];
		float cd = c->m[2] < 0.0f ? -c->m[2] : c->m[2];
		rotated_or_skewed = (ab > 0.0005f) || (cd > 0.0005f);
	}

	if (!rotated_or_skewed) {
		struct rect r;
		r.x0 = (int)svg__map_x(c, x, y);
		r.y0 = (int)svg__map_y(c, x, y);
		r.x1 = (int)svg__map_x(c, x + w, y + h);
		r.y1 = (int)svg__map_y(c, x + w, y + h);
		c->plot_ctx->plot->rectangle(c->plot_ctx, &pstyle, &r);
		return;
	}

	/* Rotated / skewed rect — emit as a 4-point closed polygon.
	 * Corner order: TL, TR, BR, BL. The matrix maps each corner
	 * independently so the polygon honours the full affine. */
	{
		float buf[18];
		int n = 0;
		buf[n++] = (float)PLOTTER_PATH_MOVE;
		buf[n++] = svg__map_x(c, x, y);
		buf[n++] = svg__map_y(c, x, y);
		buf[n++] = (float)PLOTTER_PATH_LINE;
		buf[n++] = svg__map_x(c, x + w, y);
		buf[n++] = svg__map_y(c, x + w, y);
		buf[n++] = (float)PLOTTER_PATH_LINE;
		buf[n++] = svg__map_x(c, x + w, y + h);
		buf[n++] = svg__map_y(c, x + w, y + h);
		buf[n++] = (float)PLOTTER_PATH_LINE;
		buf[n++] = svg__map_x(c, x, y + h);
		buf[n++] = svg__map_y(c, x, y + h);
		buf[n++] = (float)PLOTTER_PATH_CLOSE;
		c->plot_ctx->plot->path(c->plot_ctx, &pstyle, buf,
				(unsigned int)n, NULL);
	}
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
	r.x0 = (int)svg__map_x(c, x1, y1);
	r.y0 = (int)svg__map_y(c, x1, y1);
	r.x1 = (int)svg__map_x(c, x2, y2);
	r.y1 = (int)svg__map_y(c, x2, y2);
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
				tmp[j++] = svg__map_x(c, buf[i++], buf[i++]);
				tmp[j++] = svg__map_y(c, buf[i++], buf[i++]);
			} else if (op == (float)PLOTTER_PATH_BEZIER) {
				int k;
				for (k = 0; k < 3; k++) {
					tmp[j++] = svg__map_x(c, buf[i++], buf[i++]);
					tmp[j++] = svg__map_y(c, buf[i++], buf[i++]);
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
/* Helper for the SVG arc -> cubic-bezier approximation. Splits the
 * (rx,ry,phi,large,sweep,x,y) end-point arc into 1-4 90-degree arcs
 * and emits each as a cubic Bezier into the output buffer. Returns
 * the number of floats written, or 0 on degenerate input.
 *
 * Algorithm follows the SVG 1.1 Appendix F.6 conversion + the
 * standard kappa-based circular arc approximation. We deliberately
 * ignore the x-axis-rotation parameter (phi) because rotated arcs
 * are vanishingly rare in icon use; for V2 we'd add a 2D rotation
 * to each control point. */
static int svg__emit_arc_as_bezier(const struct svg_ctx *c,
		float x1, float y1, float rx, float ry,
		int large_flag, int sweep_flag,
		float x2, float y2,
		float *out, int cap)
{
	float dx, dy;
	float cxA, cyA;
	float dist_sq, half_d;
	float scale;
	float ux, uy;
	float a0, a1, a_sweep, da, k;
	int n_segs, i, n;

	if (rx <= 0.0f || ry <= 0.0f) {
		if (cap >= 3) {
			out[0] = (float)PLOTTER_PATH_LINE;
			out[1] = svg__map_x(c, x2, y2);
			out[2] = svg__map_y(c, x2, y2);
			return 3;
		}
		return 0;
	}
	if (rx < 0) rx = -rx;
	if (ry < 0) ry = -ry;

	/* fixes201 V1: use the average radius for both axes — true
	 * ellipse-aware arc requires the full Appendix F.6 transform.
	 * Most SVG icons use rx == ry (circular arcs) so the average
	 * is exact; for true ellipses this is an approximation. */
	{
		float r = (rx + ry) * 0.5f;
		rx = r;
		ry = r;
	}

	dx = (x2 - x1) * 0.5f;
	dy = (y2 - y1) * 0.5f;
	dist_sq = dx * dx + dy * dy;
	if (dist_sq < 1e-6f) return 0;
	half_d = dist_sq;
	scale = rx * rx - half_d;
	if (scale < 0.0f) {
		/* Endpoints are further apart than the diameter — stretch
		 * rx/ry until they touch. */
		float d = (float)((rx * rx) / half_d);
		if (d < 0) d = -d;
		(void)d;
		scale = 0.0f;
	}
	if (scale < 0.0f) scale = 0.0f;
	{
		float t = (float)(scale / half_d);
		if (t < 0.0f) t = 0.0f;
		/* sqrt approximation via Newton-Raphson: 4 iters from
		 * a seed = t is accurate enough for arc midpoint. */
		{
			float s = (t > 0.0f) ? t : 0.0f;
			float sr = 1.0f;
			int it;
			if (s > 0.0f) {
				sr = s;
				for (it = 0; it < 6; it++) {
					sr = 0.5f * (sr + s / sr);
				}
			} else {
				sr = 0.0f;
			}
			t = sr;
		}
		if (large_flag == sweep_flag) t = -t;
		cxA = (x1 + x2) * 0.5f - t * dy;
		cyA = (y1 + y2) * 0.5f + t * dx;
	}

	/* Compute start angle, sweep. Atan2 approximation — use the
	 * built-in via the C library; libnetsurf already links libm
	 * but on CW8 we have <math.h> with float ops. */
	{
		extern double atan2(double, double);
		extern double cos(double);
		extern double sin(double);
		a0 = (float)atan2((double)(y1 - cyA), (double)(x1 - cxA));
		a1 = (float)atan2((double)(y2 - cyA), (double)(x2 - cxA));
		a_sweep = a1 - a0;
		if (sweep_flag == 0 && a_sweep > 0.0f) a_sweep -= 6.283185307f;
		else if (sweep_flag != 0 && a_sweep < 0.0f) a_sweep += 6.283185307f;
	}

	/* Split into N segments of <= 90 deg each. */
	n_segs = (int)((a_sweep < 0 ? -a_sweep : a_sweep) / 1.5707963f) + 1;
	if (n_segs < 1) n_segs = 1;
	if (n_segs > 16) n_segs = 16;
	da = a_sweep / (float)n_segs;
	/* Bezier control-point offset for circular arc of angle |da|:
	 * k = (4/3) * tan(da / 4). */
	{
		extern double tan(double);
		k = (float)((4.0 / 3.0) * tan((double)(da / 4.0f)));
	}

	n = 0;
	{
		extern double cos(double);
		extern double sin(double);
		for (i = 0; i < n_segs; i++) {
			float ca = a0 + da * (float)i;
			float cb = a0 + da * (float)(i + 1);
			float cos_a = (float)cos((double)ca);
			float sin_a = (float)sin((double)ca);
			float cos_b = (float)cos((double)cb);
			float sin_b = (float)sin((double)cb);
			float p0x = cxA + rx * cos_a;
			float p0y = cyA + ry * sin_a;
			float p3x = cxA + rx * cos_b;
			float p3y = cyA + ry * sin_b;
			float p1x = p0x - k * rx * sin_a;
			float p1y = p0y + k * ry * cos_a;
			float p2x = p3x + k * rx * sin_b;
			float p2y = p3y - k * ry * cos_b;
			if (n + 7 > cap) return n;
			out[n++] = (float)PLOTTER_PATH_BEZIER;
			out[n++] = svg__map_x(c, p1x, p1y);
			out[n++] = svg__map_y(c, p1x, p1y);
			out[n++] = svg__map_x(c, p2x, p2y);
			out[n++] = svg__map_y(c, p2x, p2y);
			out[n++] = svg__map_x(c, p3x, p3y);
			out[n++] = svg__map_y(c, p3x, p3y);
		}
		(void)ux; (void)uy;
	}
	return n;
}

/* Returns the number of floats written.  Supported commands:
 *   M m  move
 *   L l  line
 *   H h  horizontal line
 *   V v  vertical line
 *   C c  cubic Bezier
 *   S s  smooth cubic Bezier (first control reflected from prev)
 *   Q q  quadratic Bezier (converted to cubic)
 *   T t  smooth quadratic Bezier (control point reflected from prev)
 *   A a  elliptical arc (approximated via 1-4 cubic Bezier segments)
 *   Z z  close
 *
 * S/T require remembering the previous segment's control point so a
 * smooth continuation reflects it across the current point. The
 * `prev_cx`/`prev_cy` variables track that; they're reset to (cx,cy)
 * when the previous command isn't a curve of the same family.
 */
static int svg__path_parse(const char *d, const struct svg_ctx *c,
		float *out, int cap)
{
	float cx = 0.0f;
	float cy = 0.0f;
	float sx = 0.0f;        /* subpath start, for Z */
	float sy = 0.0f;
	float prev_cubic_x = 0.0f;   /* reflected control for S */
	float prev_cubic_y = 0.0f;
	float prev_quad_x = 0.0f;    /* reflected control for T */
	float prev_quad_y = 0.0f;
	int has_prev_cubic = 0;
	int has_prev_quad = 0;
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
			out[n++] = svg__map_x(c, x, y);
			out[n++] = svg__map_y(c, x, y);
			cx = x; cy = y;
			sx = x; sy = y;
			has_prev_cubic = 0;
			has_prev_quad = 0;
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
			out[n++] = svg__map_x(c, x, y);
			out[n++] = svg__map_y(c, x, y);
			cx = x; cy = y;
			has_prev_cubic = 0;
			has_prev_quad = 0;
			break;
		}
		case 'H': case 'h': {
			float x;
			p = svg__read_num(p, &v);
			x = (cmd == 'h') ? cx + v : v;
			if (n + 3 > cap) return n;
			out[n++] = (float)PLOTTER_PATH_LINE;
			out[n++] = svg__map_x(c, x, cy);
			out[n++] = svg__map_y(c, x, cy);
			cx = x;
			has_prev_cubic = 0;
			has_prev_quad = 0;
			break;
		}
		case 'V': case 'v': {
			float y;
			p = svg__read_num(p, &v);
			y = (cmd == 'v') ? cy + v : v;
			if (n + 3 > cap) return n;
			out[n++] = (float)PLOTTER_PATH_LINE;
			out[n++] = svg__map_x(c, cx, y);
			out[n++] = svg__map_y(c, cx, y);
			cy = y;
			has_prev_cubic = 0;
			has_prev_quad = 0;
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
			out[n++] = svg__map_x(c, x1, y1);
			out[n++] = svg__map_y(c, x1, y1);
			out[n++] = svg__map_x(c, x2, y2);
			out[n++] = svg__map_y(c, x2, y2);
			out[n++] = svg__map_x(c, x, y);
			out[n++] = svg__map_y(c, x, y);
			/* Reflected control point for a following S/s command. */
			prev_cubic_x = 2.0f * x - x2;
			prev_cubic_y = 2.0f * y - y2;
			has_prev_cubic = 1;
			has_prev_quad = 0;
			cx = x; cy = y;
			break;
		}
		case 'S': case 's': {
			/* Smooth cubic Bezier. If previous command was C/c/S/s,
			 * the first control point is the reflection of the
			 * previous segment's last control point across the
			 * current point; otherwise it's the current point. */
			float x1, y1, x2, y2, x, y;
			if (has_prev_cubic) {
				x1 = prev_cubic_x;
				y1 = prev_cubic_y;
			} else {
				x1 = cx;
				y1 = cy;
			}
			p = svg__read_num(p, &x2);
			p = svg__read_num(p, &y2);
			p = svg__read_num(p, &x);
			p = svg__read_num(p, &y);
			if (cmd == 's') {
				x2 += cx; y2 += cy;
				x  += cx; y  += cy;
			}
			if (n + 7 > cap) return n;
			out[n++] = (float)PLOTTER_PATH_BEZIER;
			out[n++] = svg__map_x(c, x1, y1);
			out[n++] = svg__map_y(c, x1, y1);
			out[n++] = svg__map_x(c, x2, y2);
			out[n++] = svg__map_y(c, x2, y2);
			out[n++] = svg__map_x(c, x, y);
			out[n++] = svg__map_y(c, x, y);
			prev_cubic_x = 2.0f * x - x2;
			prev_cubic_y = 2.0f * y - y2;
			has_prev_cubic = 1;
			has_prev_quad = 0;
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
			out[n++] = svg__map_x(c, c1x, c1y);
			out[n++] = svg__map_y(c, c1x, c1y);
			out[n++] = svg__map_x(c, c2x, c2y);
			out[n++] = svg__map_y(c, c2x, c2y);
			out[n++] = svg__map_x(c, x, y);
			out[n++] = svg__map_y(c, x, y);
			/* Reflected quadratic control point for a following T. */
			prev_quad_x = 2.0f * x - qx;
			prev_quad_y = 2.0f * y - qy;
			has_prev_quad = 1;
			has_prev_cubic = 0;
			cx = x; cy = y;
			break;
		}
		case 'T': case 't': {
			/* Smooth quadratic: control point reflected from the
			 * previous T/Q segment, or = current point if not. */
			float qx, qy, x, y;
			float c1x, c1y, c2x, c2y;
			if (has_prev_quad) {
				qx = prev_quad_x;
				qy = prev_quad_y;
			} else {
				qx = cx;
				qy = cy;
			}
			p = svg__read_num(p, &x);
			p = svg__read_num(p, &y);
			if (cmd == 't') { x += cx; y += cy; }
			c1x = cx + (2.0f / 3.0f) * (qx - cx);
			c1y = cy + (2.0f / 3.0f) * (qy - cy);
			c2x = x  + (2.0f / 3.0f) * (qx - x);
			c2y = y  + (2.0f / 3.0f) * (qy - y);
			if (n + 7 > cap) return n;
			out[n++] = (float)PLOTTER_PATH_BEZIER;
			out[n++] = svg__map_x(c, c1x, c1y);
			out[n++] = svg__map_y(c, c1x, c1y);
			out[n++] = svg__map_x(c, c2x, c2y);
			out[n++] = svg__map_y(c, c2x, c2y);
			out[n++] = svg__map_x(c, x, y);
			out[n++] = svg__map_y(c, x, y);
			prev_quad_x = 2.0f * x - qx;
			prev_quad_y = 2.0f * y - qy;
			has_prev_quad = 1;
			has_prev_cubic = 0;
			cx = x; cy = y;
			break;
		}
		case 'A': case 'a': {
			/* Elliptical arc:
			 *   rx ry x-axis-rotation large-arc-flag sweep-flag x y
			 * We approximate via cubic Bezier subdivision. The
			 * x-axis-rotation parameter is read but ignored (most
			 * SVG icons don't use rotated arcs). */
			float rx, ry, phi;
			float la_v, sw_v;
			int large_flag, sweep_flag;
			float x, y;
			int emit;
			p = svg__read_num(p, &rx);
			p = svg__read_num(p, &ry);
			p = svg__read_num(p, &phi);
			p = svg__read_num(p, &la_v);
			p = svg__read_num(p, &sw_v);
			p = svg__read_num(p, &x);
			p = svg__read_num(p, &y);
			large_flag = (la_v != 0.0f);
			sweep_flag = (sw_v != 0.0f);
			if (cmd == 'a') { x += cx; y += cy; }
			(void)phi; /* TODO: 2D rotation of control points */
			emit = svg__emit_arc_as_bezier(c, cx, cy, rx, ry,
					large_flag, sweep_flag, x, y,
					out + n, cap - n);
			n += emit;
			has_prev_cubic = 0;
			has_prev_quad = 0;
			cx = x; cy = y;
			break;
		}
		case 'Z': case 'z': {
			if (n + 3 > cap) return n;
			out[n++] = (float)PLOTTER_PATH_LINE;
			out[n++] = svg__map_x(c, sx, sy);
			out[n++] = svg__map_y(c, sx, sy);
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
/* <text>                                                            */
/* ----------------------------------------------------------------- */

/* fixes201 — SVG <text> V1.
 *
 * Reads x / y / font-size from the element attributes, takes the
 * concatenated text content of immediate text-node children, and
 * dispatches to the plotter table. Strokes are not handled (SVG
 * text supports both fill and stroke, but the plotter text op is
 * fill-only; document as PARTIAL).
 *
 * V1 limits:
 *   - no <tspan> recursion (concatenates one level of text-node
 *     children only)
 *   - no text-anchor / dominant-baseline (uses default left-aligned
 *     alphabetic-baseline behaviour)
 *   - no font-family / font-weight attributes (uses the default
 *     QuickDraw text style)
 *   - no transform on the text element
 */
static void svg__paint_text(dom_node *node, const struct svg_ctx *c,
		const struct svg_paint_state *st)
{
	float x_svg = svg__attr_float(node, "x", 0.0f);
	float y_svg = svg__attr_float(node, "y", 0.0f);
	float font_size = svg__attr_float(node, "font-size", 12.0f);
	int screen_x;
	int screen_y;
	plot_font_style_t fstyle;
	dom_node *child = NULL;
	char accum[256];
	size_t accum_len = 0;

	if (!st->fill_present) return;

	screen_x = (int)svg__map_x(c, x_svg, y_svg);
	screen_y = (int)svg__map_y(c, x_svg, y_svg);

	memset(&fstyle, 0, sizeof(fstyle));
	fstyle.family = PLOT_FONT_FAMILY_SANS_SERIF;
	fstyle.size = (int)(font_size *
			(c->scale_x < c->scale_y ? c->scale_x : c->scale_y) *
			PLOT_STYLE_SCALE);
	if (fstyle.size < 6 * PLOT_STYLE_SCALE) fstyle.size = 6 * PLOT_STYLE_SCALE;
	fstyle.weight = 400;
	fstyle.flags = FONTF_NONE;
	fstyle.background = 0;
	fstyle.foreground = st->fill;
	/* transform_b identity (no rotation in V1 text). */

	/* Concatenate text-node children. */
	if (dom_node_get_first_child(node, &child) != DOM_NO_ERR ||
			child == NULL) {
		return;
	}
	while (child != NULL) {
		dom_node *next = NULL;
		dom_node_type ntype;
		if (dom_node_get_node_type(child, &ntype) == DOM_NO_ERR &&
				ntype == DOM_TEXT_NODE) {
			dom_string *content = NULL;
			if (dom_node_get_text_content(child, &content) ==
					DOM_NO_ERR && content != NULL) {
				const char *s =
					(const char *)dom_string_data(content);
				size_t slen = dom_string_length(content);
				if (slen > sizeof(accum) - 1 - accum_len) {
					slen = sizeof(accum) - 1 - accum_len;
				}
				if (slen > 0) {
					memcpy(accum + accum_len, s, slen);
					accum_len += slen;
				}
				dom_string_unref(content);
			}
		}
		if (dom_node_get_next_sibling(child, &next) != DOM_NO_ERR) {
			dom_node_unref(child);
			break;
		}
		dom_node_unref(child);
		child = next;
	}

	if (accum_len == 0) return;
	accum[accum_len] = '\0';
	c->plot_ctx->plot->text(c->plot_ctx, &fstyle, screen_x, screen_y,
			accum, accum_len);
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
			buf[n++] = svg__map_x(c, x, y);
			buf[n++] = svg__map_y(c, x, y);
			first_x = x;
			first_y = y;
			got_first = 1;
		} else {
			buf[n++] = (float)PLOTTER_PATH_LINE;
			buf[n++] = svg__map_x(c, x, y);
			buf[n++] = svg__map_y(c, x, y);
		}
	}
	dom_string_unref(ds);
	if (closed && got_first && n + 4 <= MACOS9_SVG_PATH_MAX) {
		buf[n++] = (float)PLOTTER_PATH_LINE;
		buf[n++] = svg__map_x(c, first_x, first_y);
		buf[n++] = svg__map_y(c, first_x, first_y);
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

/* fixes201 — parse one transform function out of an attribute
 * string. Returns the number of bytes consumed (including the
 * opening `name(...` and the trailing `)`), and writes the
 * corresponding 6-component matrix into *fn_matrix. Returns 0 on
 * end-of-string or syntax error.
 *
 * Supported functions:
 *   translate(tx [, ty])    : [1 0 0 1 tx ty]
 *   scale(sx [, sy])        : [sx 0 0 sy 0 0]   (sy defaults = sx)
 *   rotate(deg)             : [cos sin -sin cos 0 0] with deg→rad
 *   rotate(deg, cx, cy)     : translate(cx,cy) × rotate × translate(-cx,-cy)
 *   matrix(a, b, c, d, e, f): direct [a b c d e f]
 *   skewX(deg) / skewY(deg) : [1 tan 0 1 0 0] / [1 0 tan 1 0 0]
 */
static size_t svg__parse_one_transform(const char *s,
		float *fn_matrix)
{
	const char *p = s;
	size_t cs;
	const char *name_start;
	size_t name_len;

	/* skip leading whitespace + commas */
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ||
			*p == ',') p++;
	if (*p == '\0') return 0;

	name_start = p;
	while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
			*p == '_') p++;
	name_len = (size_t)(p - name_start);
	if (name_len == 0) return 0;

	while (*p == ' ' || *p == '\t') p++;
	if (*p != '(') return 0;
	p++;

	/* Read up to 6 numeric arguments. */
	{
		float args[6] = {0,0,0,0,0,0};
		int n_args = 0;
		while (*p && *p != ')' && n_args < 6) {
			while (*p == ' ' || *p == '\t' || *p == ',') p++;
			if (*p == ')' || *p == '\0') break;
			args[n_args] = svg__atof(p, &cs);
			if (cs == 0) {
				/* parse error — abort by walking to ')'. */
				while (*p && *p != ')') p++;
				if (*p == ')') p++;
				svg__matrix_identity(fn_matrix);
				return (size_t)(p - s);
			}
			p += cs;
			n_args++;
		}
		while (*p == ' ' || *p == '\t') p++;
		if (*p == ')') p++;

		svg__matrix_identity(fn_matrix);

		if (name_len == 9 &&
				(name_start[0] == 't' || name_start[0] == 'T') &&
				strncasecmp(name_start, "translate", 9) == 0) {
			fn_matrix[4] = args[0];
			fn_matrix[5] = (n_args >= 2) ? args[1] : 0.0f;
		} else if (name_len == 5 &&
				strncasecmp(name_start, "scale", 5) == 0) {
			fn_matrix[0] = args[0];
			fn_matrix[3] = (n_args >= 2) ? args[1] : args[0];
		} else if (name_len == 6 &&
				strncasecmp(name_start, "rotate", 6) == 0) {
			float rad = args[0] * 3.14159265f / 180.0f;
			extern double cos(double);
			extern double sin(double);
			float ca = (float)cos((double)rad);
			float sa = (float)sin((double)rad);
			if (n_args >= 3) {
				/* rotate(deg, cx, cy) — compose
				 * translate(cx, cy) * rotate * translate(-cx,-cy) */
				float cx = args[1];
				float cy = args[2];
				/* Rotation centred on (cx, cy):
				 * a=cos, b=sin, c=-sin, d=cos
				 * e = cx - cx*cos + cy*sin
				 * f = cy - cx*sin - cy*cos */
				fn_matrix[0] = ca;
				fn_matrix[1] = sa;
				fn_matrix[2] = -sa;
				fn_matrix[3] = ca;
				fn_matrix[4] = cx - cx*ca + cy*sa;
				fn_matrix[5] = cy - cx*sa - cy*ca;
			} else {
				fn_matrix[0] = ca;
				fn_matrix[1] = sa;
				fn_matrix[2] = -sa;
				fn_matrix[3] = ca;
			}
		} else if (name_len == 6 &&
				strncasecmp(name_start, "matrix", 6) == 0) {
			if (n_args >= 6) {
				fn_matrix[0] = args[0];
				fn_matrix[1] = args[1];
				fn_matrix[2] = args[2];
				fn_matrix[3] = args[3];
				fn_matrix[4] = args[4];
				fn_matrix[5] = args[5];
			}
		} else if (name_len == 5 &&
				strncasecmp(name_start, "skewX", 5) == 0) {
			extern double tan(double);
			float rad = args[0] * 3.14159265f / 180.0f;
			fn_matrix[2] = (float)tan((double)rad);
		} else if (name_len == 5 &&
				strncasecmp(name_start, "skewY", 5) == 0) {
			extern double tan(double);
			float rad = args[0] * 3.14159265f / 180.0f;
			fn_matrix[1] = (float)tan((double)rad);
		} else if (name_len == 10 &&
				strncasecmp(name_start, "translateX", 10) == 0) {
			fn_matrix[4] = args[0];
		} else if (name_len == 10 &&
				strncasecmp(name_start, "translateY", 10) == 0) {
			fn_matrix[5] = args[0];
		}
		/* Unknown function: identity (already set). */
	}

	return (size_t)(p - s);
}

/* fixes201 — Read the `transform="..."` attribute on a node and
 * compose its multi-function value into a matrix. Returns 1 if a
 * transform attribute was present (and composed into *out), 0
 * otherwise (caller's matrix stays as-is). */
static int svg__read_transform_attr(dom_node *node, float *out)
{
	dom_string *ds = NULL;
	const char *src = svg__attr(node, "transform", &ds);
	float chain[6];
	float fn_mat[6];
	size_t off;

	if (src == NULL) return 0;
	svg__matrix_identity(chain);
	while (*src) {
		off = svg__parse_one_transform(src, fn_mat);
		if (off == 0) break;
		{
			float tmp[6];
			svg__matrix_mul(chain, fn_mat, tmp);
			memcpy(chain, tmp, sizeof(chain));
		}
		src += off;
	}
	memcpy(out, chain, sizeof(chain));
	dom_string_unref(ds);
	return 1;
}

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
			struct svg_ctx child_ctx = *c;
			float local_xform[6];
			const struct svg_ctx *use_ctx = c;
			svg__update_style(child, &child_st, c->grads);

			/* fixes201 — compose any transform="..." on the
			 * child element into a local ctx that's then used
			 * for painting that element (and recursively
			 * inherited for <g> children). The viewBox
			 * matrix already lives in c->m[]; for the child
			 * we compute child.m = parent.m * local_xform. */
			if (svg__read_transform_attr(child, local_xform)) {
				svg__matrix_mul(c->m, local_xform,
						child_ctx.m);
				use_ctx = &child_ctx;
			}

			macsurf_debug_log_writef(
				"svg_shape: depth=%d tag=%s fill=0x%08x stroke=0x%08x",
				depth,
				(const char *)dom_string_data(tag),
				(unsigned int)child_st.fill,
				(unsigned int)child_st.stroke);

			if (dom_string_caseless_lwc_isequal(tag,
					corestring_lwc_g)) {
				svg__paint_subtree(child, use_ctx, child_st,
						depth + 1);
			} else if (dom_string_caseless_lwc_isequal(tag,
					corestring_lwc_rect)) {
				svg__paint_rect(child, use_ctx, &child_st);
			} else if (dom_string_caseless_lwc_isequal(tag,
					corestring_lwc_circle)) {
				svg__paint_circle(child, use_ctx, &child_st);
			} else if (dom_string_caseless_lwc_isequal(tag,
					corestring_lwc_ellipse)) {
				svg__paint_ellipse(child, use_ctx, &child_st);
			} else if (dom_string_caseless_lwc_isequal(tag,
					corestring_lwc_line)) {
				svg__paint_line(child, use_ctx, &child_st);
			} else if (dom_string_caseless_lwc_isequal(tag,
					corestring_lwc_path)) {
				svg__paint_path(child, use_ctx, &child_st);
			} else if (dom_string_caseless_lwc_isequal(tag,
					corestring_lwc_polygon)) {
				svg__paint_poly(child, use_ctx, &child_st, 1);
			} else if (dom_string_caseless_lwc_isequal(tag,
					corestring_lwc_polyline)) {
				svg__paint_poly(child, use_ctx, &child_st, 0);
			} else if (dom_string_caseless_lwc_isequal(tag,
					corestring_lwc_text)) {
				svg__paint_text(child, use_ctx, &child_st);
			}
			/* <defs> / <linearGradient> / <radialGradient> /
			 * unknown tags are silently skipped (gradients are
			 * pre-walked separately). */
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

	/* fixes201 — pre-bake the viewBox-to-screen affine matrix at
	 * root. svg__map_x / svg__map_y apply this for every painted
	 * shape. <g transform="..."> child elements compose into a
	 * copy of this matrix during the subtree walk. */
	c.m[0] = c.scale_x;
	c.m[1] = 0.0f;
	c.m[2] = 0.0f;
	c.m[3] = c.scale_y;
	c.m[4] = (float)c.box_x - c.vb_x * c.scale_x;
	c.m[5] = (float)c.box_y - c.vb_y * c.scale_y;

	/* fixes201 — gradient pre-walk. Allocated on stack to avoid heap
	 * pressure during paint; cap of MACOS9_SVG_MAX_GRADIENTS handles
	 * the busiest icon sets (mactrove's apple has 1; the full menu
	 * panel has under 10). */
	{
		struct svg_gradient_table grads_local;
		grads_local.n_gradients = 0;
		svg__pre_walk_gradients((dom_node *)box->node,
				&grads_local, 0);
		c.grads = &grads_local;

		/* Initial paint state. SVG defaults: fill = black,
		 * stroke = none. */
		st.fill = 0xFF000000;
		st.stroke = 0xFF000000;
		st.fill_present = 1;
		st.stroke_present = 0;
		st.stroke_width = 1.0f;

		/* Read style attributes set on the <svg> itself. */
		svg__update_style((dom_node *)box->node, &st, c.grads);

		macsurf_debug_log_writef(
			"svg_walk_enter: vb=(%d,%d,%d,%d) box=(%d,%d,%d,%d) scale=(%d,%d) thousandths grads=%d",
			(int)(c.vb_x * 1000), (int)(c.vb_y * 1000),
			(int)(c.vb_w * 1000), (int)(c.vb_h * 1000),
			c.box_x, c.box_y, c.box_w, c.box_h,
			(int)(c.scale_x * 1000), (int)(c.scale_y * 1000),
			grads_local.n_gradients);

		svg__paint_subtree((dom_node *)box->node, &c, st, 0);
	}
	return NSERROR_OK;
}
