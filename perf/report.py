#!/usr/bin/env python3
"""
Generate perf/profile.html -- a self-contained MacSurf performance
dashboard from perf/history.csv.

No external dependencies, no CDN: inline CSS + inline SVG so the page
renders straight from the repo / offline. Re-run after every
scrape (perf/capture.sh does this automatically).

    python3 perf/report.py                # -> perf/profile.html
    python3 perf/report.py --open         # also print the file:// URL

Charts:
  1. Load-time over fixes -- stacked phase breakdown per capture
     (fetch / parse / cascade / layout / paint), one bar per row,
     chronological, labelled by the fix label + commit.
  2. Page weight over fixes -- total_bytes per capture (where measured).
  3. Full capture table.

The sequential phases (fetch -> parse -> cascade -> layout -> paint)
sum to ~total_ms; tls is a subset of fetch and js overlaps, so both are
shown in the table but not double-counted in the stack.
"""

import argparse
import csv
import html
import os

HERE = os.path.dirname(os.path.abspath(__file__))
CSV_PATH = os.path.join(HERE, 'history.csv')
OUT_PATH = os.path.join(HERE, 'profile.html')

# Sequential, non-overlapping phases that stack to ~total_ms.
STACK_PHASES = [
    ('fetch_ms', '#3b82f6', 'fetch (incl TLS)'),
    ('parse_ms', '#8b5cf6', 'parse+convert'),
    ('cascade_ms', '#ec4899', 'CSS cascade'),
    ('layout_ms', '#f59e0b', 'layout'),
    ('paint_ms', '#10b981', 'first paint'),
]

TABLE_COLS = [
    'timestamp', 'commit_sha', 'url', 'label', 'total_ms',
    'tls_handshake_ms', 'fetch_ms', 'parse_ms', 'cascade_ms',
    'layout_ms', 'paint_ms', 'js_ms', 'total_bytes', 'subresources',
]


def load_rows(path):
    if not os.path.exists(path):
        return []
    with open(path, newline='') as fp:
        return list(csv.DictReader(fp))


