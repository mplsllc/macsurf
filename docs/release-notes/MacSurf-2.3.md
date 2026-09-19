## ⬇️ Download

**[Download MacSurf 2.3 ↓](https://macsurf.org/download.html)**  
One click from macsurf.org — free, no account, starts immediately. Source and tagged releases stay here on GitHub.

---

# MacSurf 2.3 "OPEN TABS"

*One window. More web.*

MacSurf 2.0.5 was the release where modern pages started looking right.

**2.3 is the release where MacSurf starts behaving much more like a modern browser.**

The obvious change is at the top of the window: **MacSurf has tabs now.** Underneath them is one of the largest development stretches the browser has had so far — hundreds of changes across JavaScript, dynamic page rendering, CSS, images, SVG, networking, cache behavior, browser chrome, downloads, diagnostics, and stability.

All of it is still happening locally on the Mac.

No rendering proxy. No remote browser. The page is fetched, scripted, laid out, and drawn by MacSurf.

## The headline: OPEN TABS

**MacSurf finally has tabs.**

A single native browser window can now hold multiple independent pages. Each tab owns its own:

- URL
- title
- page content
- scroll position
- navigation history
- JavaScript environment
- rendering state
- timers and page callbacks

Use **Command-T** or File → New Tab to open one. Use **Command-W** to close the current tab. There is also a **+** button for a new tab and a close **×** when hovering an existing one. When only one page is open, the tab strip gets out of the way.

This is not several browser windows disguised as tabs. MacSurf now has a real shared-window architecture: one native Mac window and one set of browser chrome, with several independent browser documents underneath it.

That required a lot more than drawing a strip at the top of the window. Background tabs must stay alive without changing the front tab's address field, title, scrollbars, caret, toolbar state, page controls, or JavaScript environment. Closing one tab cannot destroy another tab's runtime. Late network or timer callbacks from an old page cannot be allowed to execute in a newer one.

The final Tiger multi-tab stress campaign completed **20/20 cycles with zero crashes and zero hangs**, followed by testing on real Mac OS 9.2.2 hardware.

## JavaScript is much closer to a browser environment

MacSurf still executes JavaScript locally on PowerPC through macQJS / QuickJS, but the browser environment around the engine has grown substantially since 2.0.5.

Major additions and improvements include:

- ES module execution for script type="module"
- Headers, Request, and Response
- AbortController and AbortSignal
- Native fetch cancellation
- Real queueMicrotask ordering
- MutationObserver
- ResizeObserver
- Improved IntersectionObserver
- UIEvent and PointerEvent
- Event.composedPath()
- Better synthetic-event behavior and event constants
- document.createTreeWalker() and NodeFilter
- Much stronger querySelector() and querySelectorAll()
- Compound, descendant, attribute and selector-list support
- Better :not() handling
- Better DOM prototype and constructor relationships
- More correct instanceof behavior for Node, Element and HTMLElement
- Reflected HTML properties and attributes
- Better innerHTML, outerHTML, textContent and node replacement
- Working cloneNode()
- document.write()
- document.scripts, images, forms and links
- HTMLScriptElement.async
- document.title and document.defaultView improvements
- Better matchMedia()
- Persistent localStorage
- Real sendBeacon()
- Better URLSearchParams
- crypto.getRandomValues() and randomUUID()
- A useful Canvas 2D compatibility surface, including real native-font measurements through measureText()

A great deal of the work is lifecycle work that should be invisible when it succeeds: Promise jobs, timers, event handlers and late callbacks now have much stronger ownership rules so they cannot casually outlive the page or JavaScript runtime that created them.

## Dynamic pages can rebuild themselves without tearing the browser apart

Modern sites constantly change after the initial HTML arrives:

JavaScript mutates the DOM → CSS changes → boxes are rebuilt → layout changes → the page repaints.

MacSurf 2.3 can survive much more of that cycle.

A major reconstruction/reconvert campaign fixed entire families of failures involving:

- DOM mutation while a box tree is being rebuilt
- stale CSS selection data surviving into a replacement tree
- old JavaScript wrappers outliving their document
- iframe records pointing at retired boxes
- deferred content retirement racing a rebuild
- duplicate death-row entries
- images being repeatedly freed and fetched again during reconstruction
- stale page callbacks reaching a destroyed QuickJS context
- background callbacks reaching the wrong tab

Reconstruction is now treated much more like a transaction rather than something arbitrary browser work can interleave with.

That work is one of the reasons real tabs are possible.

## Another large CSS step

2.0.5 already moved MacSurf much closer to current CSS. 2.3 goes considerably farther.

### CSS math and sizing

MacSurf now has stronger support for:

- calc()
- min()
- max()
- clamp()
- min-content
- max-content
- fit-content
- min/max width behavior
- viewport-relative sizing
- percentage and calculated sizes
- display: contents
- overflow: clip
- independent row gaps

CSS math is carried through computed style and layout rather than reduced to a guessed number too early.

### Grid and flex layout

Grid received another substantial round of work:

- Auto tracks are sized after placement
- Remaining auto tracks stretch correctly
- minmax() pixel floors are honored
- justify-self support
- Better explicit-placement sizing
- Independent row and column gaps
- More reliable track sizing in real-world page layouts

Flexbox also received fixes around stretching, cross-size calculations and generated content.

### Backgrounds and paint

New or improved behavior includes:

- background-origin
- background-clip: text
- background blend modes
- clipped gradient-backed text
- better gradient stop colors
- better background sizing and positioning
- pseudo-element backgrounds
- inline background sizing
- generated-content paint fixes

### CSS custom properties

Custom properties received one of the deepest internal rewrites in the release.

The old implementation could allow a --variable definition elsewhere in a sheet to influence an element whose selector never actually matched it. In 2.3, variables are tied much more closely to the cascade that produced them.

The engine now handles selector scope, media scope, specificity, !important, inheritance, local overrides, sibling isolation, deferred var() resolution, and computed-value timing much more faithfully.

On modern component-heavy sites, that is the difference between "variables exist" and "variables behave like CSS variables."

## Images: native WebP and fewer disappearing pictures

**MacSurf now has native WebP support**, including the machinery needed for animated WebP frame updates.

Image handling in general is much more mature:

- Better lazy/eager loading balance
- Important images start earlier
- Intrinsic dimensions survive reconstruction
- Rebuilt boxes reconnect to already-loaded image objects
- Duplicate image fetches are reduced
- Cached image completion no longer leaves images squashed
- Oversized images can be reduced instead of simply disappearing
- Bitmap lifetime and retirement are safer
- Source selection considers actual decoder capability

The practical result is less flicker, fewer disappearing images, less duplicate network traffic, and fewer pages whose layout changes wildly as images finish.

## SVG is much stronger

External and inline SVG both received major work.

Improvements include:

- SVG through img
- SVG used as CSS backgrounds
- Intrinsic width and height
- viewBox sizing
- preserveAspectRatio
- meet / slice / none behavior
- alignment modes
- rect, line, polygon and polyline
- circles and ellipses
- translate, scale, rotate, matrix, skewX and skewY transforms
- CSS fill
- currentColor
- stroke width and opacity
- dashed and dotted strokes
- raster images embedded inside inline SVG

Graphics that previously appeared as tiny placeholders, stretched shapes, or black silhouettes now have a much better chance of looking like the source intended.

## A proper Downloads window

Downloads now have a real home.

The rebuilt Downloads window includes:

- File progress
- Determinate and indeterminate progress bars
- Transfer speed
- ETA
- Cancel
- Open
- Reveal in Finder
- Double-click to open
- Clear Finished
- Open Downloads Folder
- Change Download Folder

A custom destination can be selected with Navigation Services and remembered by MacSurf. Finder integration uses real AppleEvents.

## Preferences, Bookmarks and History got a major native-UI pass

The rest of MacSurf's interface was not ignored while tabs were being built.

**Preferences** now uses native Appearance Manager controls and tabbed panels, with fixes for control bounds, redraw, checkbox behavior and OS 9 embedding.

**Bookmarks** gained a cleaner native manager with scrolling, folders, rename/delete/move, import/export, URL preview and double-click navigation.

**History** received a more polished multi-column manager.

Across the browser there are also fixes for Classic Mac spacing, clipped labels, button widths, default-button treatment, status text, Geneva typography, menu ellipses, shortcuts, auxiliary-window activation, and native-control painting.

## Networking is faster and safer

HTTPS responses can now use **gzip compression**, which matters a great deal on a G3 downloading modern HTML, CSS and JavaScript.

Security and transport fixes include:

- Certificate rejection now fails closed
- A rejected certificate is no longer silently retried over plain HTTP
- Scripted fetch/XHR cannot follow an HTTPS → HTTP downgrade redirect
- Cross-origin referrers are reduced to the origin instead of sending the full path/query
- Tracker blocking covers HTTP as well as HTTPS
- TLS 1.3 record lengths are bounds-checked before decryption
- Classic Mac local time is converted correctly for X.509 certificate validation

That last one fixed a very old-Mac sort of bug: a freshly-issued certificate could appear "not yet valid" simply because the Mac's local timezone had been handed to the validator as GMT.

## Cache and loading behavior are more mature

MacSurf does much less redundant work now.

Changes include:

- Better HTTP 304 handling
- Correct freshness through disk-cache hits
- Better revalidation state
- Streaming disk-cache writes
- Better image and webfont caching
- Better cache behavior following POST/login
- Less duplicate image fetching
- Better object reuse across DOM reconstruction
- More graceful handling of transient network failures
- Decoded gzip content stored consistently in cache

On a G3, avoiding unnecessary work is often as important as making individual operations faster.

## More pages are actually clickable

A number of fixes addressed pages that looked correct but did not behave correctly with the mouse:

- Zero-width inline containers can expose their text for hit testing
- Click/drag tolerance is more forgiving
- CSS-painted boxes no longer incorrectly cover opaque native controls
- Radio buttons outside a form work
- appearance / appearance:none is respected in more places
- Click dispatch is less likely to duplicate or fall through
- Mouse release produces one click instead of repeatedly dispatching while held

This is representative of the 2.3 cycle: more of the remaining work is no longer "can MacSurf draw it?" but "does it keep behaving correctly afterward?"

## Diagnostics became part of the browser

MacSurf now has a much deeper built-in diagnostic system for investigating real websites directly on the machine.

It can track and correlate things such as:

- Navigation identity
- Document and frame identity
- Network requests
- JavaScript tasks
- Timers and asynchronous causality
- DOM mutation batches
- XHR/fetch activity
- Promise rejections
- Event handlers
- Layout passes
- Paint work
- Resource loading
- API compatibility gaps
- CSS/rendering gaps
- Box geometry and page maps

This tooling is what made many of the 2.3 fixes practical on a 25-year-old computer. Future compatibility work can increasingly start from evidence instead of guesses.

## Stability work you should never have to notice

A large portion of the work between 2.0.5 and 2.3 is not a feature at all. It is the browser being beaten on until failure cases stopped reproducing.

Fixed classes include:

- Box-tree double frees
- Stale iframe pointers
- JavaScript callbacks after context destruction
- Cross-runtime timer frees
- Wrapper lifetime bugs
- Deferred cache/content lifetime errors
- Duplicate retirement/death-row state
- Stale image objects
- Class changes not invalidating selector state
- CSS node-data lifetime problems across reconstruction
- Storage operating through a retired document
- Cache/LRU corruption
- TLS buffer overruns
- Mouse hit testing against dead layout state
- Late events reaching the wrong tab

The final tab-specific QuickJS guards validate a thread against the live registry, runtime generation, document and content ownership before a late callback is allowed to dispatch.

That is the difference between "tabs appear to work" and safely leaving several dynamic pages alive at the same time.

## Smaller improvements

There are hundreds of smaller changes in this release. Some of the more visible/useful ones include:

- Better word-break: break-word behavior
- Better CJK line breaking
- Better soft-hyphen handling
- More reliable line wrapping
- Table-cell vertical alignment fixes
- Better replaced-element/image aspect sizing
- a download handling and filename support
- Native select menus
- Better form-control appearance
- currentScript
- Better script property reflection
- Better dynamically-created script handling
- More correct DOMContentLoaded/load ordering
- Better document.readyState
- Better document/window event targets
- Real event.target behavior
- Timer callback arguments
- requestAnimationFrame timestamps
- Runtime-owned timer cleanup
- Stale Promise jobs discarded after navigation
- history.pushState()/replaceState() no longer reloading the page
- Better cookie persistence
- Better XHR/fetch URL resolution
- Better data: script handling
- Better large-bundle/script-size handling
- WOFF2/Brotli and webfont fixes
- Better text/plain and MIME handling
- Large numbers of C89 / CodeWarrior 8 compatibility and reliability fixes

## Seen on real hardware

MacSurf continues to be developed against real PowerPC hardware.

The primary acceptance target remains a **Power Macintosh G3 running Mac OS 9.2.2**.

Tiger has also become an important second environment, particularly for multi-tab and JavaScript-lifecycle stress testing.

The final tab stress run on Mac OS X 10.4.11 completed:

**20/20 cycles — 0 crashes, 0 hangs.**

The same architecture was then exercised on Mac OS 9.2.2 hardware.

## Supported systems

MacSurf 2.3 spans the Carbon era from **Mac OS 8.6 through Mac OS X 10.6 Snow Leopard**.

On PowerPC systems it runs directly. On Intel Macs that provide **Rosetta**, the PowerPC application runs through Rosetta, extending the usable Mac OS X range through 10.6.

Mac OS 9 remains the primary target and the environment most heavily tested on original hardware.

Recommended memory:

- 128 MB minimum
- 256 MB recommended
- 384 MB for the heaviest JavaScript sites

## The honest part

MacSurf 2.3 is dramatically more capable than 2.0.5. It is still a browser bringing a contemporary web platform to machines built for a very different Internet.

There are still modern CSS effects, selectors, browser APIs, Canvas capabilities and difficult layout/JavaScript interactions that are incomplete. Some frameworks expect synchronous layout measurement that is expensive to reproduce on a G3. Heavily-scripted themes can still expose geometry and layout edge cases.

MacSurf also deliberately avoids inventing browser state simply to make a feature test pass. A believable-but-wrong geometry value can be worse than no value at all: modern scripts will happily use the lie to rewrite their own page into a broken state.

A lot of 2.3 is about replacing those convenient lies with real browser behavior.

There is more to do.

But this is the strongest MacSurf has been.

## To run

Expand the StuffIt archive and double-click **MacSurf**.

No installer and no rendering proxy are required.

MacSurf handles HTTPS, CSS, JavaScript, images, layout and page rendering directly on the Mac.

## Support

MacSurf is a full-time project. If it puts an old Mac back online for you, support helps keep development moving:

- **Ko-Fi:** https://ko-fi.com/macsurf
- **Patreon:** https://www.patreon.com/cw/MacSurf
- **Discord:** https://discord.gg/mrwZK8zHr2

Thanks to everyone who keeps loading unreasonable websites on old Macs, taking screenshots, sending crash logs, and finding the next thing MacSurf was not supposed to be able to do.

2.0.5 proved a G3 could render Hackaday.

**2.3 opens another tab.**
