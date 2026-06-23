# CarbonLib on Mac OS 9

Background notes for the MacSurf wiki. CarbonLib is the shared library that
implements Apple's Carbon API on classic Mac OS (8.1–9.2.2). MacSurf is a
Carbon CFM application, so it depends on CarbonLib at runtime; understanding
what CarbonLib is, how an app binds to it, and the marker resources involved
is foundational. Facts below are drawn from Apple's own *Carbon Porting Guide*
(Legacy, 2002) and corroborating secondary sources; each bullet cites the URL
it came from. Where a detail is uncertain, that is stated plainly.

## Facts

- **Carbon is a set of programming interfaces derived from earlier Mac OS APIs
  that can run on Mac OS X**, covering ~70% of existing Mac OS APIs and ~95% of
  the functions applications actually use. Its purpose was a single source base
  spanning Mac OS 8, 9, and X.
  Source: https://leopard-adc.pepas.com/documentation/Carbon/Conceptual/carbon_porting_guide/carbonporting.pdf

- **On Mac OS 8 and 9 the Carbon implementation is a single system extension
  named `CarbonLib`.** Apple: "On Mac OS 8 and 9, the Carbon implementation is
  stored as a system extension named CarbonLib." It contains both Carbon-specific
  function implementations and exports that "call through" to existing system
  software (e.g. Menu Manager calls route to `InterfaceLib`). On Mac OS X the
  same role is played by `Carbon.framework`.
  Source: https://leopard-adc.pepas.com/documentation/Carbon/Conceptual/carbon_porting_guide/carbonporting.pdf

- **A Carbon app links at build time against a stub (`CarbonLibStub`) and binds
  at runtime to `CarbonLib` (OS 8/9) or `Carbon.framework` (OS X).** For a pure
  Carbon application "the only library you should link against is CarbonLib."
  Source: https://leopard-adc.pepas.com/documentation/Carbon/Conceptual/carbon_porting_guide/carbonporting.pdf

- **CarbonLib runs Carbon applications on Mac OS 8.1 and later, and ships in
  every version of Mac OS 9** (and in the Classic environment under Mac OS X).
  Apple: "CarbonLib is the standard implementation of Carbon for Mac OS 8.1 or
  later" and is "included in all versions of Mac OS 9."
  Source: https://leopard-adc.pepas.com/documentation/Carbon/Conceptual/carbon_porting_guide/carbonporting.pdf

- **Version history (from Apple's "Determine the Appropriate CarbonLib Version"
  table):** 1.0 (Universal Interfaces 3.3.1, back to OS 8.1; shipped with OS 9,
  "Do not develop with this version"); 1.0.4 (UI 3.3.1, OS 8.1; adds Navigation
  Services, Core Foundation, Carbon Printing Manager, Appearance Manager 1.1);
  1.2 (UI 3.4, OS 8.6; adds Carbon Event Manager, Data Browser, ATSUI, URL
  Access). Later versions add Keychain Manager, Apple Help Viewer, and Font
  Management. The guide (2002) calls **1.6 the latest** at that time.
  Source: https://leopard-adc.pepas.com/documentation/Carbon/Conceptual/carbon_porting_guide/carbonporting.pdf

