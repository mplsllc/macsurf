<p align="center">
  <img src="img/bannerlogo.png" alt="MacSurf" width="460">
</p>

<p align="center"><strong>The modern web, on a 25-year-old Mac.</strong></p>

<p align="center"><strong>
  Submit logs directly from your mac! Link tested on IE 5 and Netscape 4.77/7. Also includes a link
  to a much more comprensive logger verson of MacSurf.
  - <a href="http://macsurf.org/debugs.html">http://macsurf.org/debugs.html</a></strong>
</p>

<p align="center">
  A native web browser for Classic Mac OS 9 on PowerPC — real CSS3, modern JavaScript, and HTTPS, running on a G3 iMac. No proxy, no second machine.
</p>

<p align="center">
  <img src="screenshots/2.0.5-hackaday-imac.jpg" alt="MacSurf 2.0.5 rendering hackaday.com on a Power Macintosh G3 iMac" width="760">
  <br>
  <em>MacSurf 2.0.5 on a Power Mac G3 iMac, rendering <a href="https://hackaday.com/">hackaday.com</a> at full desktop width over native HTTPS.</em>
</p>

---

<p align="center">
  <em>MacSurf is a full-time project. If it brought your old Mac back to the web, a few dollars keeps development going — and you'll get the dev logs.</em>
</p>

<p align="center">
  <a href="https://ko-fi.com/macsurf" target="_blank"><img src="support_me_on_kofi_badge_blue.png" alt="Support on Ko-Fi" height="46"></a>
  &nbsp;&nbsp;
  <a href="https://www.patreon.com/cw/MacSurf" target="_blank"><img src="patreon-banner-519x200.png" alt="Support on Patreon" height="46"></a>
  &nbsp;&nbsp;
  <a href="https://discord.gg/mrwZK8zHr2" target="_blank"><img src="img/discord.jpeg" alt="Join our Discord!" height="46"></a>
    &nbsp;&nbsp;
  <a href="https://bsky.app/profile/macsurfos9.bsky.social" target="_blank"><img src="img/bsky.png" alt="Follow me on BlueSky!" height="46"></a>
</p>

<p align="center"><sub><strong>Thanks to our supporters:</strong> Shlooom, Kestral, Mothra (Patreon) &middot; kilgeist, Turuun, Rogue (Ko-Fi)</sub></p>

<p align="center">
  <img src="screenshots/2.0-thankyou.png" alt="The MacSurf 2.0 About box crediting supporters" width="420">
</p>

---

> [!NOTE]
> **2.0.5 "HACKADAY"** is a polish release over 2.0: point it at **hackaday.com** and the front page renders at real desktop width, with the right fonts and the article cards laid out. Under that headline is a browser-wide text-size fix (author `font-size` was drawing ~25% too small, which is why so many sites came up cramped), a large modern-CSS pass, a more capable on-device JavaScript engine, and tracker/ad-network blocking. It's still at its best on hand-built pages, retro sites, and forums; very heavy modern apps — GitHub, video, React-heavy SPAs — still don't render. This is honest, in-progress software. Got a G3 or G4? Load it up and tell us what breaks. See [docs/status.md](docs/status.md) for the current punch list.

## Why this exists

The web outgrew Classic Mac OS twenty years ago, and modern HTTPS finished the job around 2016. Pull a G3 or G4 out of the closet today and it can barely reach a single live site.

MacSurf fixes that on the machine itself — no screenshot proxy, no remote-terminal trick. It's a native browser built with the tools that shipped on the platform: CodeWarrior, Carbon, QuickDraw, Open Transport. It speaks TLS 1.3 straight to the modern web through [macTLS](https://github.com/mplsllc/macTLS), a BearSSL-based stack baked into the binary with the full Mozilla CA bundle, and runs modern JavaScript through [macQJS](https://github.com/mplsllc/macQJS), a QuickJS port for Mac OS 9.

As far as we can tell, it's the first serious [NetSurf](https://www.netsurf-browser.org/) port to Classic Mac OS, and the first Mac OS 9 browser with native CSS Grid, CSS custom properties, and an on-device modern JavaScript engine.

## Real sites, on real hardware

Every shot below is a live site, captured on a Power Mac G3 running Mac OS 9.2.2.

