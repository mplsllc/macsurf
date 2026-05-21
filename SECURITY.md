# Security Policy

macsurf is a web browser that parses untrusted HTML, CSS, JavaScript, and untrusted network responses on a memory-constrained, 25-year-old operating system that predates modern OS-level security primitives. The combination is interesting: the attack surface looks more like a 1998 browser than a 2026 one, but the network it reaches is the modern web.

We take security reports seriously even though we're a small alpha project. Thank you for helping make macsurf safer.

## Scope

Vulnerabilities we're interested in:

- **Parser crashes / heap corruption** in HTML, CSS, or JS handling triggered by remote content (i.e. anything where a malicious page can crash or corrupt the browser).
- **Infinite loops or hangs** that lock the cooperative event loop, on Mac OS 9 this freezes the entire machine, not just macsurf.
- **TLS or proxy bypass**, anything that lets HTTPS-labelled traffic flow without the expected TLS verification, or that bypasses the local proxy chain.
- **Memory exhaustion attacks** that bypass our resource budgets (decoded-image cap, CSS cap, fetcher slot governor) to crash the browser deterministically from a remote page.
- **Cross-origin information leaks** in the DOM, JS bindings, or storage layers (cookies, local storage if/when implemented).
- **Path traversal** in any local-file or download-handling code path.

## Out of scope

The following are platform realities, not macsurf vulnerabilities, please don't report them:

- Mac OS 9 has no memory protection between processes.
- Mac OS 9 has no preemptive multitasking, any sufficiently long-running computation can hang the system.
- Mac OS 9's filesystem (HFS / HFS+) has limited permission semantics.
- The Carbon application partition is statically allocated; running out of memory leads to undefined behavior.

These are constraints we mitigate as best we can (yielding, budgets, defensive parsing), but they are not bugs in macsurf.

## Reporting a vulnerability

**Please use [GitHub Private Vulnerability Reporting](https://github.com/mplsllc/macsurf/security/advisories/new)** to report security issues privately. This routes the report to the maintainers without making it public until a fix is ready.

If you can't use Private Vulnerability Reporting for some reason, please open a **private** post in [GitHub Discussions](https://github.com/mplsllc/macsurf/discussions) and ask for a private contact channel. **Do not file public issues for security problems**, even if the issue seems minor.

Please include:

- A description of the vulnerability and its impact.
- Reproduction steps or a proof-of-concept (a minimal `.html`/`.css`/`.js` test case is ideal).
- Affected versions (macsurf release tag or `fixesNN` build).
- Mac OS version and hardware where you reproduced it.

## Response expectations

macsurf has one maintainer and is in alpha. Realistic timeline:

- **Acknowledgement** within **7 days** of report.
- **Initial assessment** (severity, scope, likely fix path) within **14 days**.
- **Fix timeline** depends on severity and platform constraints. Crashes-from-remote-content with a known fix typically ship in the next `fixesNN` round (usually under a week). Architectural issues may take longer.

If you haven't heard back within the acknowledgement window, please post a non-sensitive nudge in Discussions, the silence is almost certainly inbox failure, not indifference.

## Disclosure

We prefer **coordinated disclosure**. Once a fix is available and shipped in a release, we'll publish a security advisory and credit the reporter in the release notes, unless the reporter asks for anonymity, in which case the credit reads "anonymous reporter."

## Supported versions

During alpha, **only the latest tagged release** receives security fixes. Once macsurf reaches `v1.0`, this policy will be revised.

| Version | Status |
|---------|--------|
| latest tag (currently `0.1a2`) | supported |
| any earlier tag | not supported, please upgrade |
