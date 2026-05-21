# MacSurf Modern Site Gauntlet

A fixed set of real-world pages tested on hardware to triage where MacSurf falls down today and measure visible improvement across sprints.

The goal is **usability**, not pixel perfection: can a user read the page, search, navigate, see images? Not "does it look identical to Chrome."

## How to run

1. Build and launch MacSurf with current sprint binaries.
2. Visit each URL below in order.
3. For each page:
   - Wait for it to finish loading (or visibly stall).
   - Grab a screenshot.
   - Pull `MacSurf Debug.log` from the Desktop.
   - Find the `SITE url="..."` line for that page in the log.
   - Fill in the row in this doc.
4. Note the FIRST CATASTROPHIC BLOCKER (single most disabling thing) per page.

## SITE line format (fixes160a)

Each completed reformat emits one line:

```
SITE url="<url>" boxes=N blk=N inlinec=N inline=N text=N other=N in_w=W in_h=H c_w=W c_h=H img_ok=N img_fail=N
```

HTTP status / body bytes / proxy live in the preceding `http: done body=... status=... ka=...` line for the matching URL, grep up from the SITE line.

## Blocker categories

| Code | Meaning |
|---|---|
| `FETCH_PROXY` | HTTP timeout, proxy failure, body=0, redirect loop, blocking status |
| `MEMORY_IMAGE` | `bitmap create FAIL`, oversized image alloc, decode storm OOM |
| `CSS_LAYOUT` | box convert succeeds but content is unreadable due to missing CSS |
| `JS_REQUIRED` | body loads, meaningful content hidden behind script |
| `REDRAW_CLAMP` | box tree exists but `redraw_visits` near zero, page blank |
| `INPUT_FORM` | page renders but search/input field unusable |
| `HIT_TEST` | visible links/buttons but clicks miss target (overlays, pointer-events) |
| `FONT_TEXT` | text invisible, overlapping, scrambled, wrong family |
| `UNKNOWN` | unclassified |
| `NONE` | page renders and is usable |

## Pages

### 1. MacTrove home (regression baseline)

- **URL:** https://mactrove.com
- **Why:** Existing-working baseline. Any regression here means the sprint broke something already shipped.

| Run | Loaded | Above-fold readable | Search/input usable | Images usable | Scroll usable | First blocker | Screenshot |
|---|---|---|---|---|---|---|---|
| Before fixes160a | | | | | | | |
| After fixes160a | | | | | | | |

SITE line (before): _paste here_
SITE line (after): _paste here_
Notes:

### 2. MacTrove app page (regression baseline)

- **URL:** https://mactrove.com/advanced.html
- **Why:** The probe page used through fixes150-158a. Grids, gradients, transforms, fonts.

| Run | Loaded | Above-fold readable | Search/input usable | Images usable | Scroll usable | First blocker | Screenshot |
|---|---|---|---|---|---|---|---|
| Before fixes160a | | | | | | | |
| After fixes160a | | | | | | | |

SITE line (before):
SITE line (after):
Notes:

### 3. DuckDuckGo Lite (search/input baseline)

- **URL:** https://lite.duckduckgo.com/lite/
- **Why:** No-JS search baseline. Tests form input, navigation, plain HTML rendering.

| Run | Loaded | Above-fold readable | Search/input usable | Images usable | Scroll usable | First blocker | Screenshot |
|---|---|---|---|---|---|---|---|
| Before fixes160a | | | | | | | |
| After fixes160a | | | | | | | |

SITE line (before):
SITE line (after):
Notes:

### 4. DuckDuckGo HTML (form/input baseline)

- **URL:** https://html.duckduckgo.com/html/
- **Why:** Slightly richer than lite, tables, forms, navigation.

| Run | Loaded | Above-fold readable | Search/input usable | Images usable | Scroll usable | First blocker | Screenshot |
|---|---|---|---|---|---|---|---|
| Before fixes160a | | | | | | | |
| After fixes160a | | | | | | | |

