#!/usr/bin/env python3
"""
Scrape phase-stamp lines from MacSurf Debug.log into perf/history.csv.

Looks for lines emitted by macsurf_profile_stamp:
    [+NNNNNus] LABEL

Phase boundaries (labels) tracked:
    nav-start, tls-handshake-start, tls-handshake-done,
    fetch-finished, parse-convert-done, cascade-done,
    layout-done, first-paint-done, js-start, js-end

For each navigation cycle (delimited by nav-start), record the
LATEST timestamp for each phase and append one row to
perf/history.csv with column-deltas as phase durations in
milliseconds.

Usage:
    python3 perf/scrape.py <log-file>
    python3 perf/scrape.py <log-file> --url https://example.com
    python3 perf/scrape.py <log-file> --label "fixes366 baseline"

The script appends to perf/history.csv. Doesn't overwrite.

Example:
    python3 perf/scrape.py "forclaude/MacSurf Debug.log" \
        --url https://mactrove.com --label "fixes366 first run"
"""

import argparse
import csv
import datetime
import os
import re
import subprocess
import sys

STAMP_RE = re.compile(r'\[\+(\d+)us\]\s+([^\r\n]+?)\s*(?=[\r\n\[]|\Z)')

PHASES = [
    'nav-start',
    'tls-handshake-start',
    'tls-handshake-done',
    'fetch-finished',
    'parse-convert-done',
    'cascade-done',
    'layout-done',
    'first-paint-done',
    'js-start',
    'js-end',
]


def _normalize_label(raw):
    """Map the actual label emitted by macsurf_profile_stamp into a
    canonical phase name. The agent landed on more verbose strings
    than the spec called for (e.g. 'nav: launch home' instead of
    'nav-start'); collapse them here so the scraper still works."""
    raw = raw.strip()
    low = raw.lower()
    if low.startswith('nav:') or low == 'nav-start':
        return 'nav-start'
    for p in PHASES:
        if low == p:
            return p
    return None

CSV_COLUMNS = [
    'timestamp',
    'commit_sha',
    'url',
    'label',
    'total_ms',
    'tls_handshake_ms',
    'fetch_ms',
    'parse_ms',
    'cascade_ms',
    'layout_ms',
    'paint_ms',
    'js_ms',
    'raw_stamps_us',
]


def parse_log(path):
    """Yield navigation cycles. Each cycle = dict of {phase: latest_us}."""
    with open(path, 'r', errors='replace') as fp:
        text = fp.read()

    current = None
    for match in STAMP_RE.finditer(text):
        us = int(match.group(1))
        canon = _normalize_label(match.group(2))
        if canon is None:
            continue
        if canon == 'nav-start':
            if current is not None and len(current) > 1:
                yield current
            current = {'nav-start': us}
        elif current is not None:
            current[canon] = us

    if current is not None and len(current) > 1:
        yield current


def cycle_to_row(cycle, commit_sha, url, label):
    """Compute deltas + assemble a CSV row dict."""
    def delta_ms(a, b):
        if a in cycle and b in cycle:
            return round((cycle[b] - cycle[a]) / 1000.0, 3)
        return ''

    total_us = max(cycle.values()) - cycle['nav-start']
    raw = ';'.join('{}={}'.format(k, cycle[k]) for k in PHASES if k in cycle)

    return {
        'timestamp': datetime.datetime.now().isoformat(timespec='seconds'),
        'commit_sha': commit_sha,
        'url': url,
        'label': label,
        'total_ms': round(total_us / 1000.0, 3),
        'tls_handshake_ms': delta_ms('tls-handshake-start', 'tls-handshake-done'),
        'fetch_ms': delta_ms('nav-start', 'fetch-finished'),
        'parse_ms': delta_ms('fetch-finished', 'parse-convert-done'),
        'cascade_ms': delta_ms('parse-convert-done', 'cascade-done'),
        'layout_ms': delta_ms('cascade-done', 'layout-done'),
        'paint_ms': delta_ms('layout-done', 'first-paint-done'),
        'js_ms': delta_ms('js-start', 'js-end'),
        'raw_stamps_us': raw,
    }


def get_commit_sha():
    try:
        out = subprocess.check_output(
            ['git', 'rev-parse', '--short', 'HEAD'],
            cwd=os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
            stderr=subprocess.DEVNULL,
        )
        return out.decode().strip()
    except Exception:
        return 'unknown'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log_path', help='path to MacSurf Debug.log')
    ap.add_argument('--url', default='', help='URL navigated to')
    ap.add_argument('--label', default='', help='free-text label')
    ap.add_argument('--csv', default=None,
                    help='output CSV path (default perf/history.csv)')
    ap.add_argument('--all-cycles', action='store_true',
                    help='record every navigation cycle (default: last only)')
    args = ap.parse_args()

    if args.csv is None:
        args.csv = os.path.join(
            os.path.dirname(os.path.abspath(__file__)),
            'history.csv',
        )

    sha = get_commit_sha()
    cycles = list(parse_log(args.log_path))
    if not cycles:
        print('no navigation cycles found in {}'.format(args.log_path),
              file=sys.stderr)
        return 1

    if not args.all_cycles:
        cycles = cycles[-1:]

    write_header = not os.path.exists(args.csv)
    with open(args.csv, 'a', newline='') as fp:
        writer = csv.DictWriter(fp, fieldnames=CSV_COLUMNS)
        if write_header:
            writer.writeheader()
        for cycle in cycles:
            row = cycle_to_row(cycle, sha, args.url, args.label)
            writer.writerow(row)
            print('appended: total={}ms tls={}ms fetch={}ms parse={}ms '
                  'cascade={}ms layout={}ms paint={}ms js={}ms'.format(
                      row['total_ms'], row['tls_handshake_ms'],
                      row['fetch_ms'], row['parse_ms'], row['cascade_ms'],
                      row['layout_ms'], row['paint_ms'], row['js_ms']))

    return 0


if __name__ == '__main__':
    sys.exit(main())
