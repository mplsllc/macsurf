#!/usr/bin/env python3
from pathlib import Path

p = Path('browser/netsurf/content/handlers/html/layout.c')
s = p.read_text()

# Put the alignment helper immediately before the table wrapper. A malloc base
# is suitably aligned for any object; advancing by multiples of this union's
# size keeps each carved array aligned for the types used below.
marker = '/* fixes171 - Watchdog wrapper for layout_table. */\n'
assert marker in s
helper = '''/* Alignment unit for the single-allocation table-layout workspace. */\nunion layout_table_work_align {\n\tvoid *p;\n\tdouble d;\n\tlong l;\n};\n\nstatic size_t\nlayout_table_work_round(size_t n)\n{\n\tsize_t a = sizeof(union layout_table_work_align);\n\tsize_t r = n % a;\n\n\tif (r != 0) {\n\t\tsize_t add = a - r;\n\t\tif (n > (size_t)-1 - add)\n\t\t\treturn 0;\n\t\tn += add;\n\t}\n\treturn n;\n}\n\n'''
if 'union layout_table_work_align' not in s:
    s = s.replace(marker, helper + marker, 1)

# Add workspace bookkeeping at the top of layout_table_inner. Keep all C89
# declarations before statements.
needle = '''\tcss_fixed value = 0;\n\tcss_unit unit = CSS_UNIT_PX;\n'''
assert needle in s
repl = needle + '''\tunsigned char *work;\n\tunsigned char *work_p;\n\tsize_t col_bytes;\n\tsize_t excess_bytes;\n\tsize_t row_span_bytes;\n\tsize_t row_span_cell_bytes;\n\tsize_t xs_bytes;\n\tsize_t work_size;\n'''
s = s.replace(needle, repl, 1)

old_alloc = '''\t/* allocate working buffers */\n\tcol = malloc(columns * sizeof col[0]);\n\texcess_y = malloc(columns * sizeof excess_y[0]);\n\trow_span = malloc(columns * sizeof row_span[0]);\n\trow_span_cell = malloc(columns * sizeof row_span_cell[0]);\n\txs = malloc((columns + 1) * sizeof xs[0]);\n\tif (!col || !xs || !row_span || !excess_y || !row_span_cell) {\n\t\tfree(col);\n\t\tfree(excess_y);\n\t\tfree(row_span);\n\t\tfree(row_span_cell);\n\t\tfree(xs);\n\t\treturn false;\n\t}\n'''
assert old_alloc in s
new_alloc = '''\t/* One temporary allocation instead of five allocator round-trips per\n\t * table layout. Each segment is rounded to a max-alignment-sized unit. */\n\tif (columns > (size_t)-1 / sizeof col[0] ||\n\t\t\tcolumns > (size_t)-1 / sizeof excess_y[0] ||\n\t\t\tcolumns > (size_t)-1 / sizeof row_span[0] ||\n\t\t\tcolumns > (size_t)-1 / sizeof row_span_cell[0] ||\n\t\t\t(size_t)columns + 1 > (size_t)-1 / sizeof xs[0])\n\t\treturn false;\n\tcol_bytes = layout_table_work_round((size_t)columns * sizeof col[0]);\n\texcess_bytes = layout_table_work_round((size_t)columns * sizeof excess_y[0]);\n\trow_span_bytes = layout_table_work_round((size_t)columns * sizeof row_span[0]);\n\trow_span_cell_bytes = layout_table_work_round(\n\t\t\t(size_t)columns * sizeof row_span_cell[0]);\n\txs_bytes = layout_table_work_round(((size_t)columns + 1) * sizeof xs[0]);\n\tif (col_bytes == 0 || excess_bytes == 0 || row_span_bytes == 0 ||\n\t\t\trow_span_cell_bytes == 0 || xs_bytes == 0)\n\t\treturn false;\n\twork_size = col_bytes;\n\tif (work_size > (size_t)-1 - excess_bytes) return false;\n\twork_size += excess_bytes;\n\tif (work_size > (size_t)-1 - row_span_bytes) return false;\n\twork_size += row_span_bytes;\n\tif (work_size > (size_t)-1 - row_span_cell_bytes) return false;\n\twork_size += row_span_cell_bytes;\n\tif (work_size > (size_t)-1 - xs_bytes) return false;\n\twork_size += xs_bytes;\n\n\twork = malloc(work_size);\n\tif (work == NULL)\n\t\treturn false;\n\twork_p = work;\n\tcol = (struct column *)work_p;\n\twork_p += col_bytes;\n\texcess_y = (int *)work_p;\n\twork_p += excess_bytes;\n\trow_span = (unsigned int *)work_p;\n\twork_p += row_span_bytes;\n\trow_span_cell = (struct box **)work_p;\n\twork_p += row_span_cell_bytes;\n\txs = (int *)work_p;\n'''
s = s.replace(old_alloc, new_alloc, 1)

# Every table-layout cleanup currently frees the same five buffers. Collapse
# all such paths inside this source to the workspace free. This exact sequence
# is unique to layout_table_inner.
old_free = '''\t\t\t\t\tfree(col);\n\t\t\t\t\tfree(excess_y);\n\t\t\t\t\tfree(row_span);\n\t\t\t\t\tfree(row_span_cell);\n\t\t\t\t\tfree(xs);\n'''
s = s.replace(old_free, '\t\t\t\t\tfree(work);\n')
old_free2 = '''\tfree(col);\n\tfree(excess_y);\n\tfree(row_span);\n\tfree(row_span_cell);\n\tfree(xs);\n'''
s = s.replace(old_free2, '\tfree(work);\n')

# No old workspace allocation/free pattern may survive.
for token in ('col = malloc(columns * sizeof col[0])',
              'excess_y = malloc(columns * sizeof excess_y[0])',
              'row_span = malloc(columns * sizeof row_span[0])',
              'row_span_cell = malloc(columns * sizeof row_span_cell[0])',
              'xs = malloc((columns + 1) * sizeof xs[0])'):
    assert token not in s, token

p.write_text(s)
