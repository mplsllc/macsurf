# MacSurf performance tracking

Track load time + page weight **across fixes** so we can see whether each
change helped or hurt. All of this is Linux-side; the data comes from the
G3's `MacSurf Debug.log` (the durable diagnostic channel).

## The loop

1. On the G3: build the fix, load a page (mactrove is the render benchmark),
   quit. The phase stamps + page-weight land in `MacSurf Debug.log`.
2. Pull the log to `forclaude/MacSurf Debug.log` (the usual scp).
3. One command:

   ```sh
   perf/capture.sh "forclaude/MacSurf Debug.log" "fixes377 mactrove"
   ```

   That scrapes the **last** navigation cycle, tags it with the current git
   commit + your label, appends a row to `history.csv`, and regenerates the
   dashboard. Open **`perf/profile.html`** to see the trend.

   Add the URL as a 3rd arg if it isn't mactrove:
   ```sh
   perf/capture.sh "forclaude/MacSurf Debug.log" "fixes377 fb" https://mbasic.facebook.com
   ```

## Files

| File | Role |
|---|---|
| `scrape.py` | log → `history.csv` (one row per nav cycle; auto commit SHA). `--all-cycles` for every cycle, else last only. |
| `report.py` | `history.csv` → `profile.html` (self-contained dashboard: stacked phase bars + page-weight + table). |
| `capture.sh` | the two above in one step. |
| `history.csv` | the data. 15 cols: timestamp, commit, url, label, the phase ms, `total_bytes`, `subresources`, raw stamps. |
| `profile.html` | generated dashboard (regenerated on every capture). |

## What's measured

Phase stamps from `macsurf_profile_stamp` (in `MacSurf Debug.log` as
`[+Nus] label`): `nav-start → tls-handshake-start/done → fetch-finished →
parse-convert-done → cascade-done → layout-done → first-paint-done`, plus
`js-start/js-end`. Page weight (`total_bytes`, `subresources`) comes from the
`PROFILE url=… total_bytes=… subresources=…` line (`macsurf_profile_emit`,
fixes369).

The sequential phases (fetch → parse → cascade → layout → paint) stack to
~`total_ms`; tls is a subset of fetch and js overlaps, so both show in the
table but aren't double-counted in the bar.

> Multi-reflow pages (mactrove's reflow storm, FB) can produce noisy/negative
> per-phase deltas because the scraper takes the latest stamp per phase per
> cycle. `total_ms` stays meaningful; the dashboard skips non-positive phases.
