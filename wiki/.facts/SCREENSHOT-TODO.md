# Screenshot to-do — captures to add to the wiki later

Staged here (in `.facts/`, never published). When you've grabbed shots, add them to the wiki and delete the line.

**Tooling:** capture with **Snapz Pro 2** on the G3 (or SheepShaver for UI-only shots that don't need real hardware). Save as PNG, lowercase-hyphen names (e.g. `cw-target-settings.png`).

**How to add an image to a GitHub wiki:**
- Easiest: open the page in the wiki's web editor and drag the PNG in — GitHub uploads it and inserts the markdown for you.
- Or: drop the PNG into the `macsurf.wiki.git` checkout and reference it with `![caption](image-name.png)`.
- Add a short, honest caption under each. Don't stage a shot you'd have to fake — a missing screenshot beats a misleading one.

Prioritized by impact (★ = do these first):

## Home
- [ ] ★ **Hero shot** — MacSurf rendering a real, recognizable site on the G3. The "modern web on a 25-year-old Mac" money shot; goes at the top. (`screenshots/v0.3-mactrove-fixes139.png` may still work, or grab something fresher on v1.4.)

## CodeWarrior Project Settings (the highest-value page for shots)
- [ ] ★ **Target Settings** panel (linker = MacOS PPC Linker, project type, `MPLS`/`APPL`).
- [ ] ★ **C/C++ Language** panel (the `macsurf_prefix.h` prefix file field, the language toggles).
- [ ] ★ **Access Paths** panel showing the user-path tree.
- [ ] ★ **PPC PEF** panel showing the partition (preferred / minimum heap).

## Building MacSurf
- [ ] ★ CodeWarrior project window open on `MacSurf.mcp` — the grouped file list in the left pane.
- [ ] A finished build / the app launching (title bar showing it's loaded).

## Setting Up the Build Environment
- [ ] CodeWarrior installed (the Metrowerks folder, or the install running).
- [ ] SheepShaver booted to the OS 9 desktop with MacSurf running.
- [ ] CarbonLib in the Extensions folder with its version visible (Get Info).

## The Rendering Pipeline
- [ ] ★ A CSS-heavy real page rendered (gradients / flex / grid visibly working).
- [ ] A transparent PNG composited over a page background (alpha working, not a white box).

## The JavaScript Engine
- [ ] ★ The JS probe page scoring 19/19 on the G3.
- [ ] `about:perf`, or a JS demo (the Mandelbrot) mid-run.

## Networking & TLS
- [ ] An HTTPS site loaded over native macTLS (something recognizable — the padlock story).

## Diagnostics & Debugging
- [ ] ★ A MacsBug screen mid-crash (a phone photo of the monitor is fine — it's outside the GUI). Worth annotating the PC / registers.
- [ ] `MacSurf Debug.log` open in SimpleText, showing the last lines before a crash.
- [ ] One of the `about:` pages (`about:cache` / `about:memory` / `about:perf`) rendered.

## Start Your Own Classic Mac Project
- [ ] A minimal Carbon app window (the skeleton) running.
- [ ] The `'carb'` resource shown in ResEdit/Resorcerer (nice-to-have).

**Skip:** Architecture Overview wants a block *diagram*, not a screenshot; Contributing and Resources don't really need images.
