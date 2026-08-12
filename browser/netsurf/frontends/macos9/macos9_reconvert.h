/*
 * MacSurf  -  Mac OS 9 frontend for NetSurf
 * macos9_reconvert.h  -  JS DOM-mutation dirty signal.
 *
 * This file is part of MacSurf, built on the NetSurf engine.
 * Licensed under GPL v2.
 *
 * fixes910 (incremental-reconvert Phase 0)  -  carry WHAT changed, not just THAT
 * something changed.
 *
 * Until now the signal was `macos9_js_mark_dom_dirty(content)`: a per-document
 * boolean. Every JS binding that mutates the DOM already has the mutated node in
 * hand and threw it away, so the reconvert had no choice but to rebuild the whole
 * box tree and re-run every O(document) side-pass for a one-attribute change.
 *
 * Phase 0 changes NOTHING about that behaviour  -  the reconvert still rebuilds
 * everything. It only records the node + mutation kind so later phases can
 * decide to do less. Keeping the behaviour change out of this step is
 * deliberate: the rebuild path was just hardware-verified
 * (fixes906-reconvert-crash-verified) and this must not disturb it.
 *
 * LIFETIME: the recorded node is held across the ~400ms debounce, and JS is free
 * to remove and free that node inside the window. The pending table therefore
 * holds a real dom_node REFERENCE, taken via the opaque shims in html.c. The
 * frontend deliberately carries no dom headers (frontends/macos9/dom/dom.h is a
 * stub that shadows the real header under CW8's access paths), so the node is
 * stored as void* here and is NEVER dereferenced on this side.
 */

#ifndef MACOS9_RECONVERT_H
#define MACOS9_RECONVERT_H

struct content;

/*
 * Which DOM binding produced the mutation. Recorded only; no consumer yet.
 *
 * The point of keeping the kind separate from the node is that geometry
 * relevance differs sharply between them: a CHARDATA/TEXTCONTENT write on a leaf
 * can often be answered with a targeted redraw (see html_texty_element_update in
 * dom_event.c, which already does exactly that for form values), whereas
 * APPENDCHILD / REMOVECHILD / INSERTBEFORE / INNERHTML restructure the tree and
 * generally cannot. That classification is Phase 2's job, not this file's.
 */
enum macos9_dommut_kind {
	MACOS9_DOMMUT_UNKNOWN = 0,
	MACOS9_DOMMUT_SETATTRIBUTE,
	MACOS9_DOMMUT_REMOVEATTRIBUTE,
	MACOS9_DOMMUT_TEXTCONTENT,
	MACOS9_DOMMUT_INNERHTML,
	MACOS9_DOMMUT_APPENDCHILD,
	MACOS9_DOMMUT_REMOVECHILD,
	MACOS9_DOMMUT_INSERTBEFORE,
	MACOS9_DOMMUT_CHARDATA,
	/* fixes926  -  split the setattr bucket by ATTRIBUTE NAME. The Phase 2
	 * question is not "how many setAttribute calls" but "how many of them
	 * could be answered by a recascade instead of a rebuild", and only
	 * class/style can be. Everything else is either baked at box
	 * construction (src/type/colspan/rowspan/title/id/href...) or reaches
	 * nothing at all. Classified at the JS call site where the name is
	 * already in hand, so this costs one strcmp on the mutation path. */
	MACOS9_DOMMUT_SETATTR_CLASS,
	MACOS9_DOMMUT_SETATTR_STYLE
};

/*
 * Record a DOM mutation on `c`.
 *
 * `node` is the mutated dom_node as an opaque pointer (the PARENT for child-list
 * mutations  -  that is where layout would change), or NULL if unknown. `kind` is
 * a macos9_dommut_kind.
 *
 * Precision is best-effort and degrades safely: if two DISTINCT nodes mutate
 * before the debounce fires, the slot drops to "multiple" and forgets the node,
 * which simply means later phases fall back to the full rebuild we do today.
 */
void macos9_js_mark_dom_dirty_node(struct content *c, void *node, int kind);

/* Legacy entry point  -  equivalent to _node(c, NULL, MACOS9_DOMMUT_UNKNOWN),
 * i.e. "something changed, no idea what". Kept so any caller that cannot name
 * the node still works, and always yields the full rebuild. */
void macos9_js_mark_dom_dirty(struct content *c);

#endif /* MACOS9_RECONVERT_H */
