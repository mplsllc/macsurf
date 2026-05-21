# Social preview image

The repo's social preview is the link card shown when [github.com/mplsllc/macsurf](https://github.com/mplsllc/macsurf) is pasted into Twitter, Mastodon, Hacker News, Discord, Slack, iMessage, etc. It's seen orders of magnitude more often than the README, so the artwork is worth getting right.

This page documents the asset spec and the manual upload step (the GitHub API does not expose social preview uploads).

## Spec

- **Dimensions**: exactly 1280 × 640 pixels (2:1 landscape).
- **Format**: PNG or JPG.
- **File size**: under 1 MB; ideally under 500 KB.
- **Safe area**: keep critical content outside the outer ~60 px on all sides, some platforms crop the edges of the card.

## Recommended composition

- **Right ~58%**: cropped screenshot of MacSurf rendering something visually striking. `screenshots/03-css-radial-gradients.jpg` is the canonical choice, the colourful gradient cards inside the platinum window chrome read instantly as "modern CSS on Mac OS 9". Alternatives: `04-css-animations.jpg`, `08-css-grid-placement.jpg`.
- **Left ~42%**: dark panel (~#121828) with:
  - The puffin wordmark from [img/bannerlogo.png](../img/bannerlogo.png), scaled to ~440 px wide, top-left.
  - Two-line tagline in bold sans-serif white / warm-orange: "The modern web, on a 25-year-old Mac."
  - Sub-tagline in lighter weight: "Real CSS3. Real ES5 JavaScript. Real HTTPS. Running on a beige G3."
  - Bottom strip in small caps, muted blue-gray: `MAC OS 9 · POWERPC · CARBON · CODEWARRIOR`.
- **Bleed**: a 100-120 px gradient feather between the dark panel and the screenshot makes the transition feel intentional rather than collaged.

A Python reference generator using PIL and the source screenshot lived at `/tmp/make_social.py` during initial scaffolding, re-create it if needed, or compose directly in Figma / Photoshop / Affinity.

## Upload step (manual)

1. Place the finished 1280 × 640 image in the repo at `img/social-preview.png` (or keep it out-of-tree if you prefer not to commit it).
2. Open <https://github.com/mplsllc/macsurf/settings>.
3. Scroll to **Social preview**.
4. Click **Edit** → **Upload an image** and select the file.
5. Save.

## Verification

After upload, paste the repo URL into any of:

- <https://opengraph.xyz/url/https%3A%2F%2Fgithub.com%2Fmplsllc%2Fmacsurf>
- Twitter / X Card Validator (requires login)
- Or just share the URL into any chat client and watch the unfurl

GitHub caches the preview aggressively, if you replace the image and the old one keeps appearing, try the `?cache_bust=N` trick on the OpenGraph URL or wait a few hours.