<table>
<tr>
<td width="50%" align="center" valign="top"><img src="screenshots/2.0-68kmla.png" alt="68kmla.org forum"><br><strong>68kmla.org</strong><br><em>A full XenForo forum — logged in, full-width, posting replies.</em></td>
<td width="50%" align="center" valign="top"><img src="screenshots/2.0-ppcmla.png" alt="ppcmla.org forum"><br><strong>ppcmla.org</strong><br><em>Another full forum, over native HTTPS.</em></td>
</tr>
<tr>
<td width="50%" align="center" valign="top"><img src="screenshots/2.0-macintoshrepository.png" alt="Macintosh Repository"><br><strong>macintoshrepository.org</strong><br><em>Thumbnails, tables, and all.</em></td>
<td width="50%" align="center" valign="top"><img src="screenshots/2.0-duckduckgo.png" alt="DuckDuckGo search"><br><strong>DuckDuckGo</strong><br><em>Search that works.</em></td>
</tr>
</table>

## New in 2.0.5

<table>
<tr>
<td colspan="2" align="center" valign="top"><img src="screenshots/2.0.5-hackaday.png" alt="hackaday.com rendering at desktop width" width="720"><br><strong>hackaday.com renders</strong><br><em>A modern, JavaScript-heavy news site at full desktop width — correct type, article cards, images in place.</em></td>
</tr>
</table>

<sub>Under it: text is measured in real device pixels (author `font-size` no longer draws 25% too small), a large modern-CSS pass (justified text, soft hyphens, logical properties, box-alignment shorthands, grid auto-sizing), a more capable JavaScript engine (real `fetch`/`XHR`, resolving Promise chains, DOM traversal, `document.cookie`), and tracker/ad-network blocking. [Full 2.0.5 notes →](docs/release-notes/MacSurf-2.0.5.md)</sub>

## New in 2.0

<table>
<tr>
<td colspan="2" align="center" valign="top"><img src="screenshots/2.0-url-autocomplete.png" alt="Type-ahead address bar" width="720"><br><strong>Type-ahead address bar</strong><br><em>History- and bookmark-backed suggestions as you type.</em></td>
</tr>
<tr>
<td width="50%" align="center" valign="top"><img src="screenshots/2.0-history.png" alt="History manager"><br><strong>History manager</strong><br><em>Day-grouped, searchable, clearable — a real window.</em></td>
<td width="50%" align="center" valign="top"><img src="screenshots/2.0-bookmarks.png" alt="Bookmark manager"><br><strong>Bookmark manager</strong><br><em>Save, organize, and jump straight from the menu.</em></td>
</tr>
</table>

<details>
<summary><strong>Earlier shots</strong> — the same sites on previous builds</summary>

<br>

<table>
<tr>
<td width="50%" align="center" valign="top"><img src="screenshots/site-68kmla.png" alt="68kmla.org forum"><br><strong>68kmla.org</strong></td>
<td width="50%" align="center" valign="top"><img src="screenshots/site-macintoshgarden.png" alt="Macintosh Garden"><br><strong>macintoshgarden.org</strong></td>
</tr>
<tr>
<td width="50%" align="center" valign="top"><img src="screenshots/site-macintoshrepository.png" alt="Macintosh Repository"><br><strong>macintoshrepository.org</strong></td>
<td width="50%" align="center" valign="top"><img src="screenshots/site-machut.png" alt="Mac Hut"><br><strong>machut.net</strong></td>
</tr>
<tr>
<td width="50%" align="center" valign="top"><img src="screenshots/site-lobsters.png" alt="Lobsters thread about MacSurf"><br><strong>lobste.rs</strong></td>
<td width="50%" align="center" valign="top"><img src="screenshots/site-duckduckgo.png" alt="DuckDuckGo search"><br><strong>DuckDuckGo</strong></td>
</tr>
</table>
</details>

<details>
<summary><strong>How it got here</strong> — a couple of early milestones</summary>

<br>

<table>
<tr>
<td width="50%" align="center" valign="top"><img src="screenshots/01-javascript-on-os9.jpg" alt="JavaScript on Mac OS 9"><br><strong>v0.2 — JavaScript on Mac OS 9</strong><br><em>The first JS-bearing page evaluating live, on-device.</em></td>
<td width="50%" align="center" valign="top"><img src="screenshots/08-css-grid-placement.jpg" alt="CSS Grid on Mac OS 9"><br><strong>CSS Grid</strong><br><em>Real Grid layout — spans, full-row heroes, auto-wrap.</em></td>
</tr>
</table>
</details>

