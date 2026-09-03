#!/usr/bin/env python3
from pathlib import Path

bp = Path('browser/netsurf/content/handlers/html/box_construct.c')
dp = Path('browser/netsurf/frontends/macos9/macsurf_debug_log.c')
b = bp.read_text()
d = dp.read_text()

# Gate the reconvert node counter increment; it only feeds crash-hunt markers.
old = '''\t\tif (macsurf_reconvert_in_progress)\n\t\t\tg_reconv_node_ix++;\n'''
new = '''#ifdef MACSURF_VERBOSE_RECONVERT\n\t\tif (macsurf_reconvert_in_progress)\n\t\t\tg_reconv_node_ix++;\n#endif\n'''
assert old in b
b = b.replace(old, new, 1)

# Gate first-150 element durable/log marker block completely.
old = '''\t\tif (macsurf_reconvert_in_progress && g_reconv_node_ix <= 150) {\n\t\t\tmacsurf_debug_log_writef(\n\t\t\t\t"WORK reconvert #%ld: elem node=%ld ptr=%p",\n\t\t\t\t(long) macsurf_reconvert_seq, (long) g_reconv_node_ix,\n\t\t\t\t(void *) ctx->n);\n\t\t\tmacsurf_reconv_pos_set("construct-elem",\n\t\t\t\t(long) macsurf_reconvert_seq, (long) g_reconv_node_ix, "");\n\t\t\tmacsurf_reconv_pos_flush();\n\t\t}\n'''
new = '''#ifdef MACSURF_VERBOSE_RECONVERT\n\t\tif (macsurf_reconvert_in_progress && g_reconv_node_ix <= 150) {\n\t\t\tmacsurf_debug_log_writef(\n\t\t\t\t"WORK reconvert #%ld: elem node=%ld ptr=%p",\n\t\t\t\t(long) macsurf_reconvert_seq, (long) g_reconv_node_ix,\n\t\t\t\t(void *) ctx->n);\n\t\t\tmacsurf_reconv_pos_set("construct-elem",\n\t\t\t\t(long) macsurf_reconvert_seq, (long) g_reconv_node_ix, "");\n\t\t\tmacsurf_reconv_pos_flush();\n\t\t}\n#endif\n'''
assert old in b
b = b.replace(old, new, 1)

# Gate first-150 text marker block.
old = '''\t\t\t\tif (macsurf_reconvert_in_progress &&\n\t\t\t\t\t\tg_reconv_node_ix <= 150) {\n\t\t\t\t\tmacsurf_debug_log_writef(\n\t\t\t\t\t\t"WORK reconvert #%ld: text node=%ld ptr=%p",\n\t\t\t\t\t\t(long) macsurf_reconvert_seq,\n\t\t\t\t\t\t(long) g_reconv_node_ix, (void *) next);\n\t\t\t\t\tmacsurf_reconv_pos_set("construct-text",\n\t\t\t\t\t\t(long) macsurf_reconvert_seq,\n\t\t\t\t\t\t(long) g_reconv_node_ix, "");\n\t\t\t\t\tmacsurf_reconv_pos_flush();\n\t\t\t\t}\n'''
new = '''#ifdef MACSURF_VERBOSE_RECONVERT\n\t\t\t\tif (macsurf_reconvert_in_progress &&\n\t\t\t\t\t\tg_reconv_node_ix <= 150) {\n\t\t\t\t\tmacsurf_debug_log_writef(\n\t\t\t\t\t\t"WORK reconvert #%ld: text node=%ld ptr=%p",\n\t\t\t\t\t\t(long) macsurf_reconvert_seq,\n\t\t\t\t\t\t(long) g_reconv_node_ix, (void *) next);\n\t\t\t\t\tmacsurf_reconv_pos_set("construct-text",\n\t\t\t\t\t\t(long) macsurf_reconvert_seq,\n\t\t\t\t\t\t(long) g_reconv_node_ix, "");\n\t\t\t\t\tmacsurf_reconv_pos_flush();\n\t\t\t\t}\n#endif\n'''
assert old in b
b = b.replace(old, new, 1)

# The RAM marker formatter should also disappear entirely when the hunt is off.
old = '''void\nmacsurf_reconv_pos_set(const char *phase, long seq, long node_ix,\n\t\tconst char *tag)\n{\n\tint pos = 0;\n\tint cap = (int)sizeof(g_reconv_pos);\n\n\tfmt_append_str(g_reconv_pos, cap, &pos, "reconv-pos phase=");\n\tfmt_append_str(g_reconv_pos, cap, &pos, (phase != NULL) ? phase : "(null)");\n\tfmt_append_str(g_reconv_pos, cap, &pos, " seq=");\n\tfmt_append_long(g_reconv_pos, cap, &pos, seq);\n\tfmt_append_str(g_reconv_pos, cap, &pos, " node=");\n\tfmt_append_long(g_reconv_pos, cap, &pos, node_ix);\n\tfmt_append_str(g_reconv_pos, cap, &pos, " tag=");\n\tfmt_append_str(g_reconv_pos, cap, &pos, (tag != NULL) ? tag : "");\n\tg_reconv_pos[pos] = '\\0';\n}\n'''
new = '''void\nmacsurf_reconv_pos_set(const char *phase, long seq, long node_ix,\n\t\tconst char *tag)\n{\n#ifdef MACSURF_VERBOSE_RECONVERT\n\tint pos = 0;\n\tint cap = (int)sizeof(g_reconv_pos);\n\n\tfmt_append_str(g_reconv_pos, cap, &pos, "reconv-pos phase=");\n\tfmt_append_str(g_reconv_pos, cap, &pos, (phase != NULL) ? phase : "(null)");\n\tfmt_append_str(g_reconv_pos, cap, &pos, " seq=");\n\tfmt_append_long(g_reconv_pos, cap, &pos, seq);\n\tfmt_append_str(g_reconv_pos, cap, &pos, " node=");\n\tfmt_append_long(g_reconv_pos, cap, &pos, node_ix);\n\tfmt_append_str(g_reconv_pos, cap, &pos, " tag=");\n\tfmt_append_str(g_reconv_pos, cap, &pos, (tag != NULL) ? tag : "");\n\tg_reconv_pos[pos] = '\\0';\n#else\n\t(void)phase;\n\t(void)seq;\n\t(void)node_ix;\n\t(void)tag;\n#endif\n}\n'''
assert old in d
d = d.replace(old, new, 1)

bp.write_text(b)
dp.write_text(d)
