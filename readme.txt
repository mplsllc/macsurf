MacSurf 2.3 "OPEN TABS"
=======================

One window. More web.

MacSurf is a native web browser for old Macs, with modern CSS,
modern JavaScript through macQJS, and HTTPS handled directly on
the machine through macTLS. No rendering proxy and no companion
computer are required.

WHAT'S NEW IN 2.3
-----------------

TABS ARE HERE.

MacSurf can now keep multiple independent pages alive inside one
native browser window. Use Command-T or File > New Tab to open a
tab and Command-W to close the current one. Each tab keeps its own
URL, title, page content, scroll position, JavaScript environment,
rendering state, and history.

2.3 also includes a very large compatibility and stability pass:

 * Much stronger dynamic-DOM and JavaScript lifecycle handling.
 * ES modules and many more browser APIs.
 * MutationObserver, ResizeObserver and improved IntersectionObserver.
 * Headers, Request, Response, AbortController and better fetch/XHR.
 * Better DOM traversal, selectors, events, storage and script loading.
 * Major CSS improvements including calc/min/max/clamp, sizing, Grid,
   custom properties, backgrounds, paint and generated content.
 * Native WebP support and much stronger SVG support.
 * A rebuilt Downloads window plus major Preferences, Bookmarks and
   History interface improvements.
 * HTTPS gzip support, stricter certificate failure handling, safer
   redirect/referrer behavior and additional TLS hardening.
 * Large cache, image-lifetime, reconstruction and crash-stability work.
 * Built-in diagnostics for navigation, JavaScript, DOM mutation,
   layout, paint, resources and asynchronous work.

MacSurf 2.3 is the result of more than a single feature cycle:
the tab UI sits on top of extensive work making several live,
scripted pages coexist safely on PowerPC hardware.

SYSTEMS
-------

MacSurf supports the Carbon era from Mac OS 8.6 through
Mac OS X 10.6.

On PowerPC Macs, it runs directly. On Intel Macs that provide
Rosetta, the PowerPC application runs through Rosetta; this extends
the usable Mac OS X range through Snow Leopard 10.6.

Mac OS 9.2.2 on a Power Macintosh G3 remains the primary hardware
acceptance target.

Memory:
  128 MB minimum
  256 MB recommended
  384 MB recommended for the heaviest JavaScript sites

TO RUN
------

Expand the StuffIt archive and double-click MacSurf.
There is no installer and no proxy configuration.

DOWNLOAD
--------

https://macsurf.org/download.html

A plain HTTP version of macsurf.org is also available for vintage
machines that need it.

SOURCE
------

https://github.com/mplsllc/macsurf

SUPPORT
-------

Patreon: https://www.patreon.com/cw/MacSurf
Ko-Fi:   https://ko-fi.com/macsurf
Discord: https://discord.gg/mrwZK8zHr2

Made with care for old Macs.

For Gary & Kaija