- **CarbonLib 1.0.4 was released April 21, 2000 as a system extension** requiring
  Mac OS 8.1, 8.5.1, 8.6, 9.0, 9.0.2, 9.0.3, or 9.0.4 (per Apple's TIL read-me).
  Source: https://til-2001.mirror.kb1max.com/techinfo.nsf/artnum/n88016/index.html

- **CarbonLib 1.3.1 was posted May 11, 2001** for Mac OS 8.6, all Mac OS 9
  releases, and Mac OS X (note: 8.6 became the floor for 1.3.1 and later).
  Source: https://www.mactech.com/2001/05/11/apple-carbonlib-1-3-1-posted/

- **1.6.1 (2003) is the highest CarbonLib version**, distributed on later OS
  install media rather than as a standalone Apple self-mounting image, per the
  Macintosh Garden archive page. Garden also hosts 1.5, 1.6.0, and 1.6.1
  installers (e.g. `CarbonLib_1.5.smi_.bin`, `CarbonLib_161.sit`).
  Source: https://macintoshgarden.org/apps/carbonlib

- **CarbonLib is obtainable today from retro-computing archives**, notably the
  Macintosh Garden (multiple mirrors) and Macintosh Repository. Apple's own
  legacy download pages are largely gone; these archives are the practical source.
  Sources: https://macintoshgarden.org/apps/carbonlib ; https://www.macintoshrepository.org/17069-carbonlib

- **The `'cfrg'` (code-fragment) resource, ID 0, is what the Code Fragment
  Manager reads to locate a CFM binary's code and its import libraries.** Carbon
  "fully supports the Code Fragment Manager"; PEF (Preferred Executable Format)
  is the CFM object format, Mach-O is the OS X format. The `'cfrg' 0` resource
  is one of the few resources Apple says **must stay in the resource fork** for
  CFM-based apps so the Finder can launch them.
  Source: https://leopard-adc.pepas.com/documentation/Carbon/Conceptual/carbon_porting_guide/carbonporting.pdf

- **The `'carb'` resource (ID 0) marks a binary as a Carbon application.** Apple:
  the `'plst' 0` resource "supersedes the older `'carb' 0` resource" but you may
  still use `'carb' 0`; both, plus `'cfrg' 0`, must remain in the resource fork
  for CFM apps. A common community description is that the resource's content
  doesn't matter (often a single byte). Without the marker, OS X opens the app in
  Classic; on classic OS it affects whether CarbonLib engages.
  Sources: https://leopard-adc.pepas.com/documentation/Carbon/Conceptual/carbon_porting_guide/carbonporting.pdf ; https://www.highcaffeinecontent.com/blog/20150124-MPW,-Carbon-and-building-Classic-Mac-OS-apps-in-OS-X

## Beginner gotchas / things that surprise people

- **`'carb'` and `'plst'` are siblings, not the same as `'cfrg'`.** `'cfrg' 0`
  is the CFM loader's map (where the code and imports live); `'carb' 0` /
  `'plst' 0` flag the app as Carbon. A CFM Carbon app needs both kinds present
  in the resource fork.
- **`'carb'` was deprecated in favor of `'plst'` by 2002**, but `'carb' 0` still
  works and is simpler; that's why old toolchains and ports still use it.
- **CarbonLib is included in every Mac OS 9**, so "install CarbonLib" usually
  means *update* it (e.g. to 1.6) rather than add a missing piece.
- **Reported build dates conflict across secondary sources.** Search summaries
  variously cite 1.5 as built 2001-12-14 vs 2002-06-17. These exact build dates
  are NOT confirmed from a primary Apple source here and should be treated as
  uncertain; the version ordering (1.0 → 1.0.4 → 1.2 → 1.3.1 → 1.5 → 1.6 →
  1.6.1) and approximate years (1.0.4 = 2000, 1.3.1 = 2001, 1.6.1 = 2003) are
  the reliable parts.
- **A non-Carbon CFM app and a Carbon CFM app look similar on disk** (both PEF +
  `'cfrg'`); the Carbon marker resource is the distinguishing piece, which is
  why a missing/incorrect marker is a classic launch failure on OS 9.

## Sources

- Apple, *Carbon Porting Guide* (Legacy, 2002-12-01), full PDF: https://leopard-adc.pepas.com/documentation/Carbon/Conceptual/carbon_porting_guide/carbonporting.pdf
- Apple AppleCare TIL n88016, *CarbonLib 1.0.4 Update: Read Me* (mirror): https://til-2001.mirror.kb1max.com/techinfo.nsf/artnum/n88016/index.html
- MacTech, "Apple CarbonLib 1.3.1 posted" (2001-05-11): https://www.mactech.com/2001/05/11/apple-carbonlib-1-3-1-posted/
- Macintosh Garden, CarbonLib (versions, archive downloads): https://macintoshgarden.org/apps/carbonlib
- Macintosh Repository, CarbonLib: https://www.macintoshrepository.org/17069-carbonlib
- High Caffeine Content, "MPW, Carbon and building Classic Mac OS apps in OS X" (2015): https://www.highcaffeinecontent.com/blog/20150124-MPW,-Carbon-and-building-Classic-Mac-OS-apps-in-OS-X
- Wikipedia, "Carbon (API)" (background only): https://en.wikipedia.org/wiki/Carbon_(API)
