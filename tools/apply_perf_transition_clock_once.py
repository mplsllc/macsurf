#!/usr/bin/env python3
from pathlib import Path

# Header sentinel.
hp=Path('browser/netsurf/frontends/macos9/macos9_transition.h')
h=hp.read_text()
needle='#define MACSURF_TRANSITION_TICK_MS 16\n'
assert needle in h
h=h.replace(needle, needle + '#define MACSURF_TRANSITION_NOW_AUTO 0xffffffffUL\n',1)
hp.write_text(h)

# Resolve the clock only after all transition rejection tests pass.
tp=Path('browser/netsurf/frontends/macos9/macos9_transition.c')
t=tp.read_text()
old='''    timing = desc.timing;\n    if (duration == 0) return false;\n    /* create/replace via generic engine */\n'''
new='''    timing = desc.timing;\n    if (duration == 0) return false;\n#ifdef __MACOS9__\n    /* Most style changes do not create transitions. Defer the toolbox clock\n     * read until opacity changed, a matching descriptor exists, and duration\n     * is non-zero. */\n    if (now == MACSURF_TRANSITION_NOW_AUTO)\n        now = (uint32_t)TickCount();\n#endif\n    /* create/replace via generic engine */\n'''
assert old in t
t=t.replace(old,new,1)
tp.write_text(t)

# Paint-only fast style path: request lazy clock instead of reading it eagerly.
hp2=Path('browser/netsurf/content/handlers/html/html.c')
s=hp2.read_text()
old='''\t\t/* 2B-2 opacity: A->B transition check before style replacement */\n\t\t{\n\t\t\tuint32_t now = 0;\n#ifdef __MACOS9__\n\t\t\tnow = (uint32_t)TickCount();\n#endif\n\t\t\tmacsurf_transition_handle_style_change(c, node, box->style, new_styles->styles[CSS_PSEUDO_ELEMENT_NONE], now);\n\t\t}\n'''
new='''\t\t/* 2B-2 opacity: A->B transition check before style replacement.\n\t\t * The handler resolves TickCount lazily only if an effect is created. */\n\t\tmacsurf_transition_handle_style_change(c, node, box->style,\n\t\t\t\tnew_styles->styles[CSS_PSEUDO_ELEMENT_NONE],\n\t\t\t\tMACSURF_TRANSITION_NOW_AUTO);\n'''
assert old in s
s=s.replace(old,new,1)
hp2.write_text(s)

# Whole-tree recascade: remove periodic modulo trace and eager TickCount.
bp=Path('browser/netsurf/content/handlers/html/box_construct.c')
b=bp.read_text()
old='''\t\tprocessed++;\n\t\tif ((processed % 200) == 0) {\n\t\t\tmacsurf_debug_log_writef(\n\t\t\t\t"recascade: processed=%d recascaded=%d top=%d",\n\t\t\t\tprocessed, recascaded, stack_top);\n\t\t}\n\n'''
new='''\t\tprocessed++;\n\n'''
assert old in b
b=b.replace(old,new,1)
old='''\t\t\tif (new_styles != NULL) {\n\t\t\t\tuint32_t now = 0;\n\t\t\t\t/* Start presentation effects before replacing the old\n\t\t\t\t * computed style. This also covers the full-reconstruction\n\t\t\t\t * path, which re-cascades this still-live old box tree just\n\t\t\t\t * before it constructs the replacement. */\n#ifdef __MACOS9__\n\t\t\t\tnow = (uint32_t)TickCount();\n#endif\n\t\t\t\tmacsurf_transition_handle_style_change(c, box->node,\n\t\t\t\t\t\told_self_style,\n\t\t\t\t\t\tnew_styles->styles[CSS_PSEUDO_ELEMENT_NONE], now);\n'''
new='''\t\t\tif (new_styles != NULL) {\n\t\t\t\t/* Start presentation effects before replacing the old computed\n\t\t\t\t * style. The transition handler only reads TickCount if this\n\t\t\t\t * change actually creates an effect. */\n\t\t\t\tmacsurf_transition_handle_style_change(c, box->node,\n\t\t\t\t\t\told_self_style,\n\t\t\t\t\t\tnew_styles->styles[CSS_PSEUDO_ELEMENT_NONE],\n\t\t\t\t\t\tMACSURF_TRANSITION_NOW_AUTO);\n'''
assert old in b
b=b.replace(old,new,1)
bp.write_text(b)
