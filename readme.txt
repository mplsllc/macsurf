MacSurf 1.68 "macQJS"
A native web browser for Classic Mac OS (PowerPC, Mac OS 9.1-9.2.2).
Real HTTPS, a modern JavaScript engine, no proxy, no second machine.

--------------------------------------------------------------------
PLEASE SUPPORT MACSURF
--------------------------------------------------------------------
MacSurf is a full-time project. If it put your old Mac back on the
web, please consider chipping in - every bit genuinely helps keep
development going at this pace, and if I don't ask, I can't keep
doing this.

  Patreon:  https://www.patreon.com/cw/MacSurf
  Ko-Fi:    https://ko-fi.com/macsurf

Thank you.

--------------------------------------------------------------------
WHAT'S NEW IN 1.68
--------------------------------------------------------------------
* NEW JAVASCRIPT ENGINE (macQJS). Duktape (ES5) is gone; MacSurf now
  runs QuickJS - modern ES2023 JavaScript - natively on Mac OS 9.
  Real sites' real scripts run without being dumbed down first.

* Text input actually works: a visible blinking cursor, click-drag
  selection, Cut/Copy/Paste, and Tab between form fields.

* Logins stick. Signing in to a site now keeps you signed in.

* Near-instant startup (removed a ~17-second launch self-test).

* Bookmarks menu, a Downloads manager (HTTPS downloads work now),
  working window Maximize, and one tidy "MacSurfData" folder next to
  the app for the cache, downloads, bookmarks, and log.

* Faster page loads: images and fonts are cached to disk.

* Lots of rendering fixes (forum layouts, web-font icons, text
  spacing, cookie-consent bars you can actually click).

--------------------------------------------------------------------
REQUIREMENTS
--------------------------------------------------------------------
* A Power Macintosh (G3 or G4)
* Mac OS 9.1 - 9.2.2 with CarbonLib 1.5 or newer
* ~64 MB RAM (more helps on heavy pages)

--------------------------------------------------------------------
TO RUN
--------------------------------------------------------------------
Expand this archive with StuffIt Expander and double-click MacSurf.
That's it - no installer, no configuration.

--------------------------------------------------------------------
KNOWN LIMITATIONS
--------------------------------------------------------------------
* Very heavy sites (e.g. GitHub) don't render yet. To download new
  MacSurf releases from a Mac OS machine, use the plain non-SSL
  (http) version of macsurf.org - no other machine needed.
* Tab reaches form fields, but not links/buttons yet.
* Tabs (multiple pages in one window) are not enabled yet.

--------------------------------------------------------------------
THANK YOU, 68kmla.org
--------------------------------------------------------------------
This release is named 1.68 in honor of 68kmla.org - the 68k/PowerPC
Mac community that has been so kind and welcoming, tested MacSurf
every day, and shaped this whole cycle of work. Getting your forums
to render, log in, and post from a real Mac OS 9 machine has been a
joy to build toward. Thank you.

--------------------------------------------------------------------
  Web:      https://macsurf.org   (a non-SSL http version is
                                   available for downloading from
                                   Mac OS machines)
  Source:   https://github.com/mplsllc/macsurf
  Engine:   https://github.com/mplsllc/macQJS
--------------------------------------------------------------------
