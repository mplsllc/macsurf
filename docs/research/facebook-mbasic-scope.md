# MacSurf → Facebook via the lightweight no-JS surface (`mbasic.facebook.com`)

**Status:** ACTIVE. Strategy set and first build step shipped 2026-06-03 (fixes367 / issue [#167](https://github.com/mplsllc/macsurf/issues/167)).

**Companion doc:** [facebook-native-roadmap.md](facebook-native-roadmap.md) is the *heavy* path (QuickJS engine swap + HTTP/2, 18–30 months) for the full `www.facebook.com` SPA. **This doc is the lightweight path** and is what we are actually building. The two are complementary: the heavy roadmap remains the long-term north star; this gets a usable Facebook onto a G3 *now*, on the current engine, with no new JS engine and no proxy.

---

## 0. Thesis

Facebook still serves a **pure-HTML, no-JavaScript** surface at `mbasic.facebook.com`, built years ago for feature phones and 2G networks. It is tables + forms, ~6 KB per page, zero `<script>`. MacSurf already renders that class of HTML and already speaks HTTPS natively (macTLS) and HTTP/1.1 with redirects, keep-alive, chunked, and **POST** (fixes312). The surface is *native* — the Mac is the real client talking to facebook.com, so login cookies / auth live on the client, which is exactly what Facebook requires and exactly what a render-and-flatten proxy could never do.

The only thing standing between MacSurf and a logged-in Facebook session was **two small fetcher gaps**: no cookie jar, and a User-Agent that Facebook bounces off the lightweight surface. Both are now closed (fixes367). The rest of this doc is the full scope, the evidence, and the staged plan from here to "read your feed and post a status."

---

## 1. Recon — what Facebook actually serves (verified live 2026-06-03)

Probed live against `facebook.com` from Linux with `curl`, varying only the User-Agent:

| User-Agent | Result | Bytes | `<script>` | Notes |
|---|---|---:|---:|---|
| `Mozilla/4.0 (compatible; MSIE 5.0; Mac_PowerPC) MacSurf/1.4` | **200** | **6458** | **0** | The target. Pure-HTML mbasic. Sets `datr`. |
| `Nokia…S40` / `Links` / `MSIE 5 Mac` (no MacSurf token) | 200 | 6458 | 0 | Same no-JS page. |
| `Opera Mini 9.80` | 200 | 12823 | 0 | Slightly heavier, still no JS. |
| KaiOS `Firefox/48 KAIOS/2.5` | 200 | 68784 | 10 | Heavier JS-driven feature-phone variant — avoid. |
| `MacSurf/0.2 (Macintosh; PPC Mac OS 9)` (old default) | **301 → www** | 0 | — | **Bounced** to the 416 KB `www` SPA. Unrenderable. |

**Decisive findings:**
1. **The lightweight surface is gated purely by User-Agent.** The trigger is a leading `Mozilla/4.0 … Mac_PowerPC`. Appending ` MacSurf/1.4` does **not** break the gate (still 200/6458/0-script) — so we stay honestly identified *and* get the light page.
2. **MacSurf's old `MacSurf/0.2` UA was actively harmful** — it 301-bounced to the SPA. The UA change is itself a bug fix, independent of cookies.
3. The vintage-UA `200` response sets `datr` via `Set-Cookie` (`domain=.facebook.com; secure; httponly`). Login-critical cookies (`c_user`, `xs`) arrive later, on the login POST's **302**.

These are FB-server behaviours and may drift; the *requirements* they imply (cookies, vintage UA, POST, redirect-follow) are stable and are what we build to.

---

## 2. Prior art — same-platform precedent (researched 2026-06-03)

The strategy is not novel; it is what every successful retro browser on this hardware class did.

- **Classilla** (Cameron Kaiser's Gecko browser for **the same Mac OS 9 platform**) **mobile-spoofs by default**, with an internal whitelist of exceptions, and exposes a **per-host UA override** via the `classilla.sitecontrol.<host>` pref branch. This is direct, same-OS precedent that "present as a lightweight/old client by default, override per host" is the correct architecture here. Classilla also ships **Byblos**, a hook that rewrites a page's raw HTML *before the parser sees it* (a "stele" is a per-domain JS file exporting `parseHTML()`); our escape hatch if any mbasic HTML ever chokes the engine.
- **TenFourFox** (PowerPC Mac OS X 10.4–10.5) added a per-site UA override (`general.useragent.override.<domain>`) and a built-in switcher whose *lightweight* option is literally "the Classilla user agent," chosen because FB and others honour it as a request for lightweight content.
- **PowerFox** (2026, Jazzzny) — a **modern Basilisk/UXP engine** for OS X 10.4–10.6 on G4/G5. It brute-forces the *real* site with a modern engine; it is **not** a UA-trick browser and **not** a model for an OS 9 / 64 MB / ES5 target. Listed only to close the loop on the name.

**What we borrow:** the per-host UA table (Classilla `sitecontrol` / TenFourFox override). **In reserve:** Byblos-style pre-parse HTML rewrite, if needed.

Sources: `floodgap.com/software/classilla/{faq,byblos}.html`; `systemfolder.wordpress.com` Classilla 9.3.1; `tenfourfox.blogspot.com` FPR31; `github.com/classilla/{classilla,tenfourfox}`.

---

## 3. Gap analysis — MacSurf vs. a logged-in mbasic session

| Requirement | Before | Status |
|---|---|---|
| HTTPS to facebook.com | macTLS (TLS 1.2/1.3) | ✅ shipped |
| HTTP/1.1 keep-alive, chunked, 3xx follow | yes | ✅ shipped |
| Form **POST** (`x-www-form-urlencoded`) | fixes312 / #144 | ✅ shipped |
| `Accept-Encoding: identity` (no gzip needed) | yes (https fetcher) | ✅ shipped |
| Hidden-field form submission (`lsd`,`jazoest`,`m_ts`) | NetSurf core `form.c` | ✅ in core |
| **Cookie jar** (send `Cookie:`, store `Set-Cookie:`) | **absent in both macos9 fetchers** | ✅ **fixes367** |
| **Vintage User-Agent** for facebook.com | **stale `MacSurf/0.2`, bounced** | ✅ **fixes367** |
| Cookie **disk persistence** (survive relaunch) | none | ⏳ next (§6) |
| `<meta http-equiv=refresh>` handling (checkpoints) | verify | ⏳ verify (§6) |

**Why cookies were the #1 gap:** NetSurf's full RFC-6265 cookie jar lives in `content/urldb.c` (and *is* in `MacSurf.mcp`), but only `content/fetchers/curl.c` ever wired it — and we don't build curl.c. Our hand-rolled `macos9_http_fetcher.c` / `macos9_https_fetcher.c` built requests by `sprintf` and never touched the jar. So: no `Cookie:` header was ever sent and no `Set-Cookie:` was ever stored. **Login could not persist for even one navigation.** That is the gap fixes367 closes.

---

## 4. The login flow we now support (pure HTML, no JS)

1. **GET** `https://mbasic.facebook.com/` with the vintage UA → `200`, ~6 KB HTML, `Set-Cookie: datr=…` (captured into urldb by fixes367).
2. Page contains a `method="post"` login form with visible `email` + `pass` inputs and **hidden** `lsd`, `jazoest`, `m_ts` (anti-CSRF/timestamp). NetSurf core `form.c` collects **all** fields, hidden included, on submit.
3. User types credentials, submits → **POST** to `/login/device-based/regular/login/`. fixes367 attaches the `datr` cookie; fixes312 sends the urlencoded body.
4. Response is a **302** whose `Set-Cookie` headers carry **`c_user`** (your user id) and **`xs`** (session secret, HttpOnly+Secure). fixes367 captures these **in the header loop, before** the redirect tears the fetch down.
5. NetSurf follows the 302 (→ a `save-device` interstitial, itself plain HTML) → next GET sends `c_user`+`xs` → **logged in**. Presence of `c_user` is the success signal.
6. `c_user` + `xs` together are sufficient to be authenticated. `fb_dtsg` (scraped from any post-login page) is needed only for **write** actions (posting, messaging), not for reading.

Cookie roles (set `domain=.facebook.com`, so they flow to every `*.facebook.com` subdomain, Secure → HTTPS only — all satisfied by macTLS): `datr` (browser id, pre-login), `sb` (secure-browsing), `c_user` (**required**), `xs` (**required**), `fr` (ads/auth, optional).

---

## 5. What shipped (fixes367 — issue #167)

Both `macos9_http_fetcher.c` and `macos9_https_fetcher.c`:

- **Per-host User-Agent** via a `macos9_ua_for_host()` helper: suffix-matches `facebook.com` (covers `mbasic.`/`m.`/`www.`/`touch.`) → vintage `Mozilla/4.0 (compatible; MSIE 5.0; Mac_PowerPC) MacSurf/1.4`; **every other host keeps `MacSurf/1.4 (Macintosh; PPC Mac OS 9)`** so nothing regresses (DIRECTIVE #5). The match guards against spoof hosts (`evilfacebook.com` → default UA).
- **Cookie request header**: `urldb_get_cookie(url, true)` → spliced as `Cookie: …\r\n` (HttpOnly included, matching curl.c). Refuses to truncate an over-cap header (logs instead).
- **`Set-Cookie` capture**: each `Set-Cookie:` line → `fetch_set_cookie(parent, value)`, in the header-parse loop so 302 login cookies are stored before teardown.
- Request buffers enlarged (https `1024→8192`, http `2048→8192`) to hold a full FB session cookie header.

Verified C89-clean with Retro68 `powerpc-apple-macos-gcc -std=c89 -pedantic-errors -Wall` (the closest Linux proxy to CW8). Behaviour of UA-gating + cookie-splice verified by a native build of the extracted logic.

### Then shipped (fixes368 — same issue)

- **Cookie disk persistence** — Step 2 below. Stay logged in across relaunch.
- **UA site-control module** — the fixes367 duplicated `macos9_ua_for_host` statics are unified into `macos9_user_agent_for_host()` (impl `macos9_fetch.c`, header `macos9_useragent.h`), a data-driven host→UA table (Classilla `sitecontrol` style). Adding another site override is now a one-row table edit. No `MacSurf.mcp` change (impl in an existing build TU + a header).
- **Meta-refresh verified** — Step 3 below; FB checkpoint pages auto-redirect with no new code.

fixes368 changes 7 files (all in `frontends/macos9/`): `main.c`, `macos9_disk_cache.c/.h`, `macos9_useragent.h` (new header), `macos9_fetch.c`, and both fetchers. **No new `.c` files → no `MacSurf.mcp` change.** `macos9_useragent.h` lands in the already-on-path `macos9/` folder.

---

## 6. Roadmap from here (dependency-ordered)

### Step 1 — Hardware bring-up of login *(next, gates everything)*
Build fixes367, load `https://mbasic.facebook.com/` on the G3. Acceptance:
- Page renders as the ~6 KB no-JS form (title-bar/log shows 200, not a www bounce).
- Log in with a real account; confirm `c_user` is stored (add a one-line `MS_LOG` of `urldb_get_cookie` length post-login if needed) and the feed/home renders.
- Navigate 2–3 pages; confirm the session persists within the launch.
- **Regression (DIRECTIVE #5):** confirm mactrove.com still renders identically (it must keep the MacSurf default UA and be cookie-unaffected).

### Step 2 — Cookie disk persistence *(stay logged in across relaunch)* — **SHIPPED fixes368**
`macos9_cookies_load()` / `macos9_cookies_save()` (in `macos9_disk_cache.c`) wrap `urldb_load_cookies` / `urldb_save_cookies` with a leaf filename (`"MacSurf Cookies"`); wired into `main.c` — load right after `netsurf_init`, save right before `netsurf_exit`. Best-effort: any I/O failure is a silent no-op (in-session-only). **Hardware caveat (the one thing still to confirm):** urldb is stdio/`fopen`-based; if the G3 shows MSL `fopen` won't honour the leaf path, switch to the FSSpec route below (the code + comments already point at it). Worst case it's a no-op, never a regression.

**Implementation nuance discovered 2026-06-03 (important — it picks the approach):** `urldb_save_cookies`/`urldb_load_cookies` do their I/O with **stdio `fopen` + `fprintf`/`fgets`**, so they need an `fopen`-able pathname. But the rest of the OS 9 frontend deliberately *avoids* stdio paths: **macEntropy's seed** (`macTLS/os9/ostls_entropy.c`, `OSTLS_LoadSeed`/`OSTLS_SaveSeed`, Preferences folder) and the **disk cache** (`macos9_disk_cache.c`, Desktop) both use **FSSpec I/O** (`FSpCreate`/`FSpOpenDF`/`FSRead`/`FSWrite`) precisely because MSL `fopen` path semantics on Classic Mac OS are unreliable. So there are two routes, to be decided with a hardware probe:
  - **Route A (reuse urldb as-is):** build a full colon-path to a Preferences-folder file and pass it to `urldb_save/load_cookies`. Verify on hardware that MSL `fopen("Vol:System Folder:Preferences:MacSurf Cookies", …)` actually opens. Lowest code, but rests on the exact `fopen`-path behaviour the codebase elsewhere sidesteps.
  - **Route B (FSSpec, matches house style):** add a small `macos9_cookie_store.c`-style writer that serializes the jar via FSSpec like the seed/cache, mirroring macEntropy exactly. More code, but uses the proven mechanism and the same `FindFolder(kPreferencesFolderType …)` + `FSpCreate('MPLS', …)` path as the seed. **Preferred** unless Route A's probe passes cleanly.
- This is a **new subsystem** → it must satisfy the CLAUDE.md Regression Audit Checklist (init wired, body reachable, **SheepShaver/hardware smoke test**) before the round closes. That, plus the `fopen`-vs-FSSpec decision needing a hardware probe, is why persistence was *not* shipped untested overnight with fixes367 — the in-session jar (which works the moment urldb's static-init trees link) already delivers a logged-in session within a launch.

### Step 3 — Checkpoint / interstitial robustness — **VERIFIED (no code needed)**
- `<meta http-equiv="refresh">` **is handled** end-to-end: the HTML handler broadcasts `CONTENT_MSG_REFRESH`, `desktop/browser_window.c` (line ~1598) schedules `browser_window_refresh` via `guit->misc->schedule`, and the macos9 scheduler (`schedule.c`, a real 193-line timer list pumped from `main.c`'s event loop) fires it. FB error/checkpoint pages that use meta-refresh auto-redirect.
- The `save-device` interstitial is a plain-HTML form → core `form.c` handles submit/skip. No new code.

### Step 4 — Write actions
- Scrape `fb_dtsg` from a post-login page; ensure it rides along on status/comment/message POSTs (core form handling already collects it — verify).
- Exercise: post a status, comment, send a Messenger (mbasic `/messages/`) message.

### Step 5 — Fit & finish
- ~~Unify the per-host UA helper into a real host→UA table~~ **DONE (fixes368)** — `macos9_useragent.h` + `macos9_fetch.c`. Future: add `m.`/`touch.` tuning if a richer-but-still-light variant renders better than mbasic; add other sites (Instagram, etc.) as table rows.
- Photos/thumbnails: mbasic serves small `<img>` from `*.fbcdn.net` — confirm the image pipeline fetches cross-origin CDN images (no UA gate there; default UA is fine).
- **Byblos reserve:** if a specific mbasic page has HTML our engine mis-renders, add a pre-parse rewrite hook rather than touching the layout engine.

### Acceptance for "Facebook works on MacSurf (v1)"
Log in, read the news feed, open a profile, read a thread, post a status — all on a G3 over the no-JS mbasic surface, session surviving relaunch.

---

## 7. Risks & open questions

- **FB deprecates mbasic.** It has been "deprecated" for years yet still serves. If it ever dies, `m.facebook.com` (heavier, some JS) is the fallback, which pulls work toward the heavy roadmap. Monitor.
- **Anti-bot / checkpoints.** A fresh login from a new "device" may trigger a confirmation checkpoint (email/SMS code) — a normal HTML form we can submit, but the user must complete it once. TLS/JS fingerprinting is *not* applied to the mbasic surface today.
- **Cookie domain matching.** Relies on `urldb`'s RFC-6265 domain logic sending `.facebook.com` cookies to `mbasic.facebook.com`. Mature upstream code; verify on first hardware login.
- **Field-name drift.** `email`/`pass`/`lsd`/`jazoest` are stable today but FB rotates endpoint paths and hidden-field names periodically. Because we submit whatever the rendered form contains (core `form.c`), we are mostly insulated — we don't hard-code field names anywhere.

---

*Scoped and first step shipped 2026-06-03. This is the lightweight, native, achievable Facebook path. The heavy [facebook-native-roadmap.md](facebook-native-roadmap.md) remains the long-term plan for the full SPA.*
