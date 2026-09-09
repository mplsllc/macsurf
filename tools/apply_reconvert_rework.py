from pathlib import Path

hp = Path('browser/netsurf/content/handlers/html/html.c')
hs = hp.read_text()

marker = 'int html_reconvert_fast_style(struct content *base_c, void *vnode)\n{\n'
assert marker in hs, 'fast-style marker missing'
helper = '''/* stabilization/reconvert-rework: targeted style work must never trust the\n * DOM -> box backlink. Full reconvert has repeatedly made that backlink a\n * lifetime hazard. Search the current live rendered tree instead. */\nstatic struct box *\nhtml_find_live_box_for_node(html_content *c, dom_node *node)\n{\n\tstruct box *root;\n\tstruct box *b;\n\n\tif (c == NULL || node == NULL || c->layout == NULL)\n\t\treturn NULL;\n\troot = c->layout;\n\tb = root;\n\twhile (b != NULL) {\n\t\tif (b->node == node && !(b->flags & CLONE))\n\t\t\treturn b;\n\t\tif (b->children != NULL) {\n\t\t\tb = b->children;\n\t\t\tcontinue;\n\t\t}\n\t\twhile (b != root && b->next == NULL)\n\t\t\tb = b->parent;\n\t\tif (b == root)\n\t\t\tbreak;\n\t\tb = b->next;\n\t}\n\treturn NULL;\n}\n\n'''
hs = hs.replace(marker, helper + marker, 1)

old = '\tif (node == NULL || c == NULL) return -1;\n\tif (c->select_ctx == NULL) return -1;\n\t\n\tif (macos9_paint_gw != NULL || macsurf_reconvert_in_progress != 0 || base_c->active != 0) return -1;\n\n\tbox = box_for_node(node);\n'
new = '\tif (node == NULL || c == NULL) return -1;\n\tif (c->select_ctx == NULL || c->layout == NULL) return -1;\n\tif (base_c->status != CONTENT_STATUS_DONE) return -1;\n\t\n\tif (macos9_paint_gw != NULL || macsurf_reconvert_in_progress != 0 || base_c->active != 0) return -1;\n\n\tbox = html_find_live_box_for_node(c, node);\n'
assert old in hs, 'fast-style preconditions changed'
hs = hs.replace(old, new, 1)

old = '\tif (c == NULL || node == NULL || c->select_ctx == NULL ||\n\t\tc->layout == NULL || macos9_paint_gw != NULL ||\n\t\tmacsurf_reconvert_in_progress != 0 || base_c->active != 0)\n\t\treturn -1;\n\n\troot = box_for_node(node);\n'
new = '\tif (c == NULL || node == NULL || c->select_ctx == NULL ||\n\t\tc->layout == NULL || base_c->status != CONTENT_STATUS_DONE ||\n\t\tmacos9_paint_gw != NULL || macsurf_reconvert_in_progress != 0 ||\n\t\tbase_c->active != 0)\n\t\treturn -1;\n\n\troot = html_find_live_box_for_node(c, node);\n'
assert old in hs, 'inherited preconditions changed'
hs = hs.replace(old, new, 1)
hp.write_text(hs)

rp = Path('browser/netsurf/frontends/macos9/macos9_reconvert.c')
rs = rp.read_text()

marker = '#endif\n\n/*\n * fixes925 (census)'
assert marker in rs, 'content-token section marker missing'
cosmetic = '''#endif\n\n/* stabilization/reconvert-rework: class/style invalidations live in a\n * separate bounded queue. They can never be consumed by flush_now(), so a\n * geometry read cannot turn cosmetic churn into a whole-document rebuild. */\n#define RECONVERT_MAX_COSMETIC 32\n#define RECONVERT_COSMETIC_DEBOUNCE_MS 80\nstruct macos9_cosmetic_pending {\n\tstruct content *c;\n\tunsigned long token;\n\tvoid *node;\n\tint kind;\n};\nstatic struct macos9_cosmetic_pending g_cosmetic[RECONVERT_MAX_COSMETIC];\n\nstatic void\nmacos9_cosmetic_clear(int i)\n{\n\tif (g_cosmetic[i].node != NULL)\n\t\tmacsurf_reconvert_node_unref(g_cosmetic[i].node);\n\tg_cosmetic[i].c = NULL;\n\tg_cosmetic[i].token = 0;\n\tg_cosmetic[i].node = NULL;\n\tg_cosmetic[i].kind = MACOS9_DOMMUT_UNKNOWN;\n}\n\nstatic void\nmacos9_cosmetic_add(struct content *c, void *node, int kind)\n{\n\tint i;\n\tint free_slot = -1;\n\n\tif (c == NULL || node == NULL)\n\t\treturn;\n\tfor (i = 0; i < RECONVERT_MAX_COSMETIC; i++) {\n\t\tif (g_cosmetic[i].c == c && g_cosmetic[i].node == node &&\n\t\t    g_cosmetic[i].kind == kind) {\n\t\t\tg_cosmetic[i].token = macos9_content_token(c);\n\t\t\treturn;\n\t\t}\n\t\tif (g_cosmetic[i].c == NULL && free_slot < 0)\n\t\t\tfree_slot = i;\n\t}\n\t/* Cosmetic overflow is deliberately lossy. Never escalate paint/style\n\t * backlog into the full-document transaction we are trying to remove. */\n\tif (free_slot < 0)\n\t\treturn;\n\tg_cosmetic[free_slot].c = c;\n\tg_cosmetic[free_slot].token = macos9_content_token(c);\n\tg_cosmetic[free_slot].node = macsurf_reconvert_node_ref(node);\n\tg_cosmetic[free_slot].kind = kind;\n}\n\n/*\n * fixes925 (census)'''
rs = rs.replace(marker, cosmetic, 1)

