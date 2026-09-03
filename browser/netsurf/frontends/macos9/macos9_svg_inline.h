/*
 * macos9_svg_inline.h - fixes195 inline SVG renderer (V1).
 *
 * Renders an inline <svg> element by walking its DOM children and
 * issuing plotter calls for each shape primitive. The root <svg>
 * box is detected at box-construct time (box->flags & SVG_INLINE);
 * the html_redraw_box dispatcher in redraw.c calls into this file
 * once per SVG root.
 *
 * V1 coverage:
 *   - shapes: <rect>, <circle>, <ellipse>, <line>, <polygon>,
 *             <polyline>, <path d="...">
 *   - grouping: <g> (children walked, attributes inherited)
 *   - attributes: x, y, width, height, cx, cy, r, rx, ry,
 *                 x1, y1, x2, y2, points, d, fill, stroke,
 *                 stroke-width
 *   - colours:   #rgb, #rrggbb, named (16 colours), rgb(R,G,B),
 *                "none"
 *   - viewBox + width/height attrs map to box rect
 *
 * This V1/V2 split predates several since-shipped rounds - text, use,
 * gradients, transform= (full translate/scale/rotate/matrix/skewX/
 * skewY composition via svg__parse_one_transform + svg__matrix_mul),
 * and fill-opacity/stroke-opacity are all wired into the dispatcher
 * (svg__paint_subtree) below, not deferred. Treat that dispatcher as
 * ground truth over this list for what's actually painted.
 *
 * Still deferred:
 *   - <symbol>, <image>
 *   - CSS <style> selectors targeting SVG elements
 *
 * Coordinate transform: maps the SVG viewBox to the box rect.
 * viewBox="x y w h" sets the inner coordinate system; the renderer
 * scales each shape's coords through (x, y, w, h) -> (box.x, box.y,
 * box.w, box.h). If viewBox is absent, the SVG's width/height
 * attrs are used directly; if those are absent too, the box rect
 * is used 1:1.
 *
 * Integer math is used throughout except where the plot_path
 * plotter requires float coords. CW8 PPC long-long codegen is
 * unsafe, so 32-bit intermediates only.
 */

#ifndef MACOS9_SVG_INLINE_H_
#define MACOS9_SVG_INLINE_H_

#include "utils/ns_errors.h"
#include "netsurf/plotters.h"

struct box;
struct redraw_context;

/**
 * Paint an inline <svg> root and all its children.
 *
 * \param box   The box constructed for the <svg> element. Must have
 *              SVG_INLINE set in box->flags and box->node pointing
 *              at the SVG DOM element.
 * \param x     Box pixel x in current paint coords (already scrolled
 *              and offset-adjusted by the caller).
 * \param y     Box pixel y.
 * \param w     Box pixel width.
 * \param h     Box pixel height.
 * \param ctx   Redraw context (plotter table).
 * \param base_url  Page base URL for resolving external <use> sprite
 *              references; may be NULL (external icons then skipped).
 * \return NSERROR_OK on success; on partial failure paints what it
 *         can and returns NSERROR_OK. Returns an error only on
 *         catastrophic state (NULL ctx, etc.).
 */
struct nsurl;
struct html_content;
nserror macos9_svg_paint_inline(struct box *box,
		int x, int y, int w, int h,
		const struct redraw_context *ctx,
		struct nsurl *base_url,
		const struct html_content *html);

/**
 * Paint a STANDALONE external SVG (img src=*.svg / CSS background
 * url(*.svg)) from its raw source text into (x,y,w,h), clipped to
 * (the intersection of) clip and that box. Shape/transform/fit coverage
 * matches the inline DOM renderer (macos9_svg_paint_inline): path, rect,
 * circle, ellipse, line, polygon, polyline; full affine transform=
 * composition (translate/scale/rotate/matrix/skewX/skewY) across nested
 * <g>; preserveAspectRatio (meet/slice/none + 9 alignments). Used by the
 * image/svg+xml content handler's redraw (macos9_image.c).
 */
nserror macos9_svg_paint_standalone(const char *src, size_t len,
		int x, int y, int w, int h,
		const struct rect *clip,
		const struct redraw_context *ctx);

/**
 * Locate the root <svg ...> tag inside a bounded (262144 byte),
 * NUL-terminated copy of raw SVG source. On success (true), *out_buf is
 * a malloc'd buffer the caller owns and must free(); *out_root/
 * *out_root_end bound the opening tag's span within it (for
 * svg__tag_attr-style lookups). On failure (false, no <svg found), the
 * buffer has already been freed internally and the outputs are
 * untouched - nothing for the caller to free.
 */
bool macos9_svg_locate_root(const char *src, size_t len,
		char **out_buf, const char **out_root,
		const char **out_root_end);

struct macos9_svg_root_dims {
	int have_vb;
	float vb_x, vb_y, vb_w, vb_h;
	int have_width;
	float width;
	int have_height;
	float height;
};

/**
 * Pure: parse viewBox (full float precision) and width/height (skipped
 * if the value contains '%') off an already-located root <svg ...> tag
 * span. No allocation, no side effects.
 */
void macos9_svg_parse_root_dims(const char *root, const char *root_end,
		struct macos9_svg_root_dims *out);

/**
 * Compute the viewBox-to-destination affine (uniform/non-uniform scale
 * + translate) per preserveAspectRatio semantics. par_value is the raw
 * attribute value (may be NULL/empty for the SVG default, xMidYMid
 * meet). Shared by the DOM and standalone paint paths so both honour
 * the same fit rules; has no knowledge of clipping.
 */
void macos9_svg_compute_fit(int x, int y, int w, int h,
		float vb_x, float vb_y, float vb_w, float vb_h,
		const char *par_value,
		float *out_scale_x, float *out_scale_y,
		float *out_tx, float *out_ty);

#endif /* MACOS9_SVG_INLINE_H_ */