## The pieces

<table>
<tr><th align="left">Component</th><th align="left">Language</th><th align="left">Purpose</th></tr>
<tr>
<td><a href="browser/"><code>browser/</code></a></td>
<td>C (C89, CW8)</td>
<td>NetSurf fork with a <code>macos9</code> frontend. Carbon for the UI, QuickDraw for drawing, Open Transport for networking, macQJS for JavaScript.</td>
</tr>
<tr>
<td><code>macTLS</code><br><sub><a href="https://github.com/mplsllc/macTLS">sibling repo</a></sub></td>
<td>C (CW8)</td>
<td>Native TLS 1.3 (1.2 fallback) for OS 9 — HTTPS straight from the Mac. BearSSL underneath, full Mozilla CA bundle baked in.</td>
</tr>
<tr>
<td><code>macQJS</code><br><sub><a href="https://github.com/mplsllc/macQJS">sibling repo</a></sub></td>
<td>C (CW8)</td>
<td>A QuickJS port for Classic Mac OS — modern ES2023 JavaScript on PowerPC.</td>
</tr>
</table>

## What works today

<table>
<tr>
<td valign="top" width="50%">

**Rendering**
- Full NetSurf fetch → parse → cascade → layout → plot
- Native libcss with `var()`, ~150 properties consumed in layout
- Flexbox, CSS Grid, gradients, `border-radius`, `box-shadow`, opacity, transforms, z-index stacking
- PNG (real alpha), GIF, JPEG, BMP, TIFF
- Downloadable web-font icon glyphs

[Full status →](docs/status.md)

</td>
<td valign="top" width="50%">

**JavaScript — macQJS (QuickJS, ES2023)**
- `let`/`const`, arrows, classes, template literals, Promises, generators, modern regex
- Runs real site bundles on-device

**Networking**
- HTTP/1.1: chunked, keep-alive, 3xx follow, connection pooling
- HTTPS via macTLS — TLS 1.3, 1.2 fallback, full CA bundle
- Cookies and logins that persist

**Chrome**
- Address bar, back / forward / reload / home, bookmarks menu, downloads manager
- Text input: caret, selection, cut / copy / paste, Tab between fields
- Multi-window, smooth scroll bar, keyboard scrolling

</td>
</tr>
</table>

## Download

**[MacSurf 2.0.5 "HACKADAY"](https://github.com/mplsllc/macsurf/releases/latest)** (2026-07-17) — a polish release over 2.0: hackaday.com renders at full desktop width, a browser-wide text-size fix (author `font-size` was drawing ~25% too small), a large modern-CSS pass, a more capable on-device JavaScript engine, and tracker/ad-network blocking. [Full release notes →](docs/release-notes/MacSurf-2.0.5.md) &middot; [2.0 notes →](docs/release-notes/MacSurf-2.0.md)

- **[Download the .sit →](https://github.com/mplsllc/macsurf/releases/latest)** — expand with StuffIt Expander on Mac OS 9.1+ (CarbonLib 1.5+) and double-click. No installer.
- Already on a Mac OS 9 machine? Grab it from the plain-HTTP **[macsurf.org](http://macsurf.org/)** — GitHub doesn't render on-device yet.
- [All releases →](https://github.com/mplsllc/macsurf/releases)

## Building

MacSurf builds on Mac OS 9 with CodeWarrior 8 Pro (8.3 update). The source is cross-compile-clean against Retro68 PowerPC GCC, which we use for fast Linux-side syntax checks.

- [Mac-side build guide](docs/codewarrior-setup.md)
- [Linux cross-dev workflow](docs/cross-dev-from-linux.md)

---

<p align="center"><sub>
  Native HTTPS via <a href="https://github.com/mplsllc/macTLS">macTLS</a> &middot;
  JavaScript via <a href="https://github.com/mplsllc/macQJS">macQJS</a> &middot;
  built on <a href="https://www.netsurf-browser.org/">NetSurf</a> &middot;
  <a href="https://www.youtube.com/watch?v=PLpbHSXca60">intro video</a>
</sub></p>

<br>

<p align="center">
  <img src="img/PuffyCircle.gif" alt="" width="90">
</p>

<p align="center"><em><a href="https://www.patreon.com/MacSurf/posts/this-is-for-gary-163164919">For Gary &amp; Kaija</a></em></p>
