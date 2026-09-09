# Test Procedure

Apply identically to every build. Do not vary the procedure between builds.

---

## Site 1 — Hacker News (simple control)

URL: `https://news.ycombinator.com/`
Purpose: Determine whether basic navigation/scrolling is globally damaged
or whether the problem requires JS/layout pressure.

1. Fresh MacSurf launch (quit and relaunch between every build/site)
2. Navigate to URL via address bar
3. Wait until page is visibly settled (no spinner, text readable)
4. Scroll: top → bottom → top, twice. Sustain scrolling — don't flick once.
   Observe and record: smooth? jerky? freeze? progressive degradation?
5. Wait 20-30 seconds while idle. Scroll again. Record change if any.
6. Click one ordinary story link
7. Wait for target page to settle
8. Back button
9. Reload once (Cmd-R or reload button)
10. Wait 20 seconds
11. Scroll again

Record all fields in RUN_TEMPLATE.md.

---

## Site 2 — 68kMLA Forum

URL: `https://68kmla.org/bb/`
Also: navigate to one ordinary thread page from the index.

1. Fresh MacSurf launch
2. Navigate to 68kMLA index URL
3. Wait for visible settle
4. Scroll: top → bottom → top, **twice**. Do NOT flick — sustain.
   Observe reconvert activity in log if accessible during run.
5. Wait 20-30 seconds idle
6. Scroll again. Record whether performance changed after idle.
7. Click one thread link from the index
8. Wait for thread page to settle
9. Scroll the thread page once
10. Click back (return to index)
11. Navigate to a second thread
12. Click back again
13. Reload the index (Cmd-R)
14. If logged in: open a post/reply UI (click Post Thread or Reply)
15. Type some text into the title/body fields — do NOT submit
16. Record whether editor appeared and whether typing worked
17. Close/cancel the editor

Record SUBMIT: NOT_TESTED unless the maintainer explicitly authorizes a test post.

---

## Site 3 — Hackaday

URL: Use a specific article URL from prior MacSurf testing (record which one).

1. Fresh MacSurf launch
2. Navigate to article URL
3. Wait for settle
4. Scroll: top → bottom → top, twice
5. Wait 20-30 seconds idle
6. Scroll again — record if scroll performance changed
7. Navigate via a link to a second article
8. Back
9. Reload
10. If the comment/reply UI is visible: click it, type text
11. Record editor appearance and typing behavior

---

## Observations to capture during each run

- Is the first scroll responsive or already jank?
- Does scroll degrade progressively (gets worse over time) or immediately?
- Does a freeze/hang occur? At what point in the workflow?
- Does a crash occur? Capture the MacsBug screen or crash report verbatim.
- What was the last log line before the crash?
- Did the page make visible network requests (fetches completing)?
- Did JS appear to run (page JS-driven UI elements responding)?

---

## What NOT to do

- Do not navigate to more pages than the procedure specifies
- Do not try to work around a crash by reloading — record it and move on
- Do not modify source between runs A through E
- Do not clear caches between sites within the same build (only before each build)
- Do not "help" a struggling build by waiting longer than the procedure specifies
- Do not call a non-crashing build "good" after only one navigation pass —
  the current crash is intermittent; run the full scroll/nav sequence