marker = 'static void\nmacos9_reconvert_cb(void *p)\n{\n'
assert marker in rs, 'reconvert callback marker missing'
processor = '''static int\nmacos9_process_cosmetic_pending(void)\n{\n\tint i;\n\tint remain = 0;\n\textern int html_reconvert_fast_style(struct content *c, void *node);\n\textern int html_reconvert_fast_inherited_color(struct content *c,\n\t\tvoid *node);\n\textern struct gui_window *macos9_paint_gw;\n\textern int macsurf_reconvert_in_progress;\n\n\tfor (i = 0; i < RECONVERT_MAX_COSMETIC; i++) {\n\t\tstruct content *c = g_cosmetic[i].c;\n\t\tint rc;\n\t\tif (c == NULL)\n\t\t\tcontinue;\n\t\tif (!macos9_content_is_live(c) ||\n\t\t    !macos9_content_token_valid(c, g_cosmetic[i].token)) {\n\t\t\tmacos9_cosmetic_clear(i);\n\t\t\tcontinue;\n\t\t}\n\t\tif (c->status != CONTENT_STATUS_DONE || c->active != 0 ||\n\t\t    macos9_paint_gw != NULL || macsurf_reconvert_in_progress != 0) {\n\t\t\tremain = 1;\n\t\t\tcontinue;\n\t\t}\n\t\trc = html_reconvert_fast_style(c, g_cosmetic[i].node);\n\t\tif (rc != 0)\n\t\t\trc = html_reconvert_fast_inherited_color(c,\n\t\t\t\tg_cosmetic[i].node);\n\t\t/* Geometry/topology differences intentionally do NOT fall back to\n\t\t * html_reconvert_content(). The DOM remains authoritative and a later\n\t\t * structural rebuild incorporates the change. */\n\t\t(void) rc;\n\t\tmacos9_cosmetic_clear(i);\n\t}\n\treturn remain;\n}\n\n'''
rs = rs.replace(marker, processor + marker, 1)

marker = '\tnow = (unsigned long) TickCount();\n\n\t/* R1.4 - defer diagnostic.'
replacement = '\tnow = (unsigned long) TickCount();\n\n\tif (macos9_process_cosmetic_pending())\n\t\tbusy = 1;\n\n\t/* R1.4 - defer diagnostic.'
assert marker in rs, 'callback start changed'
rs = rs.replace(marker, replacement, 1)

old = '''\t/* stabilization/reconvert-rework: class/style writes are NOT reasons to\n\t * rebuild the entire document.  The hardware A/B proved that the full\n\t * reconvert transaction is the performance/crash multiplier, while the\n\t * recent targeted fast-style experiment itself crashed in\n\t * html_reconvert_fast_style during startup.  For now keep these mutations\n\t * in the persistent DOM and let the next genuine structural mutation fold\n\t * them into its rebuild.  This deliberately trades class/style-only visual\n\t * freshness for a stable baseline while a safe incremental recascade path\n\t * is built.  Do not queue a node ref, callback, or geometry-forced rebuild. */\n\tif (macos9_reconvert_kind_is_cosmetic(kind)) {\n\t\treturn;\n\t}\n'''
new = '''\t/* Class/style writes use the incremental queue only. They never enter the\n\t * full-reconvert pending table and therefore never trigger a document\n\t * rebuild merely because targeted styling declines. */\n\tif (macos9_reconvert_kind_is_cosmetic(kind)) {\n\t\textern int macos9_sched_is_queued(\n\t\t\tvoid (*callback)(void *p), void *p);\n\t\tmacos9_cosmetic_add(c, node, kind);\n\t\tif (!macos9_sched_is_queued(macos9_reconvert_cb, NULL)) {\n\t\t\t(void) macos9_schedule(RECONVERT_COSMETIC_DEBOUNCE_MS,\n\t\t\t\tmacos9_reconvert_cb, NULL);\n\t\t}\n\t\treturn;\n\t}\n'''
assert old in rs, 'structural-only cosmetic gate changed'
rs = rs.replace(old, new, 1)
rp.write_text(rs)

# Force the canonical branch version of the diagnostics TU to be copied by the
# normal changed-file ship workflow, replacing any stale targeted-agent copy on
# the Mac that still references macos9_reconvert_render_stats.
dp = Path('browser/netsurf/frontends/macos9/macsurf_diag.c')
ds = dp.read_text()
marker = '/* All writers run on the cooperative main / notifier context; no locking. */\n'
assert marker in ds, 'diag marker missing'
ds = ds.replace(marker, marker + '/* stabilization/reconvert-rework: canonical diagnostics source sync. */\n', 1)
dp.write_text(ds)
