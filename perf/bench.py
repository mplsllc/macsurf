#!/usr/bin/env python3
"""
MacSurf site-compatibility benchmark.

Turns a captured `MacSurf Debug.log` into a SCORED, honest, per-site record and
regenerates a LOCAL compatibility record (perf/site-compatibility.md). This is
no longer published: the old public wiki page went stale and misreported working
sites as failures, so it was withdrawn until a fresh hardware bench run exists.
Builds on the existing loggers — no new Mac-side code required:

  - PROFILE line  -> total_bytes, subresources           (page weight)
  - SITE line     -> boxes/text/img_ok/img_fail/css_ok…   (render quality)
  - bw_redraw     -> plot_text/plot_rect                  (did it actually paint?)
  - [+Nus] stamps -> total load time + phase breakdown    (speed)
  - "js err [...]" -> script errors                        (JS health)
  - about:query/fetcherror, "https: FAIL … timed out"     (couldn't load)

Two modes:

  # 1. capture: score the LAST nav cycle for a URL and append to bench.csv
  perf/bench.py capture "forclaude/MacSurf Debug.log" \
      --url https://68kmla.org/bb/ --label "fixes404" [--login works]

  # 2. render: rebuild perf/site-compatibility.md from bench.csv + sites.csv
  perf/bench.py render

`capture` auto-runs `render` afterward. Untested sites from perf/sites.csv are
listed honestly as "not yet tested" so the page is a complete, honest picture.

Scoring is deliberately simple and transparent (constants below); a site that
can't load gets a low score, not a missing row — honesty is the point.
"""

import argparse
import csv
import datetime
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BENCH_CSV = os.path.join(HERE, "bench.csv")
SITES_CSV = os.path.join(HERE, "sites.csv")
WIKI_PAGE = os.path.join(HERE, "site-compatibility.md")

# ---- scoring weights (transparent + tunable) -------------------------------
W_RENDER = 40   # did real content lay out + paint
W_CSS    = 20   # stylesheets applied vs dropped
W_IMG    = 15   # images decoded ok
W_SPEED  = 15   # total load time
W_CLEAN  = 10   # no JS errors

CSV_COLS = [
    "timestamp", "commit", "url", "label", "score", "grade", "status", "login",
    "total_ms", "total_bytes", "subresources",
    "boxes", "text", "plot_text", "plot_rect",
    "img_ok", "img_fail", "css_ok", "css_skip", "css_total", "js_err",
    "loaded", "notes",
]


def git_sha():
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "--short", "HEAD"], cwd=ROOT,
            stderr=subprocess.DEVNULL).decode().strip()
    except Exception:
        return ""


def read_log(path):
    """Return the log as a list of event strings (handles the Mac's CR-joined
    single-line format and normal LF logs)."""
    data = open(path, "rb").read().decode("latin1")
    # split on the [NN] tick markers so each event is one entry
    parts = re.split(r"(?=\[\d+\])", data)
    return [p.strip() for p in parts if p.strip()]


INT = r"(-?\d+)"


def _last(events, pat, group=1, cast=int, default=None):
    val = default
    rx = re.compile(pat)
    for e in events:
        m = rx.search(e)
        if m:
            try:
                val = cast(m.group(group))
            except Exception:
                pass
    return val


