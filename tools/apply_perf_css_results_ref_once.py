#!/usr/bin/env python3
from pathlib import Path

hp = Path('browser/libcss/include/libcss/select.h')
cp = Path('browser/libcss/src/select/css_select.c')
h = hp.read_text()
c = cp.read_text()

old = '''typedef struct css_select_results {\n\t/**\n\t * Array of pointers to computed styles,\n\t * indexed by css_pseudo_element. If there\n\t * was no styling for a given pseudo element,\n\t * then no computed style will be created and\n\t * the corresponding pointer will be set to NULL\n\t */\n\tcss_computed_style *styles[CSS_PSEUDO_ELEMENT_COUNT];\n} css_select_results;\n'''
new = '''typedef struct css_select_results {\n\t/**\n\t * Array of pointers to computed styles,\n\t * indexed by css_pseudo_element. If there\n\t * was no styling for a given pseudo element,\n\t * then no computed style will be created and\n\t * the corresponding pointer will be set to NULL\n\t */\n\tcss_computed_style *styles[CSS_PSEUDO_ELEMENT_COUNT];\n\t/* Result-wrapper ownership count. Computed styles are immutable/interned;\n\t * sharing the wrapper avoids an allocation for reconvert Style-B reuse. */\n\tuint32_t refs;\n} css_select_results;\n'''
assert old in h
h = h.replace(old, new, 1)

old = '''\tstate->results = calloc(1, sizeof(css_select_results));\n\tif (state->results == NULL) {\n\t\treturn CSS_NOMEM;\n\t}\n'''
new = '''\tstate->results = calloc(1, sizeof(css_select_results));\n\tif (state->results == NULL) {\n\t\treturn CSS_NOMEM;\n\t}\n\tstate->results->refs = 1;\n'''
assert old in c
c = c.replace(old, new, 1)

old = '''css_error css_select_results_destroy(css_select_results *results)\n{\n\tuint32_t i;\n\n\tif (results == NULL)\n\t\treturn CSS_BADPARM;\n\n\tfor (i = 0; i < CSS_PSEUDO_ELEMENT_COUNT; i++) {\n\t\tif (results->styles[i] != NULL)\n\t\t\tcss_computed_style_destroy(results->styles[i]);\n\t}\n\n\tfree(results);\n\n\treturn CSS_OK;\n}\n\n/* MacSurf reconvert fast path: duplicate the result container while sharing\n * the immutable/interned computed styles by reference. */\ncss_select_results *css_select_results_ref(const css_select_results *results)\n{\n\tcss_select_results *copy;\n\tuint32_t i;\n\n\tif (results == NULL)\n\t\treturn NULL;\n\tcopy = calloc(1, sizeof(*copy));\n\tif (copy == NULL)\n\t\treturn NULL;\n\tfor (i = 0; i < CSS_PSEUDO_ELEMENT_COUNT; i++) {\n\t\tif (results->styles[i] != NULL)\n\t\t\tcopy->styles[i] = css__computed_style_ref(results->styles[i]);\n\t}\n\treturn copy;\n}\n'''
new = '''css_error css_select_results_destroy(css_select_results *results)\n{\n\tuint32_t i;\n\n\tif (results == NULL)\n\t\treturn CSS_BADPARM;\n\n\tif (results->refs > 1) {\n\t\tresults->refs--;\n\t\treturn CSS_OK;\n\t}\n\n\tfor (i = 0; i < CSS_PSEUDO_ELEMENT_COUNT; i++) {\n\t\tif (results->styles[i] != NULL)\n\t\t\tcss_computed_style_destroy(results->styles[i]);\n\t}\n\n\tfree(results);\n\n\treturn CSS_OK;\n}\n\n/* MacSurf reconvert fast path: share the immutable result wrapper itself. */\ncss_select_results *css_select_results_ref(const css_select_results *results)\n{\n\tcss_select_results *shared;\n\n\tif (results == NULL)\n\t\treturn NULL;\n\tshared = (css_select_results *)results;\n\tif (shared->refs == 0)\n\t\tshared->refs = 1;\n\tif (shared->refs == 0xffffffffUL)\n\t\treturn NULL;\n\tshared->refs++;\n\treturn shared;\n}\n'''
assert old in c
c = c.replace(old, new, 1)

hp.write_text(h)
cp.write_text(c)
