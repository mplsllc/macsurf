#!/usr/bin/env bash
# perf/capture.sh — one-command performance capture.
#
# Scrapes a MacSurf Debug.log into perf/history.csv (auto-tagged with the
# current git commit + your label) and regenerates the perf/profile.html
# dashboard. Run this every time you pull a fresh log off the G3.
#
# Usage:
#   perf/capture.sh <log-file> "<label>" [url]
#
# Examples:
#   perf/capture.sh "forclaude/MacSurf Debug.log" "fixes377 mactrove"
#   perf/capture.sh "forclaude/MacSurf Debug.log" "fixes377 fb-login" https://mbasic.facebook.com
#
# url defaults to https://mactrove.com (the render benchmark). The scraper
# records the LAST navigation cycle in the log by default; pass --all-cycles
# to scrape.py directly if you want every cycle.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
LOG="${1:?usage: capture.sh <log-file> \"<label>\" [url]}"
LABEL="${2:-}"
URL="${3:-https://mactrove.com}"

if [ ! -f "$LOG" ]; then
	echo "capture.sh: log not found: $LOG" >&2
	exit 1
fi

python3 "$HERE/scrape.py" "$LOG" --url "$URL" --label "$LABEL"
python3 "$HERE/report.py"
echo "dashboard: file://$HERE/profile.html"
