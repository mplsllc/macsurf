# MacSurf Compatibility Progress Ledger

**Tracking the progressive elimination of observed Web-platform compatibility gaps on real PowerPC hardware.**

This ledger records every tested build checkpoint, the unique gap count, total hits, diagnostic census completeness, resolved gaps, newly exposed gaps, and associated GitHub tracking issues.

---

### Reference Site: TinkerDifferent (`https://tinkerdifferent.com`)

| Build / Package | Commit SHA | Unique Gaps | Total Hits | Lossless? | Coverage State | Resolved Gaps | Newly Exposed Gaps | Notes |
| :--- | :--- | :---: | :---: | :---: | :---: | :--- | :--- | :--- |
| **MacSurf104** | `17b847279` | 78 | 209 | Lossless | Explicit Matrix | — | — | Initial Phase 4 unified gapreport census baseline on live hardware. |
| **MacSurf105** | `f5c3c5a2b` | 78 | 209 | Lossless | Explicit Matrix | — | — | Phase 5 coverage audit with explicit unobservable reasons. |
| **MacSurf106** | `7a6f3cc4d` | 78 | 209 | `census_lossless=1` | Explicit Matrix | — | — | Phase 5.1 hash-based dedup & normalized key schema (`js.<domain>.<feature>.<op>`). |
| **MacSurf107** | `c48e111e8` | 78 | 209 | `census_lossless=1` | Explicit Matrix | — | — | Phase 6 minimal default logging (pagemap walk gated OFF by default). Authoritative baseline for Phase 7 reconciliation ([#321](https://github.com/mplsllc/macsurf/issues/321)–[#328](https://github.com/mplsllc/macsurf/issues/328)). |

---

### Tracked Compatibility Issues (TinkerDifferent Campaign)

1. **[#321](https://github.com/mplsllc/macsurf/issues/321)**: DOM/JS Element Geometry (`offsetWidth`, `clientWidth`, `scrollWidth`, `offsetHeight`, `clientHeight`, `scrollHeight`)
2. **[#322](https://github.com/mplsllc/macsurf/issues/322)**: CSS Transitions (`transition`, `transition-property`)
3. **[#323](https://github.com/mplsllc/macsurf/issues/323)**: CSS Animations (`animation`, `animation-delay`, `animation-play-state`)
4. **[#324](https://github.com/mplsllc/macsurf/issues/324)**: CSS Selector `:focus-within`
5. **[#325](https://github.com/mplsllc/macsurf/issues/325)**: CSS `break-inside`
6. **[#326](https://github.com/mplsllc/macsurf/issues/326)**: CSS `outline-offset`
7. **[#327](https://github.com/mplsllc/macsurf/issues/327)**: CSS `will-change`
8. **[#328](https://github.com/mplsllc/macsurf/issues/328)**: Vendor-prefixed CSS properties (`-webkit-user-select`, `-moz-user-select`, `-webkit-overflow-scrolling`, `-webkit-font-smoothing`, `-moz-osx-font-smoothing`, `-webkit-tap-highlight-color`)
9. **[#257](https://github.com/mplsllc/macsurf/issues/257)**: CSS `user-select`
