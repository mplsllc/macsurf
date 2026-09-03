#!/usr/bin/env python3
from pathlib import Path

p = Path('browser/netsurf/content/handlers/html/layout.c')
s = p.read_text()

marker = 'static bool layout_multicol_context(\n'
assert marker in s
helper = '''union layout_multicol_work_align {\n\tvoid *p;\n\tdouble d;\n\tlong l;\n};\n\nstatic size_t\nlayout_multicol_work_round(size_t n)\n{\n\tsize_t a = sizeof(union layout_multicol_work_align);\n\tsize_t r = n % a;\n\n\tif (r != 0) {\n\t\tsize_t add = a - r;\n\t\tif (n > (size_t)-1 - add)\n\t\t\treturn 0;\n\t\tn += add;\n\t}\n\treturn n;\n}\n\n'''
if 'union layout_multicol_work_align' not in s:
    s = s.replace(marker, helper + marker, 1)

# Add workspace declarations immediately after the existing six array pointers.
needle = '''\tint *segment_starts;\n\tint *segment_ends;\n\tint *segment_targets;\n'''
assert needle in s
repl = needle + '''\tunsigned char *work;\n\tunsigned char *work_p;\n\tsize_t items_bytes;\n\tsize_t outer_bytes;\n\tsize_t flags_bytes;\n\tsize_t starts_bytes;\n\tsize_t ends_bytes;\n\tsize_t targets_bytes;\n\tsize_t work_size;\n'''
s = s.replace(needle, repl, 1)

old_alloc = '''\titems = malloc(sizeof(struct box *) * child_count);\n\touter_heights = malloc(sizeof(int) * child_count);\n\tspan_all_flags = calloc((size_t)child_count, sizeof(unsigned char));\n\tsegment_starts = malloc(sizeof(int) * child_count);\n\tsegment_ends = malloc(sizeof(int) * child_count);\n\tsegment_targets = malloc(sizeof(int) * child_count);\n\tif (items == NULL || outer_heights == NULL || span_all_flags == NULL ||\n\t\t\tsegment_starts == NULL || segment_ends == NULL ||\n\t\t\tsegment_targets == NULL) {\n\t\tfree(items);\n\t\tfree(outer_heights);\n\t\tfree(span_all_flags);\n\t\tfree(segment_starts);\n\t\tfree(segment_ends);\n\t\tfree(segment_targets);\n\t\treturn false;\n\t}\n'''
assert old_alloc in s
new_alloc = '''\t/* Six short-lived arrays used together for one multicol pass share one\n\t * aligned workspace, avoiding six allocator/free round-trips. */\n\tif ((size_t)child_count > (size_t)-1 / sizeof(struct box *) ||\n\t\t\t(size_t)child_count > (size_t)-1 / sizeof(int))\n\t\treturn false;\n\titems_bytes = layout_multicol_work_round(\n\t\t\t(size_t)child_count * sizeof(struct box *));\n\touter_bytes = layout_multicol_work_round(\n\t\t\t(size_t)child_count * sizeof(int));\n\tflags_bytes = layout_multicol_work_round((size_t)child_count);\n\tstarts_bytes = layout_multicol_work_round(\n\t\t\t(size_t)child_count * sizeof(int));\n\tends_bytes = layout_multicol_work_round(\n\t\t\t(size_t)child_count * sizeof(int));\n\ttargets_bytes = layout_multicol_work_round(\n\t\t\t(size_t)child_count * sizeof(int));\n\tif (items_bytes == 0 || outer_bytes == 0 || flags_bytes == 0 ||\n\t\t\tstarts_bytes == 0 || ends_bytes == 0 || targets_bytes == 0)\n\t\treturn false;\n\twork_size = items_bytes;\n\tif (work_size > (size_t)-1 - outer_bytes) return false;\n\twork_size += outer_bytes;\n\tif (work_size > (size_t)-1 - flags_bytes) return false;\n\twork_size += flags_bytes;\n\tif (work_size > (size_t)-1 - starts_bytes) return false;\n\twork_size += starts_bytes;\n\tif (work_size > (size_t)-1 - ends_bytes) return false;\n\twork_size += ends_bytes;\n\tif (work_size > (size_t)-1 - targets_bytes) return false;\n\twork_size += targets_bytes;\n\n\twork = malloc(work_size);\n\tif (work == NULL)\n\t\treturn false;\n\twork_p = work;\n\titems = (struct box **)work_p;\n\twork_p += items_bytes;\n\touter_heights = (int *)work_p;\n\twork_p += outer_bytes;\n\tspan_all_flags = work_p;\n\tmemset(span_all_flags, 0, (size_t)child_count);\n\twork_p += flags_bytes;\n\tsegment_starts = (int *)work_p;\n\twork_p += starts_bytes;\n\tsegment_ends = (int *)work_p;\n\twork_p += ends_bytes;\n\tsegment_targets = (int *)work_p;\n'''
s = s.replace(old_alloc, new_alloc, 1)

# Collapse every identical cleanup sequence in this function.
free_seq_tabs = '''\t\tfree(items);\n\t\tfree(outer_heights);\n\t\tfree(span_all_flags);\n\t\tfree(segment_starts);\n\t\tfree(segment_ends);\n\t\tfree(segment_targets);\n'''
s = s.replace(free_seq_tabs, '\t\tfree(work);\n')
free_seq_3 = '''\t\t\tfree(items);\n\t\t\tfree(outer_heights);\n\t\t\tfree(span_all_flags);\n\t\t\tfree(segment_starts);\n\t\t\tfree(segment_ends);\n\t\t\tfree(segment_targets);\n'''
s = s.replace(free_seq_3, '\t\t\tfree(work);\n')
free_seq = '''\tfree(items);\n\tfree(outer_heights);\n\tfree(span_all_flags);\n\tfree(segment_starts);\n\tfree(segment_ends);\n\tfree(segment_targets);\n'''
s = s.replace(free_seq, '\tfree(work);\n')

for token in ('items = malloc(sizeof(struct box *) * child_count)',
              'outer_heights = malloc(sizeof(int) * child_count)',
              'span_all_flags = calloc',
              'segment_starts = malloc(sizeof(int) * child_count)',
              'segment_ends = malloc(sizeof(int) * child_count)',
              'segment_targets = malloc(sizeof(int) * child_count)'):
    assert token not in s, token

p.write_text(s)