def extract(events, url):
    """Pull the metrics for the last nav cycle of `url` (or the whole log if
    url is None / not matched). Returns a metrics dict."""
    # Restrict to events from the LAST occurrence of this url onward, so a
    # multi-site session scores the right page. Match on SITE/PROFILE url=.
    start = 0
    if url:
        u = url.rstrip("/")
        for i, e in enumerate(events):
            if ('SITE url="%s' % u) in e or ('PROFILE url=%s' % u) in e \
               or ('REQ GET %s' % u.split("//")[-1]) in e:
                start = i
    win = events[start:]

    m = {}
    # load failure?
    joined = "\n".join(win)
    m["loaded"] = 0 if ("about:query/fetcherror" in joined or
                        "about:fetcherror" in joined) else 1
    if any("https: FAIL" in e and "timed out" in e for e in win):
        # a fetch timed out somewhere in the cycle (may still partial-load)
        m.setdefault("fetch_timeout", 1)

    m["total_bytes"]  = _last(win, r"PROFILE\s+url=\S+\s+total_bytes=" + INT, default="")
    m["subresources"] = _last(win, r"subresources=" + INT, default="")

    m["boxes"]    = _last(win, r'SITE url=.*?\bboxes=' + INT, default=0)
    m["text"]     = _last(win, r'SITE url=.*?\btext=' + INT, default=0)
    m["img_ok"]   = _last(win, r'\bimg_ok=' + INT, default=0)
    m["img_fail"] = _last(win, r'\bimg_fail=' + INT, default=0)
    m["css_ok"]   = _last(win, r'\bcss_ok=' + INT, default=0)
    m["css_skip"] = _last(win, r'\bcss_skip=' + INT, default=0)
    m["css_total"]= _last(win, r'\bcss_total=' + INT, default=0)

    # did it actually paint text? last bw_redraw plot_text/plot_rect
    m["plot_text"] = _last(win, r'bw_redraw done .*?\bplot_text=' + INT, default=0)
    m["plot_rect"] = _last(win, r'bw_redraw done .*?\bplot_rect=' + INT, default=0)

    # JS errors in the cycle
    m["js_err"] = sum(1 for e in win if e.startswith and "js err [" in e)

    # total load time: last first-paint-done minus nav-start (us -> ms)
    nav   = _last(win, r"nav-start=?\s*\]?", default=None)  # fallback below
    fp_us = None
    ns_us = None
    for e in win:
        mm = re.search(r"\[\+(\d+)us\]\s+first-paint-done", e)
        if mm:
            fp_us = int(mm.group(1))
        mm = re.search(r"\[\+(\d+)us\]\s+nav-start", e)
        if mm:
            ns_us = int(mm.group(1))
    if fp_us is not None and ns_us is not None and fp_us >= ns_us:
        m["total_ms"] = round((fp_us - ns_us) / 1000.0, 1)
    elif fp_us is not None:
        m["total_ms"] = round(fp_us / 1000.0, 1)
    else:
        m["total_ms"] = ""
    return m


def score(m):
    """Return (score 0-100, grade, status). A site that can't load scores low
    but is still recorded — honesty over a pretty number."""
    if not m.get("loaded", 1):
        return 0, "F", "Can't load yet"

    s = 0.0
    boxes = m.get("boxes") or 0
    text  = m.get("text") or 0
    pt    = m.get("plot_text") or 0
    # render
    if boxes >= 15 and text >= 3 and pt >= 3:
        s += W_RENDER
    elif boxes >= 5 and (text >= 1 or pt >= 1):
        s += W_RENDER * 0.6
    elif boxes >= 1:
        s += W_RENDER * 0.25
    # css
    cok, csk = (m.get("css_ok") or 0), (m.get("css_skip") or 0)
    if cok + csk > 0:
        s += W_CSS * cok / float(cok + csk)
    elif cok > 0:
        s += W_CSS
    # images
    iok, ifa = (m.get("img_ok") or 0), (m.get("img_fail") or 0)
    if iok + ifa > 0:
        s += W_IMG * iok / float(iok + ifa)
    elif boxes >= 5:
        s += W_IMG * 0.7   # no images is fine; don't over-penalize
    # speed
    t = m.get("total_ms")
    if isinstance(t, (int, float)) and t > 0:
        if t < 4000:    s += W_SPEED
        elif t < 10000: s += W_SPEED * 0.7
        elif t < 25000: s += W_SPEED * 0.4
        elif t < 60000: s += W_SPEED * 0.15
    elif t == "":
        s += W_SPEED * 0.4   # unknown; partial credit
    # clean
    je = m.get("js_err") or 0
    if je == 0:   s += W_CLEAN
    elif je <= 2: s += W_CLEAN * 0.5

    s = int(round(s))
    if   s >= 85: g, st = "A", "Excellent"
    elif s >= 70: g, st = "B", "Good"
    elif s >= 50: g, st = "C", "Usable"
    elif s >= 30: g, st = "D", "Rough"
    elif s >= 1:  g, st = "E", "Barely"
    else:         g, st = "F", "Can't load yet"
    return s, g, st


