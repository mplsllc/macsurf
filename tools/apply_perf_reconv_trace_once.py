#!/usr/bin/env python3
from pathlib import Path
import re

p = Path('browser/netsurf/content/handlers/html/box_construct.c')
s = p.read_text()

# Retire crash-hunt-only declarations/counter from this TU. Liveness guards remain.
s = s.replace('extern void macsurf_reconv_pos_set(const char *phase, long seq, long node_ix,\n\t\tconst char *tag);\nextern void macsurf_reconv_pos_flush(void);\nextern long macsurf_free_mem(void);\n', '')
s = re.sub(r'/\* running node index within the CURRENT reconvert.*?\*/\nstatic unsigned long g_reconv_node_ix = 0;\n', '', s, flags=re.S)

# Unused text-box diagnostic global and hot increment.
s = s.replace('/* Diagnostic: count text boxes constructed during DOM->box conversion. */\nlong macos9_box_text_created = 0;\n', '')
s = s.replace('\t\tmacos9_box_text_created++;\n', '')

# Durable tag helper existed only to decorate resolved crash-hunt checkpoints.
s = re.sub(r'/\* fixes895 - copy the node\'s tag/name.*?\n}\n\nstatic void convert_xml_to_box_inner',
           'static void convert_xml_to_box_inner', s, count=1, flags=re.S)

# Batch-enter durable checkpoint.
s = re.sub(r'\n\t/\* fixes901 - durable marker: THIS batch.*?\n\t}\n\n\tdo \{',
           '\n\tdo {', s, count=1, flags=re.S)

# Per-element node counter.
s = s.replace('\n\t\tif (macsurf_reconvert_in_progress)\n\t\t\tg_reconv_node_ix++;\n', '\n')

# First-150 element durable trace/FlushVol block.
s = re.sub(r'\n\t\t/\* fixes897 - per-ELEMENT durable marker.*?\n\t\t}\n\n\t\t\{\n\t\t\tbool bce_ok',
           '\n\t\t{\n\t\t\tbool bce_ok', s, count=1, flags=re.S)

# Failure log depended on the diagnostic node index/tag. Keep failure behavior itself.
s = re.sub(r'\n\t\t\tif \(bce_ok == false\) \{\n\t\t\t\tif \(macsurf_reconvert_in_progress\) \{.*?\n\t\t\t\t}\n\t\t\t\tctx->cb',
           '\n\t\t\tif (bce_ok == false) {\n\t\t\t\tctx->cb', s, count=1, flags=re.S)

# First-150 text durable trace/FlushVol block.
s = re.sub(r'\n\t\t\t\t/\* fixes897 - per-TEXT durable marker.*?\n\t\t\t\t}\n\t\t\t\tif \(box_construct_text',
           '\n\t\t\t\tif (box_construct_text', s, count=1, flags=re.S)

# Completion durable checkpoint/freemem log.
s = re.sub(r'\n\t\t\tif \(macsurf_reconvert_in_progress\) \{\n\t\t\t\t/\* fixes895 - all nodes walked.*?\n\t\t\t}\n\n\t\t\tmacsurf_reconv_style_cache_report',
           '\n\t\t\tmacsurf_reconv_style_cache_report', s, count=1, flags=re.S)

# Reconvert never reaches the cold-load batch-yield path anymore (atomic build),
# but retire the dead diagnostic branch so the old helper can disappear here.
s = re.sub(r'\n\tif \(macsurf_reconvert_in_progress\) \{\n\t\t/\* fixes895 - durable per-batch checkpoint.*?\n\t}\n\tguit->misc->schedule',
           '\n\tguit->misc->schedule', s, count=1, flags=re.S)

# Reset of the retired counter, wherever dom_to_box did it.
s = re.sub(r'\n\s*g_reconv_node_ix\s*=\s*0\s*;\n', '\n', s)

# Catch any accidental leftovers before writing.
for token in ('g_reconv_node_ix', 'reconv_node_tag', 'macos9_box_text_created',
              'macsurf_reconv_pos_set', 'macsurf_reconv_pos_flush', 'macsurf_free_mem'):
    assert token not in s, token + ' still referenced in box_construct.c'

p.write_text(s)
