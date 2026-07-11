#!/usr/bin/env python3
"""
MacSurf debug-log organizer + lightweight watcher.

Watches the flat inbox  builds/logs/*.txt  (where the web upload endpoint
drops user-submitted "MacSurf Debug.log" files) and, for each settled new
file:

  1. files it into  builds/logs/YYYY/MM/DD/  (date parsed from the filename,
     falling back to the file's mtime),
  2. regenerates  builds/logs/INDEX.md  — a triage table (device, size, NAV /
     RECON / failure counts, first failure line) so you can see at a glance
     which uploads actually matter,
  3. emits a short NOTICE (metadata only, never log contents) to
     builds/logs/NOTICES.log, stdout, and — if configured — a Discord webhook.

Stdlib only. No pip installs, no daemon framework.

  python3 tools/log_organizer.py            # one pass, then exit  (cron-friendly)
  python3 tools/log_organizer.py --watch    # poll loop (default 12s)
  python3 tools/log_organizer.py --watch --interval 20
  python3 tools/log_organizer.py --dir /path/to/logs

Notifications:
  - always appended to builds/logs/NOTICES.log and printed to stdout
  - Discord/Slack-compatible webhook if env MACSURF_LOG_WEBHOOK is set
  - only fires for logs newer than NOTIFY_MAX_AGE (so organizing an old
    backlog is silent)
"""

import os
import re
import sys
import time
import json
import shutil
import argparse
import urllib.request
from datetime import datetime

# ---- config -----------------------------------------------------------------

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_LOGDIR = os.path.join(REPO_ROOT, "builds", "logs")

SETTLE_SECS = 15          # don't touch a file modified within this window
                          # (it may still be uploading)
NOTIFY_MAX_AGE = 3600     # only notify for logs younger than 1h (backlog stays quiet)
POLL_INTERVAL = 12        # --watch default seconds

FNAME_RE = re.compile(
    r"^MacSurfLog_(\d{8})-(\d{6})_(.+?)_([0-9A-Fa-f]{4,})\.txt$")

FAIL_RE = re.compile(
    r"FAIL|ERROR|ASSERT|PANIC|UAF|CORRUPT|NOMEM|ABORT|WATCHDOG|TERMINAL|"
    r"exception|crash", re.IGNORECASE)
NAV_RE = re.compile(r"\bNAV\b")
RECON_RE = re.compile(r"\bRECON\b")
BANNER_RE = re.compile(r"^===.*MacSurf", re.IGNORECASE)

WEBHOOK = os.environ.get("MACSURF_LOG_WEBHOOK", "").strip()


# ---- helpers ----------------------------------------------------------------

def parse_dt(fname, fallback_path):
    """Return a datetime from the filename stamp, else the file mtime."""
    m = FNAME_RE.match(fname)
    if m:
        try:
            return datetime.strptime(m.group(1) + m.group(2), "%Y%m%d%H%M%S")
        except ValueError:
            pass
    return datetime.fromtimestamp(os.path.getmtime(fallback_path))


def device_of(fname):
    m = FNAME_RE.match(fname)
    return m.group(3) if m else "?"


def human_size(n):
    if n < 1024:
        return "%d B" % n
    if n < 1048576:
        return "%.1f KB" % (n / 1024.0)
    return "%.2f MB" % (n / 1048576.0)


def read_lines(path):
    """MacSurf logs are CR-terminated; splitlines() handles CR / LF / CRLF."""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            return fh.read().splitlines()
    except OSError:
        return []


def summarize(path):
    """Cheap triage summary of one log. Never leaks raw body to notifiers."""
    lines = read_lines(path)
    nav = recon = fail = 0
    first_fail = ""
    banner = ""
    for ln in lines:
        if NAV_RE.search(ln):
            nav += 1
        if RECON_RE.search(ln):
            recon += 1
        if FAIL_RE.search(ln):
            fail += 1
            if not first_fail:
                first_fail = ln.strip()[:160]
        if not banner and BANNER_RE.match(ln.strip()):
            banner = ln.strip()[:80]
    return {
        "lines": len(lines),
        "nav": nav,
        "recon": recon,
        "fail": fail,
        "first_fail": first_fail,
        "banner": banner,
    }


def notify(msg):
    """metadata-only notice -> NOTICES.log + stdout + optional webhook."""
    stamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    line = "[%s] %s" % (stamp, msg)
    print(line, flush=True)
    try:
        with open(os.path.join(LOGDIR, "NOTICES.log"), "a", encoding="utf-8") as fh:
            fh.write(line + "\n")
    except OSError:
        pass
    if WEBHOOK:
        try:
            data = json.dumps({"content": msg}).encode("utf-8")
            req = urllib.request.Request(
                WEBHOOK, data=data,
                headers={"Content-Type": "application/json"})
            urllib.request.urlopen(req, timeout=6).read()
        except Exception as exc:  # never let a webhook failure break organizing
            print("  (webhook failed: %s)" % exc, flush=True)


# ---- core -------------------------------------------------------------------

def inbox_files():
    """Top-level *.txt only — the flat dir is the inbox; subdirs are archive."""
    out = []
    try:
        for name in os.listdir(LOGDIR):
            p = os.path.join(LOGDIR, name)
            if name.endswith(".txt") and os.path.isfile(p):
                out.append(name)
    except OSError:
        pass
    return sorted(out)


def archived_logs():
    """Every *.txt under YYYY/MM/DD/ for index building."""
    out = []
    for root, _dirs, files in os.walk(LOGDIR):
        if root == LOGDIR:
            continue
        for f in files:
            if f.endswith(".txt"):
                out.append(os.path.join(root, f))
    return out