def cmd_capture(args):
    events = read_log(args.logfile)
    m = extract(events, args.url)
    sc, gr, st = score(m)
    row = {
        "timestamp": datetime.datetime.now().replace(microsecond=0).isoformat(),
        "commit": git_sha(), "url": args.url, "label": args.label or "",
        "score": sc, "grade": gr, "status": st, "login": args.login or "",
        "total_ms": m.get("total_ms", ""), "total_bytes": m.get("total_bytes", ""),
        "subresources": m.get("subresources", ""),
        "boxes": m.get("boxes", 0), "text": m.get("text", 0),
        "plot_text": m.get("plot_text", 0), "plot_rect": m.get("plot_rect", 0),
        "img_ok": m.get("img_ok", 0), "img_fail": m.get("img_fail", 0),
        "css_ok": m.get("css_ok", 0), "css_skip": m.get("css_skip", 0),
        "css_total": m.get("css_total", 0), "js_err": m.get("js_err", 0),
        "loaded": m.get("loaded", 1), "notes": args.notes or "",
    }
    new = not os.path.exists(BENCH_CSV)
    with open(BENCH_CSV, "a", newline="") as f:
        w = csv.DictWriter(f, fieldnames=CSV_COLS)
        if new:
            w.writeheader()
        w.writerow(row)
    print("scored %s -> %d/%s (%s)" % (args.url, sc, gr, st))
    cmd_render(args)


def load_bench():
    rows = []
    if os.path.exists(BENCH_CSV):
        with open(BENCH_CSV) as f:
            rows = list(csv.DictReader(f))
    return rows


def load_sites():
    sites = []
    if os.path.exists(SITES_CSV):
        with open(SITES_CSV) as f:
            sites = list(csv.DictReader(f))
    return sites


GRADE_EMOJI = {"A": "🟢", "B": "🟢", "C": "🟡", "D": "🟠", "E": "🔴", "F": "⚫"}
LOGIN_EMOJI = {"works": "✅", "partial": "⚠️", "fails": "❌", "": "—", "n/a": "—"}


