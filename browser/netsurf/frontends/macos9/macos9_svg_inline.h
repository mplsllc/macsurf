/*
 * macos9_svg_inline.h -- fixes195 inline SVG renderer (V1).
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
 * V2 deferred:
 *   - <text>, <use>, <symbol>, <image>
 *   - <linearGradient> / <radialGradient> + url(#id) refs
 *   - transform="..." attribute (rotate/translate/scale)
 *   - SVG arc (A) command in <path d>
 *   - stroke-dasharray / linecap / linejoin / fill-rule
 *   - opacity / fill-opacity / stroke-opacity
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

#include "utils/errors.h"
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
 * \return NSERROR_OK on success; on partial failure paints what it
 *         can and returns NSERROR_OK. Returns an error only on
 *         catastrophic state (NULL ctx, etc.).
 */
nserror macos9_svg_paint_inline(struct box *box,
		int x, int y, int w, int h,
		const struct redraw_context *ctx);

#endif /* MACOS9_SVG_INLINE_H_ */
