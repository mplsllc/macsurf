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

## Site-compatibility benchmark (multi-site, scored)

`bench.py` turns a captured log into a **scored, honest, per-site** record and
regenerates **`perf/site-compatibility.md`**.

> **Not published.** This used to render a public GitHub wiki page. That page was
> withdrawn (2026-07-21): it had gone stale at 2 scored sites, and it reported
> **mactrove.com as grade F "can't load yet"** when mactrove is the project's
> canonical working render target. A stale honest page is still misleading. The
> record is now local-only until a fresh hardware bench run produces real numbers
> across the target list.

```sh
# score the last cycle for a site you just loaded on the G3, then rebuild the page
perf/bench.py capture "forclaude/MacSurf Debug.log" \
    --url https://68kmla.org/bb/ --label "fixes404" --login works

# rebuild the local record from accumulated data only
perf/bench.py render
```

- `perf/sites.csv` — the curated target list (retro + current Mac sites + login flows). Edit freely.
- `perf/bench.csv` — accumulated scored runs (one row per capture; auto commit SHA).
- Scoring is out of 100 from the log alone (render 40 / css 20 / images 15 / speed 15 / clean 10) — transparent, no subjective judgement. A site that can't load is scored `F` and listed honestly, not hidden.
- `--login works|partial|fails` annotates login state per site (the one thing the log can't auto-detect yet).

**The loop:** load each target on the G3 → pull the log → `bench.py capture … --url <site>` → commit. Once the target list has real coverage, the record is worth publishing again — until then it stays in-repo.