SITE line (before):
SITE line (after):
Notes:

### 5. Wikipedia article (long document, images, tables)

- **URL:** https://en.wikipedia.org/wiki/Macintosh
- **Why:** Long text, tables, infobox, references. Real-world reading test. Heavy CSS but minimal JS-only content.

| Run | Loaded | Above-fold readable | Search/input usable | Images usable | Scroll usable | First blocker | Screenshot |
|---|---|---|---|---|---|---|---|
| Before fixes160a | | | | | | | |
| After fixes160a | | | | | | | |

SITE line (before):
SITE line (after):
Notes:

### 6. MDN article (modern CSS/docs layout)

- **URL:** https://developer.mozilla.org/en-US/docs/Web/CSS
- **Why:** Modern Mozilla docs layout. Code blocks, sidebars, navigation, deep CSS use.

| Run | Loaded | Above-fold readable | Search/input usable | Images usable | Scroll usable | First blocker | Screenshot |
|---|---|---|---|---|---|---|---|
| Before fixes160a | | | | | | | |
| After fixes160a | | | | | | | |

SITE line (before):
SITE line (after):
Notes:

### 7. GitHub repo page (modern app layout, code blocks)

- **URL:** https://github.com/mplsllc/macsurf
- **Why:** GitHub is JS-heavy but the noscript fallback should produce a meaningful repo view.

| Run | Loaded | Above-fold readable | Search/input usable | Images usable | Scroll usable | First blocker | Screenshot |
|---|---|---|---|---|---|---|---|
| Before fixes160a | | | | | | | |
| After fixes160a | | | | | | | |

SITE line (before):
SITE line (after):
Notes:

### 8. Simple modern news/blog (text-heavy, images, columns)

- **URL:** https://text.npr.org/
- **Why:** Lightweight text-first news layout. If this fails, the sprint is in trouble. If it works clean, the sprint has shipped a real win.

| Run | Loaded | Above-fold readable | Search/input usable | Images usable | Scroll usable | First blocker | Screenshot |
|---|---|---|---|---|---|---|---|
| Before fixes160a | | | | | | | |
| After fixes160a | | | | | | | |

SITE line (before):
SITE line (after):
Notes:

### 9. Example.com (trivial sanity baseline)

- **URL:** http://example.com
- **Why:** The smallest meaningful page on the web. If this regresses, the sprint is doomed.

| Run | Loaded | Above-fold readable | Search/input usable | Images usable | Scroll usable | First blocker | Screenshot |
|---|---|---|---|---|---|---|---|
| Before fixes160a | | | | | | | |
| After fixes160a | | | | | | | |

SITE line (before):
SITE line (after):
Notes:

## Summary table (filled after each sprint)

| Page | Before | After fixes160a | Blocker fixed | Remaining blocker |
|---|---|---|---|---|
| MacTrove home | ? | ? | | |
| MacTrove app | ? | ? | | |
| DDG Lite | ? | ? | | |
| DDG HTML | ? | ? | | |
| Wikipedia | ? | ? | | |
| MDN | ? | ? | | |
| GitHub | ? | ? | | |
| NPR Text | ? | ? | | |
| example.com | ? | ? | | |

Outcome categories: usable / partial / blocked.

## Acceptance gate

The sprint is accepted only if **at least one real modern page becomes visibly more usable** OR the SITE diagnostics clearly identify the next single dominant blocker with evidence.

A green sprint result is one of:

1. A previously-blocked page is now usable.
2. Multiple pages improve in a measurable axis (img_ok up, boxes complete, readable=yes).
3. SITE data points to a clear next target, e.g. "5 of 9 pages blocked by MEMORY_IMAGE, next sprint tackles image survival."

A red sprint result is:

1. Regression on any of pages 1-4 (the baselines).
2. New crash on any page in the gauntlet.
3. No clear blocker identified after the data.
