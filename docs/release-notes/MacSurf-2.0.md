<p align="center">
  <img src="2.0-assets/2pointoh.png" width="360" alt="MacSurf 2.0 — Prettier. Faster. Puffin-er.">
</p>

# MacSurf 2.0

A web browser for Classic Mac OS 9 that speaks the modern web — real CSS, a modern JavaScript engine, and HTTPS handled right on the Mac. No proxy, no second machine. Just your G3 or G4 and the live internet.

This is the big one. If MacSurf ever opened to a blank window on you — and clearing the cache "fixed" it for a while before it came back — that's over. 2.0 repairs an affected Mac by itself, the first time you launch it. No cache-clearing, no Virtual Memory dance (that never did anything, by the way — we measured it). If you'd given up on it, come back.

## First, the ask

MacSurf is what I do full-time. It only keeps moving at this pace if it can pay its own way, so if it put your old Mac back online, I'd be grateful for a few dollars. Supporters get the dev logs and a say in what comes next — and honestly, if I never ask, I can't expect any, so: here I am, asking.

- **Ko-Fi** — https://ko-fi.com/macsurf
- **Patreon** — https://www.patreon.com/cw/MacSurf
- **Discord** — https://discord.gg/mrwZK8zHr2

Thank you to **Shlooom**, **Kestral**, and **Mothra** on Patreon, and **kilgeist** and **Turuun** on Ko-Fi. You keep the lights on.

<p align="center"><img src="2.0-assets/kofi-qr.png" width="140" alt="Ko-Fi QR code"></p>

## What changed

**The blank screen is gone.** There were two things causing it. MacSurf was checking whether a chunk of memory was "real" against some hardcoded addresses that turned out to be wrong on Macs with more RAM — where the program loads higher up — so it threw away perfectly good pointers, and the browser quietly stopped fetching anything. On top of that, the list it keeps of unreachable servers was being saved to disk with no expiry, so one bad moment on your network could condemn your home page forever. Both fixed: MacSurf now asks the system where it actually lives, and that dead-server list only lasts the current session. I reproduced this on a maxed-out machine and watched it go from a blank window to loading two full sites.

**More of the HTTPS web works now — including macintoshgarden.org.** A lot of servers send their security certificates slightly out of order and trust the browser to sort it out. MacSurf's TLS stack was too strict about that. Now it sorts them, and a whole family of sites that used to bounce you to plain http open securely on the Mac.

<p align="center"><img src="2.0-assets/68kmla.png" width="620" alt="68kmla.org in MacSurf 2.0"></p>

**Heavy forums load in seconds instead of minutes.** Images now wait until you actually scroll to them. A busy 68kmla thread with a big attachment used to take anywhere from 20 seconds to two minutes — now it's a few seconds.

**The address bar finishes your URLs.** Start typing a site you've been to and it completes it from your history, with a little dropdown of the other matches. Arrow through them or just hit Return.

**Real History and Bookmark managers.** A proper History window that groups by day and can be cleared (Cmd-H), and a Bookmark manager with folders you can rename, move, and drag around (Cmd-B).

<p align="center">
  <img src="2.0-assets/history.png" width="350" alt="History manager">
  &nbsp;
  <img src="2.0-assets/bookmarks.png" width="350" alt="Bookmark manager">
</p>

**And it looks better.** New toolbar, a pill-shaped address bar, an animated loading spinner, a real progress bar, a fresh set of buttons, and a new About box. Typing in text fields is snappy again — each letter shows up the instant you press it. Get Info finally reports the version instead of "N/A." Multi-line copy/paste, favicons, and the duplicated Apple menu are all sorted too.

Under the hood it's still native TLS 1.3 through [macTLS](https://github.com/mplsllc/macTLS), modern ES2023 JavaScript through [macQJS](https://github.com/mplsllc/macQJS), built on [NetSurf](https://www.netsurf-browser.org/) with CodeWarrior and Carbon.

## The honest part

This is still in-progress software and I'd rather you hear it from me: the really heavy modern stuff — GitHub, YouTube, big React apps — still doesn't render. Tab reaches form fields but not links yet. Tabs (several pages in one window) aren't turned on. A handful of sites hit image-decode quirks. If something breaks, [open an issue](https://github.com/mplsllc/macsurf/issues) and drop your **MacSurf Debug.log** in — it genuinely helps, and this release exists because people did exactly that.

## Getting it

Grab the `.sit` below, expand it with StuffIt Expander, and double-click MacSurf. No installer. Runs on a Power Mac G3 or G4 under Mac OS 9.1–9.2.2 (CarbonLib 1.5+), 64 MB RAM or so. Already on a Mac OS 9 machine? Pull it straight from **http://macsurf.org/** — no other computer needed.

<p align="center"><img src="2.0-assets/home.png" width="620" alt="home.macsurf.org over native HTTPS"></p>

## Thank you

To **@glossolallia**, **@CaptainPanic29**, and **@Azryael** on GitHub and **@SandwichEnthusiast7** on Reddit — the crash logs and hardware testing you sent are the only reason the blank-screen bug got found. And to everyone at **[68kmla.org](https://68kmla.org)**, who's tested this thing every single day and been so kind about it — getting your forums to render, log in, and post a reply from a real Mac OS 9 machine has meant a lot.

<p align="center"><a href="https://www.patreon.com/MacSurf/posts/this-is-for-gary-163164919">For Gary &amp; Kaija</a></p>
