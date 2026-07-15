# MacSurf 2.0.5

*A polish release. 2.0 got the browser onto real Macs and rendering the modern
web — 2.0.5 is the batch of fixes and CSS work that makes those pages actually
look right.* No new setup, no gotchas — just a better build. Drop it in over 2.0.

---

## The big wins

**Hacker News works now.** You can log in, and the front page renders close to
what you'd see in a desktop browser — the orange logo, the vote arrows, and no
more phantom boxes around every story. (Login was being silently blocked by a
server-side check on our browser's name tag; we now introduce ourselves the way
the site expects.)

**Pages read the way they should.**
- Plain-text pages (and a lot of error/status pages) now show *in the window*
  instead of trying to download themselves.
- Images that used to render squished into a thin strip now come in at the right
  shape.
- More JPEGs decode instead of showing up as black boxes, and a photo that's too
  big for memory now shrinks to fit instead of vanishing.
- Backgrounds with see-through colors (rgba) now blend against what's actually
  behind them, instead of always assuming white.

**Typing is fast again.** Typing into a comment box or login form on a big page
used to stutter — every keystroke was redrawing the whole page. Now it only
repaints the little bit that changed. This also makes the cursor blink, hover
effects, and small scrolls snappier across the board.

**A large jump in CSS.** This is the headline for the web-standards side.
MacSurf now handles a whole cluster of modern layout and text features it didn't
before — several of which **no other classic-Mac browser has ever done**, and a
few that even the upstream NetSurf engine we're built on doesn't ship:

- **Justified text** that actually spreads words to both margins.
- **Soft hyphens** — long words break cleanly with a "-" at the line end.
- **`tab-size`** so code and pre-formatted text line up.
- **Flexbox alignment** (centering items on the cross-axis), and the
  **box-alignment shorthands** (`place-items`, `place-content`, `place-self`).
- **CSS Logical Properties** (`margin-inline`, `padding-block`, `inset-*`, and
  friends) — the modern way themes write their spacing.
- **Grid auto-sized columns** that size to their content like a real browser.
- Smaller touches: **`caret-color`**, **text-decoration** color/style/thickness,
  **`text-align-last`**, and a proper `background: none` reset.

Full property-by-property status is in the new
[CSS support tracker](../css-support.md).

---

## Closed issues

CLOSES: #62, #201, #212, #227, #230, #232, #234, #235, #239, #247, #249, #251,
#253, #268, #270, #271, #272, #273, #275, #276, #278

- **#232** — text/plain pages download instead of rendering
- **#201** — clicks falling through fixed/absolute overlays
- **#212 / #239** — typing lag / dropped keypresses in text fields
- **#227** — rgba() backgrounds compositing against white
- **#230** — JPEGs rendering as black boxes
- **#234** — words merging ("softwareand") + mid-word wraps
- **#235** — images rendering vertically squished until a reflow
- **#247** — CSS logical properties
- **#249** — text-decoration longhands
- **#251** — typography cluster (tab-size, hyphens, text-align-last, text-justify)
- **#253** — box-alignment shorthands (place-content/items/self)
- **#268** — `background: none` / `transparent` shorthand reset
- **#270** — flex cross-axis alignment (align-items / align-self)
- **#271** — text-align: justify
- **#272 / #275** — soft-hyphen rendering (`&shy;`)
- **#273** — word-break: break-all
- **#276** — justified line before a `<br>`
- **#278** — flex items / grid cells protruding past their container
- **#62** — grid auto-track content sizing

---

*Thanks as always to everyone testing on real hardware and filing issues — every
one of the fixes above came from a report on an actual G3/G4.*