def cmd_render(_args):
    rows = load_bench()
    sites = load_sites()
    # latest result per url
    latest = {}
    for r in rows:
        latest[r["url"]] = r   # csv is append-order; last wins

    def fmt(r):
        sc = int(r["score"]); g = r["grade"]
        emo = GRADE_EMOJI.get(g, "")
        login = LOGIN_EMOJI.get((r.get("login") or "").strip(), "—")
        cant = str(r.get("loaded", "1")) == "0"   # fetch failed -> metrics are the error page
        t = r.get("total_ms", "")
        speed = ("—" if cant else
                 (("%.1fs" % (float(t) / 1000.0)) if t not in ("", None) else "—"))
        wt = r.get("total_bytes", "")
        weight = ("—" if cant else
                  (("%.0f KB" % (int(wt) / 1024.0)) if str(wt).isdigit() else "—"))
        boxes_disp = "—" if cant else (r.get("boxes", "") or "—")
        return ("| [%s](%s) | %s **%d** %s | %s | %s | %s | %s | %s | %s | %s |"
                % (short(r["url"]), r["url"], emo, sc, g, r["status"],
                   login, speed, weight, boxes_disp,
                   r.get("commit", ""), (r.get("label") or "")))

    # group rows by category from sites.csv; unknown urls -> "other"
    cat_of = {s["url"].rstrip("/"): s["category"] for s in sites}
    note_of = {s["url"].rstrip("/"): s.get("notes", "") for s in sites}
    order = ["retro", "current", "login", "other"]
    buckets = {k: [] for k in order}

    seen = set()
    for url, r in sorted(latest.items(), key=lambda kv: -int(kv[1]["score"])):
        c = cat_of.get(url.rstrip("/"), "other")
        buckets.setdefault(c, []).append(r)
        seen.add(url.rstrip("/"))

    out = []
    out.append("# Site Compatibility\n")
    out.append("_How well MacSurf renders the real web, scored and tracked over "
               "time. Honest: sites that don't work yet are listed too, so you "
               "can watch them improve._\n")
    out.append("> Auto-generated by `perf/bench.py` from captured "
               "`MacSurf Debug.log` runs on a G3 iMac (Mac OS 9.2.2). "
               "Last updated: %s.\n"
               % datetime.date.today().isoformat())

    out.append("## How the score works\n")
    out.append("Each load is scored out of 100 from the diagnostic log — no "
               "subjective judgement:\n")
    out.append("| Weight | What it measures | From |\n|---:|---|---|")
    out.append("| %d | Content actually laid out **and painted** | `SITE boxes/text` + `bw_redraw plot_text` |" % W_RENDER)
    out.append("| %d | Stylesheets applied vs dropped | `SITE css_ok/css_skip` |" % W_CSS)
    out.append("| %d | Images decoded ok | `SITE img_ok/img_fail` |" % W_IMG)
    out.append("| %d | Load time | `[+Nus]` first-paint timing |" % W_SPEED)
    out.append("| %d | No JavaScript errors | `js err` lines |" % W_CLEAN)
    out.append("\nGrades: 🟢 A/B (85+/70+) · 🟡 C (50+) · 🟠 D (30+) · 🔴 E (1+) · "
               "⚫ F (can't load yet). Login: ✅ works · ⚠️ partial · ❌ no · — untested.\n")

    titles = {"retro": "Retro / vintage-Mac community sites",
              "current": "Current general-web sites",
              "login": "Login flows", "other": "Other"}
    for c in order:
        rs = buckets.get(c) or []
        if not rs:
            continue
        out.append("## %s\n" % titles.get(c, c.title()))
        out.append("| Site | Score | Status | Login | Load | Weight | Boxes | Build | Notes |")
        out.append("|---|---|---|:--:|--:|--:|--:|---|---|")
        for r in rs:
            out.append(fmt(r))
        out.append("")

    # untested target sites (honesty: show the whole roadmap)
    untested = [s for s in sites if s["url"].rstrip("/") not in seen]
    if untested:
        out.append("## Not yet tested\n")
        out.append("_On the target list, no capture yet._\n")
        out.append("| Site | Category | Login expected | Notes |\n|---|---|:--:|---|")
        for s in untested:
            out.append("| [%s](%s) | %s | %s | %s |"
                       % (short(s["url"]), s["url"], s["category"],
                          {"yes": "✅", "maybe": "⚠️", "no": "—"}.get(s.get("login", ""), "—"),
                          s.get("notes", "")))
        out.append("")

    out.append("---\n")
    out.append("Want a site added or re-tested? Open an "
               "[issue](https://github.com/mplsllc/macsurf/issues). Raw data: "
               "`perf/bench.csv`; target list: `perf/sites.csv`.")

    os.makedirs(os.path.dirname(WIKI_PAGE), exist_ok=True)
    open(WIKI_PAGE, "w").write("\n".join(out) + "\n")
    print("wrote %s (%d sites scored, %d untested)"
          % (WIKI_PAGE, len(latest), len(untested)))


def short(url):
    s = re.sub(r"^https?://", "", url).rstrip("/")
    return s[:38]


def main():
    ap = argparse.ArgumentParser(description="MacSurf site-compatibility benchmark")
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("capture", help="score a log capture + append to bench.csv")
    c.add_argument("logfile")
    c.add_argument("--url", required=True)
    c.add_argument("--label", default="")
    c.add_argument("--login", default="", choices=["", "works", "partial", "fails", "n/a"])
    c.add_argument("--notes", default="")
    c.set_defaults(func=cmd_capture)
    r = sub.add_parser("render", help="rebuild perf/site-compatibility.md")
    r.set_defaults(func=cmd_render)
    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