def fnum(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return 0.0


def short_label(row, idx):
    lab = (row.get('label') or '').strip().strip('"')
    if not lab:
        lab = row.get('commit_sha', '') or '#{}'.format(idx)
    # keep it tight for an axis tick
    if len(lab) > 22:
        lab = lab[:21] + '…'
    return lab


def svg_stacked(rows):
    """Stacked phase-breakdown bar chart, one bar per capture."""
    if not rows:
        return '<p class="empty">No captures yet.</p>'
    W, H = 960, 360
    padL, padR, padT, padB = 56, 16, 16, 96
    plotW, plotH = W - padL - padR, H - padT - padB
    totals = [sum(fnum(r.get(p)) for p, _, _ in STACK_PHASES) for r in rows]
    ymax = max(totals + [fnum(r.get('total_ms')) for r in rows] + [1.0])
    # round ymax up to a nice number
    nice = 1.0
    while nice < ymax:
        nice *= 10
    step = nice / 10.0
    ymax = (int(ymax / step) + 1) * step if step else ymax
    n = len(rows)
    bw = plotW / max(n, 1)
    barw = min(48, bw * 0.62)
    out = ['<svg viewBox="0 0 {} {}" class="chart" role="img">'.format(W, H)]
    # y gridlines + labels
    grid = 5
    for g in range(grid + 1):
        yv = ymax * g / grid
        y = padT + plotH - (yv / ymax * plotH if ymax else 0)
        out.append('<line x1="{}" y1="{:.1f}" x2="{}" y2="{:.1f}" '
                   'class="grid"/>'.format(padL, y, W - padR, y))
        out.append('<text x="{}" y="{:.1f}" class="ytick">{:g}</text>'
                   .format(padL - 6, y + 3, round(yv)))
    out.append('<text x="14" y="{}" class="axis-title" '
               'transform="rotate(-90 14 {})">load time (ms)</text>'
               .format(padT + plotH / 2, padT + plotH / 2))
    # bars
    for i, r in enumerate(rows):
        cx = padL + bw * i + (bw - barw) / 2
        y = padT + plotH
        title = []
        for key, color, name in STACK_PHASES:
            v = fnum(r.get(key))
            if v <= 0:
                continue
            h = v / ymax * plotH if ymax else 0
            y -= h
            out.append('<rect x="{:.1f}" y="{:.1f}" width="{:.1f}" '
                       'height="{:.1f}" fill="{}"><title>{}: {:g} ms</title>'
                       '</rect>'.format(cx, y, barw, h, color,
                                        html.escape(name), v))
            title.append('{}={:g}'.format(name, v))
        # total label above bar
        tot = fnum(r.get('total_ms'))
        out.append('<text x="{:.1f}" y="{:.1f}" class="bartot">{:g}</text>'
                   .format(cx + barw / 2, y - 4, round(tot)))
        # x tick label (rotated)
        lx = cx + barw / 2
        out.append('<text x="{:.1f}" y="{:.1f}" class="xtick" '
                   'transform="rotate(40 {:.1f} {:.1f})">{}</text>'
                   .format(lx, padT + plotH + 12, lx, padT + plotH + 12,
                           html.escape(short_label(r, i))))
    out.append('</svg>')
    # legend
    leg = ['<div class="legend">']
    for _, color, name in STACK_PHASES:
        leg.append('<span class="lg"><i style="background:{}"></i>{}</span>'
                   .format(color, html.escape(name)))
    leg.append('</div>')
    return ''.join(out) + ''.join(leg)


def svg_weight(rows):
    """Page-weight bars (total_bytes) for captures that measured it."""
    wrows = [r for r in rows if fnum(r.get('total_bytes')) > 0]
    if not wrows:
        return ('<p class="empty">No page-weight captures yet '
                '(needs a fixes369+ build that emits the PROFILE line).</p>')
    W, H = 960, 240
    padL, padR, padT, padB = 64, 16, 16, 96
    plotW, plotH = W - padL - padR, H - padT - padB
    ymax = max(fnum(r.get('total_bytes')) for r in wrows)
    ymax = max(ymax, 1.0)
    n = len(wrows)
    bw = plotW / max(n, 1)
    barw = min(48, bw * 0.62)
    out = ['<svg viewBox="0 0 {} {}" class="chart" role="img">'.format(W, H)]
    for g in range(6):
        yv = ymax * g / 5
        y = padT + plotH - yv / ymax * plotH
        out.append('<line x1="{}" y1="{:.1f}" x2="{}" y2="{:.1f}" '
                   'class="grid"/>'.format(padL, y, W - padR, y))
        out.append('<text x="{}" y="{:.1f}" class="ytick">{:.0f}K</text>'
                   .format(padL - 6, y + 3, yv / 1024.0))
    for i, r in enumerate(wrows):
        v = fnum(r.get('total_bytes'))
        cx = padL + bw * i + (bw - barw) / 2
        h = v / ymax * plotH
        y = padT + plotH - h
        out.append('<rect x="{:.1f}" y="{:.1f}" width="{:.1f}" height="{:.1f}" '
                   'fill="#6366f1"><title>{:.1f} KB, {} subresources</title>'
                   '</rect>'.format(cx, y, barw, h, v / 1024.0,
                                    r.get('subresources', '')))
        out.append('<text x="{:.1f}" y="{:.1f}" class="bartot">{:.0f}K</text>'
                   .format(cx + barw / 2, y - 4, v / 1024.0))
        lx = cx + barw / 2
        out.append('<text x="{:.1f}" y="{:.1f}" class="xtick" '
                   'transform="rotate(40 {:.1f} {:.1f})">{}</text>'
                   .format(lx, padT + plotH + 12, lx, padT + plotH + 12,
                           html.escape(short_label(r, i))))
    out.append('</svg>')
    return ''.join(out)


def table(rows):
    out = ['<table><thead><tr>']
    for c in TABLE_COLS:
        out.append('<th>{}</th>'.format(html.escape(c)))
    out.append('</tr></thead><tbody>')
    for r in reversed(rows):  # newest first
        out.append('<tr>')
        for c in TABLE_COLS:
            v = r.get(c, '')
            cls = ' class="num"' if c.endswith('_ms') or c in (
                'total_bytes', 'subresources') else ''
            out.append('<td{}>{}</td>'.format(cls, html.escape(str(v))))
        out.append('</tr>')
    out.append('</tbody></table>')
    return ''.join(out)


def render(rows):
    n = len(rows)
    urls = sorted({(r.get('url') or '').strip() for r in rows if r.get('url')})
    latest = rows[-1] if rows else {}
    css = """
    :root{--bg:#0f172a;--card:#1e293b;--ink:#e2e8f0;--mut:#94a3b8;--line:#334155}
    *{box-sizing:border-box}
    body{margin:0;background:var(--bg);color:var(--ink);
      font:14px/1.5 -apple-system,Segoe UI,Roboto,Helvetica,Arial,sans-serif}
    header{padding:24px 28px;border-bottom:1px solid var(--line)}
    h1{margin:0;font-size:20px} h2{font-size:15px;margin:0 0 8px;color:var(--mut)}
    .sub{color:var(--mut);font-size:13px;margin-top:4px}
    main{padding:20px 28px;max-width:1040px}
    .card{background:var(--card);border:1px solid var(--line);border-radius:12px;
      padding:18px 18px 10px;margin:0 0 22px}
    .chart{width:100%;height:auto;display:block}
    .grid{stroke:var(--line);stroke-width:1}
    .ytick{fill:var(--mut);font-size:11px;text-anchor:end}
    .xtick{fill:var(--mut);font-size:11px;text-anchor:start}
    .bartot{fill:var(--ink);font-size:11px;text-anchor:middle;font-weight:600}
    .axis-title{fill:var(--mut);font-size:12px;text-anchor:middle}
    .legend{display:flex;flex-wrap:wrap;gap:14px;padding:8px 0 2px}
    .lg{display:flex;align-items:center;gap:6px;color:var(--mut);font-size:12px}
    .lg i{width:12px;height:12px;border-radius:3px;display:inline-block}
    .empty{color:var(--mut);padding:24px;text-align:center}
    table{width:100%;border-collapse:collapse;font-size:12px}
    th,td{padding:6px 8px;border-bottom:1px solid var(--line);text-align:left;
      white-space:nowrap}
    th{color:var(--mut);font-weight:600;position:sticky;top:0;background:var(--card)}
    td.num{text-align:right;font-variant-numeric:tabular-nums}
    .scroll{overflow:auto;max-height:520px;border-radius:8px}
    .pill{display:inline-block;background:#334155;color:#cbd5e1;border-radius:999px;
      padding:2px 10px;font-size:12px;margin-right:6px}
    """
    parts = []
    parts.append('<!doctype html><html lang="en"><head><meta charset="utf-8">')
    parts.append('<meta name="viewport" content="width=device-width,'
                 'initial-scale=1">')
    parts.append('<title>MacSurf Performance</title>')
    parts.append('<style>{}</style></head><body>'.format(css))
    parts.append('<header><h1>MacSurf — Performance over fixes</h1>')
    parts.append('<div class="sub">{} capture(s)'.format(n))
    if latest:
        parts.append(' · latest: <b>{}</b> ({})'.format(
            html.escape(short_label(latest, n - 1)),
            html.escape(latest.get('commit_sha', ''))))
    parts.append('</div><div class="sub" style="margin-top:8px">')
    for u in urls:
        parts.append('<span class="pill">{}</span>'.format(html.escape(u)))
    parts.append('</div></header><main>')
    parts.append('<div class="card"><h2>Load time over fixes '
                 '(stacked phases, ms)</h2>{}</div>'.format(svg_stacked(rows)))
    parts.append('<div class="card"><h2>Page weight over fixes</h2>{}</div>'
                 .format(svg_weight(rows)))
    parts.append('<div class="card"><h2>All captures</h2>'
                 '<div class="scroll">{}</div></div>'.format(table(rows)))
    parts.append('</main></body></html>')
    return ''.join(parts)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--csv', default=CSV_PATH)
    ap.add_argument('--out', default=OUT_PATH)
    ap.add_argument('--open', action='store_true')
    args = ap.parse_args()
    rows = load_rows(args.csv)
    with open(args.out, 'w') as fp:
        fp.write(render(rows))
    print('wrote {} ({} captures)'.format(args.out, len(rows)))
    if args.open:
        print('file://{}'.format(os.path.abspath(args.out)))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