def organize_once(quiet=False):
    """Move settled inbox files into date folders; return list of new dests."""
    moved = []
    now = time.time()
    for name in inbox_files():
        src = os.path.join(LOGDIR, name)
        try:
            if now - os.path.getmtime(src) < SETTLE_SECS:
                continue  # possibly still uploading; catch it next pass
        except OSError:
            continue
        dt = parse_dt(name, src)
        ddir = os.path.join(LOGDIR, "%04d" % dt.year,
                            "%02d" % dt.month, "%02d" % dt.day)
        os.makedirs(ddir, exist_ok=True)
        dest = os.path.join(ddir, name)
        if os.path.exists(dest):          # collision: keep both
            base, ext = os.path.splitext(name)
            dest = os.path.join(ddir, "%s_dup%d%s" % (base, int(now), ext))
        try:
            shutil.move(src, dest)
        except OSError as exc:
            print("  (move failed for %s: %s)" % (name, exc), flush=True)
            continue
        moved.append((dest, name, dt))

    if moved:
        rebuild_index()
    if not quiet:
        for dest, name, dt in moved:
            age = time.time() - dt.timestamp()
            if age <= NOTIFY_MAX_AGE:
                s = summarize(dest)
                rel = os.path.relpath(dest, LOGDIR)
                verdict = ("%d FAIL" % s["fail"]) if s["fail"] else "clean"
                notify("New MacSurf log: %s @ %s  |  %s  |  %d lines, "
                       "%d NAV, %d RECON  ->  %s"
                       % (device_of(name), dt.strftime("%Y-%m-%d %H:%M"),
                          verdict, s["lines"], s["nav"], s["recon"], rel))
    return moved


def rebuild_index():
    """Write builds/logs/INDEX.md — newest first, failures flagged."""
    rows = []
    for p in archived_logs():
        name = os.path.basename(p)
        dt = parse_dt(name, p)
        try:
            size = os.path.getsize(p)
        except OSError:
            size = 0
        s = summarize(p)
        rows.append((dt, name, p, size, s))
    rows.sort(key=lambda r: r[0], reverse=True)

    total = len(rows)
    with_fail = sum(1 for r in rows if r[4]["fail"])
    out = []
    out.append("# MacSurf log index")
    out.append("")
    out.append("_Auto-generated by `tools/log_organizer.py`. "
               "%d logs, %d with failures. Updated %s._"
               % (total, with_fail,
                  datetime.now().strftime("%Y-%m-%d %H:%M:%S")))
    out.append("")
    out.append("| When | Device | Fails | NAV | RECON | Lines | Size | File | Notable |")
    out.append("|------|--------|:----:|:---:|:-----:|:-----:|-----:|------|---------|")
    for dt, name, p, size, s in rows:
        rel = os.path.relpath(p, LOGDIR)
        flag = ("**%d** ⚠" % s["fail"]) if s["fail"] else "0"
        notable = s["first_fail"] if s["fail"] else (s["banner"] or "")
        notable = notable.replace("|", "\\|")[:90]
        out.append("| %s | %s | %s | %d | %d | %d | %s | [%s](%s) | %s |"
                   % (dt.strftime("%Y-%m-%d %H:%M"), device_of(name), flag,
                      s["nav"], s["recon"], s["lines"], human_size(size),
                      name[:28] + ("…" if len(name) > 28 else ""),
                      rel, notable))
    try:
        with open(os.path.join(LOGDIR, "INDEX.md"), "w", encoding="utf-8") as fh:
            fh.write("\n".join(out) + "\n")
    except OSError as exc:
        print("  (index write failed: %s)" % exc, flush=True)


# ---- entry ------------------------------------------------------------------

def main():
    global LOGDIR
    ap = argparse.ArgumentParser(description="MacSurf log organizer/watcher")
    ap.add_argument("--dir", default=DEFAULT_LOGDIR,
                    help="log inbox directory (default: repo builds/logs)")
    ap.add_argument("--watch", action="store_true",
                    help="poll loop instead of a single pass")
    ap.add_argument("--interval", type=int, default=POLL_INTERVAL,
                    help="watch poll seconds (default %d)" % POLL_INTERVAL)
    ap.add_argument("--quiet-backlog", action="store_true",
                    help="first pass organizes silently (no notices)")
    ap.add_argument("--reindex", action="store_true",
                    help="force-rebuild INDEX.md and exit (use after manual "
                         "deletions; the index is otherwise refreshed only "
                         "when new logs arrive)")
    args = ap.parse_args()

    LOGDIR = os.path.abspath(args.dir)
    if not os.path.isdir(LOGDIR):
        print("no such log dir: %s" % LOGDIR, file=sys.stderr)
        return 2

    if args.reindex:
        rebuild_index()
        print("reindexed %s" % os.path.join(LOGDIR, "INDEX.md"), flush=True)
        return 0

    if args.watch:
        print("watching %s every %ds (webhook: %s)"
              % (LOGDIR, args.interval, "on" if WEBHOOK else "off"), flush=True)
        first = True
        while True:
            try:
                organize_once(quiet=(first and args.quiet_backlog))
            except Exception as exc:
                print("  (pass error: %s)" % exc, flush=True)
            first = False
            time.sleep(args.interval)
    else:
        organize_once(quiet=args.quiet_backlog)
        # ensure an index exists even if nothing moved this pass
        if not os.path.exists(os.path.join(LOGDIR, "INDEX.md")):
            rebuild_index()
    return 0


if __name__ == "__main__":
    sys.exit(main())
