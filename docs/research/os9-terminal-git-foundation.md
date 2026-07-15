# macShell — A Native Terminal and Git Client for Mac OS 9

**Foundation document / project handoff**
Status: **Proposed.** No code written.
Author: drafted 2026-07-10
Target: Mac OS 9.2.2, PowerPC G3/G4, CodeWarrior 8, C89, Carbon CFM under CarbonLib 1.4+

---

## 0. Thesis

A native command-line environment for Mac OS 9, whose foundational capability is **`git clone` / `git pull` over HTTPS**, and whose architecture admits new system-management tools (`wget`, `tar`, `gunzip`, a package manager) as first-class commands over time.

**It is also remotely drivable**, so that a developer — or an AI agent — on another machine can run commands on the Mac, build with CodeWarrior, and read back the results without anyone sitting at the G3. That capability is not an afterthought bolted on at the end; it is the reason the command layer is abstracted the way it is (§9).

This is not speculative. Three facts establish it:

1. **Git has already run on classic Mac OS.** MacRelix — Josh Juran's Unix-like environment for System 7 through Mac OS X 10.6, 68K and PPC — ships "sockets, a shell, and ports of perl and Git." It did this by emulating a Unix kernel inside one Mac application. We are not attempting anything that has not been done; we are doing a smaller, more focused version of it.
2. **Git's network protocol is designed for dumb clients.** Smart HTTP is two HTTP requests. No processes, no pipes, no signals, no ptys. A read-only clone is: one `GET`, one `POST`, parse the response. The protocol was explicitly built so a CGI script could serve it.
3. **MacSurf already contains most of the hard parts.** TLS 1.3, an HTTP/1.1 client, SHA-1, a CA bundle, an entropy source, a disk cache, and a cooperative I/O discipline that works on real hardware. The network layer is already factored out as a standalone library with its own CodeWarrior project.

The single genuinely missing primitive is **DEFLATE decompression**, and the canonical implementation of it is 200 lines of public C89 that allocates nothing.

Remote operation is likewise already proven at the transport layer: outbound TCP over Open Transport is green on hardware, and macTLS wraps it in verified TLS 1.3. The Mac dials out; nothing needs to listen (§9.1).

**Why this matters beyond convenience.** MacSurf's stated bottleneck is hardware verification — commits `fixes688–715` sit unverified awaiting a G3 pass, and DIRECTIVE #4 holds commits until hardware-confirmed. A remotely drivable shell that can invoke CodeWarrior over Apple Events (§9.7) turns that queue from a human bottleneck into an automated loop: pull, build, capture the error log, launch, read the debug log, report. **That is the real payoff of this project, and it arrives at Milestone 4b — before macGit even lands on the Mac.**

Everything below is grounded in verified specifications and inspected source. Where something is unverified, it is labelled.

---

## 1. What already exists

### 1.1 In this repository

| Asset | Location | Relevance |
|---|---|---|
| TLS 1.3 + 1.2 client | `macTLS/os9/ostls_*.c` | HTTPS transport for git smart HTTP |
| HTTP/1.1 client | `macTLS/os9/ostls_http.c`, `ostls_fetch.c` | Request/response, chunked, keep-alive |
| Async engine | `macTLS/os9/ostls_async.c` | Cooperative, non-blocking I/O over Open Transport |
| Standalone CW project | `macTLS/MacTLSTest/` | Proof the network stack links outside the browser |
| SHA-1 | `macTLS/bearssl/sha1.o` (BearSSL) | Git object IDs are SHA-1 |
| CA bundle | 121 Mozilla anchors, baked in | Verifying github.com / codeberg.org |
| Entropy | macEntropy, hardware-verified | TLS RNG |
| Disk cache | `macos9_disk_cache.c`, 64 MB LRU | Pattern for the object store |
| Cooperative I/O discipline | `OTUseSyncIdleEvents` + `YieldToAnyThread` notifier | Long fetches without freezing the UI |
| Carbon app skeleton | `frontends/macos9/main.c`, `window.c` | Event loop, `'carb'` resource, Appearance |
| Debug log channel | `macsurf_debug_log.c` | Crash-surviving forensics |

**`macTLS/os9/` is already an HTTPS library, not browser code.** `macos9_tls_fetcher.c` includes exactly two of its headers (`ostls_http.h`, `ostls_async.h`); everything else it includes is NetSurf. A new application links `macTLS/os9/` and has HTTPS on day one.

### 1.2 The one gap

`browser/netsurf/frontends/macos9/shims/zlib.h` is a 59-line stub. There is no inflate implementation anywhere in the tree. Section 4 closes this.

---

## 2. Prior art

Researched and verified. Each entry states what to take.

### 2.1 MacRelix — the existence proof

- **What:** A Unix-like environment running as a single classic Mac OS application, emulating a kernel internally. Implements `vfork()` + `execve()` (not full `fork()` — copying an address space without an MMU is the hard part, and `vfork`/`exec` sidesteps it), pipes, signals, `wait`, TCP sockets over Open Transport, a `/proc` filesystem, a `/sys/mac/…` view of Mac internals, a Bourne-style shell, a coreutils userland, a miniperl 5.6.1, **and a port of git**.
- **Platforms:** 68K and PPC, System 7 → Mac OS X 10.6 (OS X build is Carbon). Mac OS 9 PPC is squarely supported.
- **Source:** [github.com/jjuran/metamage_1](https://github.com/jjuran/metamage_1), under `lamp/Genie` and `relix/`.
- **License:** **AGPLv3-or-later.** This matters.
- **Take:** The *design* of in-address-space process emulation, and the confidence that comes from knowing git has run on this hardware. **Do not copy code** — AGPL is incompatible with the rest of this stack. If a conversation with Juran is possible, he has already solved problems we have not started.
- Links: [macrelix.org](https://www.macrelix.org/), [origins essay](https://www.metamage.com/text/relix/origins.html), [OSnews](https://www.osnews.com/story/139659/macrelix-a-unix-like-environment-that-runs-in-classic-mac-os/), [HN thread with Juran](https://news.ycombinator.com/item?id=40338443)

### 2.2 posix9 — the modern parallel

- **What:** An active, from-scratch POSIX shim over the classic Mac OS Toolbox. Implements `open`/`read`/`write`/`close`/`lseek`/`stat`, `opendir`/`readdir`/`mkdir`/`chdir`, BSD sockets over Open Transport, pthreads, signals, time.
- **License:** **Apache 2.0** — permissive, compatible.
- **Status:** Release dated 2026-02-26. PPC (Mac OS 7.5.2–9.2.2) is the primary target; 68K secondary. Socket layer is marked WIP ("Needs OT constants").
- **Stated goals:** hosting **Dropbear SSH, curl/wget, Python 2.x**, and Unix utilities.
- **Caveat:** targets **Retro68**, not CodeWarrior 8. And MacSurf's own OT+TLS stack is considerably more mature than posix9's WIP sockets.
- **Take:** Its POSIX-shim surface is the right reference for our platform layer's shape. Its author is chasing the same goal from the other end. Worth watching and worth talking to. **Not a dependency.**
- Link: [github.com/Scottcjn/posix9](https://github.com/Scottcjn/posix9/)

### 2.3 GUSI — the permissive substrate

- **What:** Grand Unified Socket Interface (Matthias Neeracher). POSIX file descriptors, BSD sockets over **both MacTCP and Open Transport**, a unified `select()` across files and sockets, and pthreads on the cooperative Thread Manager. System 7.0 → 9.x, 68K and PPC, builds under CodeWarrior.
- **No `fork()`.** GUSI is the I/O half of Unix, not the process half.
- **License: zlib** — permissive, directly reusable.
- **Pedigree:** MacPerl was built on it. The first Mac CVS port used it. Mozilla/Netscape's NSPR layered over the same abstraction, which carried into Classilla.
- **Take:** If we ever want a `select()` that spans files and sockets, this is proven, permissive, and battle-tested on exactly our toolchain family. Optional — our commands are synchronous and our network layer already yields correctly.
- Link: [sourceforge.net/projects/gusi](https://sourceforge.net/projects/gusi/)

### 2.4 MPW and ToolServer — the in-process tool ABI

- MPW Tools are files of type `'MPST'` that are **not separate processes**. The MPW Shell loads a tool's code into its own address space and calls a standard `main(argc, argv)`, supplying a runtime that maps `stdin`/`stdout`/`stderr` onto worksheet text or redirected files. "Pipes" run the left tool to completion into a temp store, then feed the right tool.
- ToolServer is the same engine, headless, driven by Apple Events.
- **Take:** The in-address-space tool model is the correct shape for OS 9, and MPW is its canonical precedent. We improve on it in one way (§8.3): pass a services struct rather than faking `argc`/`argv`/`stdout`.
- Link: [MacTech, "Porting Command Line Interface Programs to the Macintosh"](http://preserve.mactech.com/articles/mactech/Vol.16/16.07/CommandLinePorting/index.html)

### 2.5 MacCVS / cvsgui — the template for a native VCS client

- Native CVS clients for classic Mac OS. Not wrappers — **upstream CVS's protocol C code compiled for Mac** against a socket layer (the first port used CodeWarrior + GUSI 1.7.2 + cvs 1.84), wrapped in a PowerPlant GUI. pserver and rsh; no SSH.
- **Take:** This is the proven shape — take the protocol implementation, compile it against a Mac socket layer, add an event pump. We are doing the same thing with git's protocol instead of CVS's, and we get to write the protocol code ourselves because git's is small.
- Links: [cvsgui.sourceforge.net](https://cvsgui.sourceforge.net/), [sourceforge.net/projects/maccvspro](https://sourceforge.net/projects/maccvspro/)

### 2.6 SIOUX — the baseline to exceed

Metrowerks' console library. Link it, call `printf`, get a scrolling text window. It is a **cooked-mode console, not a terminal**: no escape handling, no cursor addressing, no raw mode, one window, and it owns the run loop.

**Take:** Use it as a zero-cost stdout sink during early bring-up, before the real terminal view exists. Then exceed it.

### 2.7 Terminal emulation

- **ZTerm** — the canonical Mac terminal; full VT100/ANSI, ANSI colour, ZMODEM. **Closed source.** Reference for UX only.
- **NCSA Telnet** — open source (public domain), VT100 over MacTCP/OT. The reusable lineage if we ever need a real VT.
- **[JulienPalard/vt100-emulator](https://github.com/JulienPalard/vt100-emulator)** — a headless ANSI/VT100 terminal emulator **written in C89**, structured as a reusable core: escape parser, terminal state machine, screen buffer. Minimal dependencies, no Toolbox. (Now at [git.afpy.org/mdk/hl-vt100](https://git.afpy.org/mdk/hl-vt100); check COPYRIGHT before use.)

**Take: we do not need a VT emulator for v1.** Our commands run in-process and write through a callback (§8.3). Escape-sequence parsing only becomes necessary when we attach to a *remote* shell (ssh, telnet). Defer it; the C89 core is there when we want it.

### 2.8 What does not exist

Stated plainly, because it defines the opportunity:

- **No standalone git client for classic Mac OS.** The only git that has ever run there is MacRelix's, hosted inside its emulated kernel.
- **No Subversion client for classic Mac OS.** SVN's dependency graph (APR, neon/serf, sqlite) never landed.
- **No general package manager for classic Mac OS.** Fink and MacPorts are Darwin-only. The closest were MacPerl's `cpan-mac` droplet and MacRelix's own Perl-scripted archive packager.

**macShell would be the first native git client and the first package manager on the platform.**

---

## 3. Architecture

### 3.1 Naming (placeholder — yours to choose)

Consistent with `macTLS`, `macQJS`, `macEntropy`:

- **macShell** — the Carbon terminal application
- **macGit** — the git engine (portable C89 core + Mac platform binding)
- **macPkg** — the package manager
- **macLib** — the shared platform layer

### 3.2 Layer diagram

```
   local console            remote agent (Linux / AI)
        │                              │
        │                     ┌────────▼────────┐
        │                     │  relay (HTTPS)  │
        │                     └────────┬────────┘
        │                              │ long-poll, outbound only
┌───────▼──────────────────────────────▼────────────────────────┐
│  macsh_session  — transport-agnostic command/output channel   │
│    console impl        │        reverse-HTTPS impl  ·  serial │
└───────────────────────────┬───────────────────────────────────┘
                            │
┌───────────────────────────▼───────────────────────────────────┐
│  macShell.app  (Carbon CFM, WaitNextEvent, QuickDraw)         │
│    terminal view · line editor · command dispatch             │
└───────────────┬───────────────────────────────┬───────────────┘
                │                               │
     ┌──────────▼──────────┐        ┌───────────▼───────────┐
     │  builtin commands   │        │  plugin commands      │
     │  ls cd cat pwd rm   │        │  CFM .shlb fragments  │
     │  fetch untar gunzip │        │  loaded on demand     │
     │  git  pkg  cw       │        │  (§8 HostServices ABI)│
     └──────────┬──────────┘        └───────────┬───────────┘
                └───────────────┬───────────────┘
                                │
              ┌─────────────────▼─────────────────┐
              │  cw → Apple Events → CodeWarrior  │
              │  MAKE · EXPT · crel · SvMs  (§9.7)│
              └─────────────────┬─────────────────┘
                                │
     ┌──────────────────────────▼──────────────────────────┐
     │  macGit           │  macPkg        │  archive        │
     │  protocol         │  index/verify  │  tar · gzip     │
     │  packfile+delta   │  install       │                 │
     │  object store     │                │                 │
     └──────────┬────────┴────────┬───────┴────────┬────────┘
                │                 │                │
     ┌──────────▼─────────────────▼────────────────▼────────┐
     │  macLib — platform layer                             │
     │  fs (FSRef) · inflate (puff) · sha1 · adler32/crc32  │
     │  term I/O · yield/interrupt                          │
     └──────────┬───────────────────────────────────────────┘
                │
     ┌──────────▼───────────────────────────────────────────┐
     │  macTLS/os9/  (EXISTS)  ostls_http · ostls_async     │
     │  BearSSL      (EXISTS)  sha1 · TLS primitives        │
     │  Open Transport                                       │
     └───────────────────────────────────────────────────────┘
```

### 3.3 The load-bearing design decision: **host-first**

**The git engine is portable C89 with no Toolbox calls. It is written and tested on Linux, against real GitHub and Codeberg, before a single line of it runs on a Mac.**

Nothing about git's protocol, packfile format, delta encoding, or object model is Mac-specific. The platform-dependent surface is four function pointers:

```c
typedef struct git_platform {
    int  (*https_request)(void *ctx, const char *method, const char *url,
                          const char *content_type, const char *body,
                          unsigned long body_len,
                          unsigned char **out, unsigned long *out_len);
    int  (*file_write)(void *ctx, const char *relpath,
                       const unsigned char *data, unsigned long len,
                       int is_text, int is_exec);
    int  (*dir_create)(void *ctx, const char *relpath);
    void (*progress)(void *ctx, const char *msg);
} git_platform;
```

On Linux, `https_request` is libcurl and `file_write` is `fopen`. On OS 9, they are `ostls_http` and `FSCreateFileUnicode`. The 3,000 lines in between are identical and are debugged with `gdb`, `valgrind`, and a test suite that clones real repositories.

This single decision moves the entire risk surface of the project onto a platform where debugging is cheap.

---

## 4. Compression (macLib)

### 4.1 Inflate — `puff.c`

**Decision: [puff.c](https://github.com/madler/zlib/blob/master/contrib/puff/puff.c) from zlib contrib.** zlib license. Mark Adler's own reference inflater, used to specify and validate zlib itself.

```c
int puff(unsigned char *dest, unsigned long *destlen,
         const unsigned char *source, unsigned long *sourcelen);
```

Four properties make it the correct choice, in priority order:

1. **It reports input bytes consumed.** On success (`return 0`), `*sourcelen` is set to the number of DEFLATE bytes actually consumed. This is the keystone requirement: a packfile has **no per-object length field**, so the only way to find object *k+1* is to fully consume object *k*'s zlib stream and learn where it ended. `puff` tells us. (Contrast: `sinfl.h` returns only output length and is therefore disqualified outright.)
2. **Strict C89, zero edits.** No `//`, no `inline`, no `long long`, no designated initializers, no declarations-after-statements. It compiles clean under `-std=c89 -pedantic`. The absence of `long long` also means it cannot trip the documented CW8 PPC 64-bit multiply miscompile.
3. **Zero heap allocation.** All state is one stack struct of ~80 bytes plus a `jmp_buf`. Huffman tables are small fixed local arrays.
4. **~200 lines of executable code** inside ~800 lines of RFC 1951 documentation.

Its one limitation — raw DEFLATE only, no zlib wrapper — is *useful* here. We skip the 2-byte header ourselves and step over the 4-byte Adler-32 ourselves, which means we always know exactly where we are:

```
next_object_offset = obj_start
                   + git_type_size_varint_len
                   + 2                        /* zlib CMF/FLG */
                   + sourcelen                /* from puff() */
                   + 4;                       /* Adler-32 trailer */
```

**Documented fallback:** the zlib inflate-only subset (`inflate.c`, `inftrees.c`, `inffast.c`, `adler32.c`, `zutil.c`) is C90-clean, has a proven CodeWarrior history (Classilla shipped a CW-built zlib), and its `z_stream.total_in` reports consumed bytes *including* wrapper and trailer. ~10× puff's footprint. Take it if we ever need true streaming for very large delta chains. Exclude `gzlib.c`/`gzread.c`/`gzwrite.c` (they pull `<unistd.h>`/`fdopen`) and use a simple table-driven `crc32.c` (≤1.2.11) or our own.

**puff return codes:** `2` = ran out of input, `1` = ran out of output, `0` = success, negative = corrupt (−1 bad BTYPE, −2 stored LEN≠~NLEN, −3..−11 Huffman/distance errors). On positive returns the out-params are *unchanged*, so a retry with a bigger buffer is safe.

### 4.2 Deflate — we do not need a compressor

To *write* git objects (loose objects, and packs for push), we need a valid zlib stream. **A zlib stream composed entirely of DEFLATE stored blocks (BTYPE=00) is fully valid** — it is exactly what real zlib emits at `Z_NO_COMPRESSION`. Every conformant inflater, including git's, accepts it.

Stored-block encoder, complete:

```
emit 0x78 0x01                          /* zlib header: CM=8, CINFO=7, FLEVEL=0.
                                           0x7801 = 31 * 991, satisfies FCHECK   */
for each chunk of up to 65535 bytes of P:
    emit (is_last_chunk ? 0x01 : 0x00)  /* BFINAL in bit0, BTYPE=00 in bits1-2   */
    emit LEN   as 2 bytes little-endian
    emit ~LEN  as 2 bytes little-endian
    emit the chunk bytes verbatim
emit Adler32(P) as 4 bytes BIG-endian
```

The empty stream is 11 bytes: `78 01 01 00 00 FF FF 00 00 00 01`.

Note the byte-order trap: **zlib uses Adler-32, big-endian trailer. gzip uses CRC-32, little-endian trailer.** Same DEFLATE core, opposite conventions.

### 4.3 Checksums

**Adler-32** (RFC 1950), `MOD = 65521`:

```c
#define ADLER_MOD 65521UL
unsigned long adler32_(unsigned long adler,
                       const unsigned char *buf, unsigned long len)
{
    unsigned long a = adler & 0xffffUL;
    unsigned long b = (adler >> 16) & 0xffffUL;
    unsigned long i;
    if (buf == 0) return 1UL;            /* initial value is 1 */
    for (i = 0; i < len; i++) {
        a += buf[i]; if (a >= ADLER_MOD) a -= ADLER_MOD;
        b += a;      if (b >= ADLER_MOD) b -= ADLER_MOD;
    }
    return (b << 16) | a;
}
```

**CRC-32** (RFC 1952, gzip only), reflected polynomial `0xEDB88320`, init/final XOR `0xFFFFFFFF`, table-driven, 256 entries. Standard reference implementation; C89 clean.

### 4.4 gzip container (RFC 1952)

10-byte fixed header: `ID1=0x1F`, `ID2=0x8B`, `CM=8`, `FLG`, `MTIME[4] LE`, `XFL`, `OS`. FLG bits: `FTEXT=1 FHCRC=2 FEXTRA=4 FNAME=8 FCOMMENT=16`. Optional fields in order: FEXTRA (`XLEN[2] LE` + data), FNAME (NUL-terminated), FCOMMENT (NUL-terminated), FHCRC (2 bytes). Then raw DEFLATE. Then `CRC32[4] LE`, `ISIZE[4] LE`.

For our purposes gzip is *header skipping* — parse the 10 bytes, skip the optional fields, hand the DEFLATE payload to `puff`.

### 4.5 tar (POSIX ustar)

512-byte header blocks. Field offsets: `name[100]` @0, `mode[8]` @100, `uid[8]` @108, `gid[8]` @116, `size[12]` @124 (octal ASCII), `mtime[12]` @136, `chksum[8]` @148, `typeflag[1]` @156, `linkname[100]` @157, `magic[6]="ustar\0"` @257, `version[2]="00"` @263, `uname[32]` @265, `gname[32]` @297, `devmajor[8]` @329, `devminor[8]` @337, `prefix[155]` @345.

Effective path = `prefix` + `/` + `name` when `prefix` is non-empty.

Checksum: sum all 512 bytes as unsigned, treating the 8 `chksum` bytes (148–155) as ASCII spaces. Be lenient on read — accept both unsigned and signed byte sums (historical writers disagreed on `char` signedness).

typeflags: `'0'`/`'\0'` regular, `'5'` directory, `'2'` symlink, `'1'` hardlink. Extensions: `'L'`/`'K'` GNU long name/link, `'x'` pax per-file header, `'g'` pax global header.

> **Concrete gotcha, must handle:** GitHub's source tarballs (`git archive` output) write a **pax global header (`'g'`) as the very first entry**, carrying a `comment=<commit-sha>` record. A parser that doesn't read its `size` and skip its data blocks desyncs on entry one. Recognize `'x'`, `'g'`, `'L'`, `'K'`; read the size; apply the override or skip.

EOF: two consecutive all-zero 512-byte blocks. Be tolerant — some producers write one, or just end the stream.

### 4.6 The early payoff

§4 alone, wired to `ostls_http`, gives macShell:

```
fetch https://codeload.github.com/user/repo/tar.gz/refs/heads/master
gunzip repo.tar.gz
untar repo.tar
```

**That is a working source-update mechanism for MacSurf contributors before macGit exists at all.** It is Milestone 1, and it is worth shipping on its own.

---

## 5. macGit

### 5.1 Scope

**v1 is read-only, shallow, and single-branch:**

```
macgit clone --depth 1 <https-url> [dir]
```

**v2 adds** incremental fetch (`have` negotiation, thin-pack resolution) → real `git pull`.
**v3 adds** push (stored-block deflate + `git-receive-pack`).

### 5.2 Protocol: smart HTTP, version 0

Use **protocol v0**, not v2. In v0 the refs arrive for free in the `GET`; v2 requires a separate `ls-refs` command round-trip. v0 is strictly less code for a read-only client. (Send no `Git-Protocol` header and you get v0.)

**Step 1 — ref discovery**

```
GET $URL/info/refs?service=git-upload-pack
User-Agent: git/macgit-0.1
Accept: */*
```

`$URL` is the repo base, e.g. `https://github.com/user/repo.git`.

Response: status 200, `Content-Type: application/x-git-upload-pack-advertisement`.

> Per `gitprotocol-http`: *"Clients MUST validate the first five bytes of the response entity matches the regex `^[0-9a-f]{4}#`. If this test fails, clients MUST NOT continue."*

Body:

```
001e# service=git-upload-pack\n
0000
<len><sha> HEAD\0<space-separated capabilities>\n     <- caps ONLY on the first ref line
<len><sha> refs/heads/master\n
<len><sha> refs/tags/v1.0\n
<len><sha> refs/tags/v1.0^{}\n                        <- "^{}" = peeled tag
0000
```

Split the first ref line at the NUL. The capability list contains, among others, `symref=HEAD:refs/heads/main` — use it to write `.git/HEAD`.

**Step 2 — the fetch**

```
POST $URL/git-upload-pack
Content-Type: application/x-git-upload-pack-request
Accept: application/x-git-upload-pack-result
```

Body (depth-1 shallow clone, no local objects, therefore no `have` lines):

```
<len>want <sha> side-band-64k ofs-delta no-progress shallow\n
000fdeepen 1\n
0000
0009done\n
```

**Capabilities to send:** `side-band-64k ofs-delta no-progress` (+ `shallow` when deepening).

**Capabilities to deliberately omit, and why:**

| Omit | Reason |
|---|---|
| `thin-pack` | **Critical.** Without it the server MUST NOT send deltas whose base is outside the pack. |
| `multi_ack`, `multi_ack_detailed`, `no-done` | Negotiation state we don't need — a clone sends no `have` lines. |
| `filter` | Partial clone; not needed. |
| `include-tag` | Harmless but unnecessary. |

> **The key guarantee, stated precisely:** `deepen` does not by itself make a pack thin — thinness is controlled *solely* by the `thin-pack` capability. **A depth-1 clone without `thin-pack` is guaranteed self-contained**: every delta's base object, whether OFS_DELTA or REF_DELTA, is present somewhere in the same pack. No local object store is required to resolve it.
>
> Depth-1 does *not* mean "no deltas" — the server may still deltify the objects it sends against each other. The resolver must handle both delta types even at depth 1.

### 5.3 pkt-line

```
pkt-line    = data-pkt / flush-pkt
data-pkt    = pkt-len pkt-payload
pkt-len     = 4 lowercase ASCII hex digits
```

**The 4-byte length includes itself.** Payload length = `parsed_len - 4`. Max total line 65520, max payload 65516.

Magic values, which carry **no payload**: `0000` flush-pkt, `0001` delim-pkt (v2), `0002` response-end-pkt (v2), `0003` reserved. A reader must branch on the numeric value *before* attempting to read a body. (`0004` is a legal empty line and must be distinguished from flush.)

### 5.4 Response: shallow section, ack, sideband

1. **Shallow update** (only if we sent `deepen`):
   ```
   0035shallow 5a3f6be755bbb7deae50065988cbfa1ffa9ab68a\n
   0000
   ```
   Each `shallow <sha>` is a boundary commit whose parents are not in the pack. Record every one into `.git/shallow`.

2. **Acknowledgement**, not sideband-wrapped: `0008NAK\n` — expected on a fresh clone.

3. **Sideband-multiplexed packfile.** Each pkt-line payload's first byte is a band code:

   | Band | Meaning | Action |
   |---|---|---|
   | `\x01` | packfile data | append to pack buffer |
   | `\x02` | progress text | ignore |
   | `\x03` | fatal error | abort, surface the message |

   Terminated by flush-pkt. Reconstruct the packfile by concatenating band-1 payloads in order.

### 5.5 Packfile v2

```
"PACK"          4 bytes
version         4 bytes big-endian, = 2
nr_objects      4 bytes big-endian
<objects>       nr_objects entries, back to back, NO length fields
SHA-1 trailer   20 bytes, over every preceding pack byte
```

**Per-object header** — type + *uncompressed* size varint:

```c
/* first byte: bit7 = continuation, bits6-4 = type, bits3-0 = low 4 size bits.
   subsequent bytes: bit7 = continuation, bits6-0 = next 7 size bits (LSB-first). */
static int obj_hdr(const unsigned char *p, const unsigned char *end,
                   int *type, unsigned long *size)
{
    const unsigned char *q = p; unsigned c; int shift;
    if (q >= end) return -1;
    c = *q++;
    *type = (c >> 4) & 7;
    *size = c & 15;
    shift = 4;
    while (c & 0x80) {
        if (q >= end) return -1;
        c = *q++;
        *size |= (unsigned long)(c & 0x7f) << shift;
        shift += 7;
    }
    return (int)(q - p);
}
```

> `*size` is the **uncompressed** object size — used to allocate the output buffer, not to find the next object. The compressed length comes from `puff`'s `*sourcelen`.

**Object types:** `1` commit, `2` tree, `3` blob, `4` tag, `5` reserved/invalid, `6` OFS_DELTA, `7` REF_DELTA.

After the header: for types 1–4, a zlib stream of the content. For type 6, the offset varint (§5.6) then a zlib stream of delta instructions. For type 7, 20 raw base-SHA bytes then a zlib stream of delta instructions.

### 5.6 OFS_DELTA offset encoding — **not the same varint**

This is the single most commonly mis-implemented piece of the format. It has a bias term.

```c
/* p points at the first offset byte. Returns bytes consumed.
   The result is a NEGATIVE offset from this object's own header byte. */
static int ofs_delta_base(const unsigned char *p, const unsigned char *end,
                          unsigned long *ofs)
{
    const unsigned char *q = p; unsigned c; unsigned long o;
    if (q >= end) return -1;
    c = *q++;
    o = c & 0x7f;
    while (c & 0x80) {
        if (q >= end) return -1;
        o += 1;                     /* the +1 per continuation IS the 2^7k bias */
        c = *q++;
        o = (o << 7) + (c & 0x7f);
    }
    *ofs = o;
    return (int)(q - p);
}
/* base_header_offset = this_object_offset - o */
```

Verified by hand: bytes `86 68` decode to 1000.

Because the offset is always subtracted, **an OFS_DELTA base always precedes its delta in the pack** — which guarantees single-forward-pass resolvability if we keep a `pack_offset → resolved_object` map. This is why we request `ofs-delta`.

REF_DELTA bases are 20 raw SHA-1 bytes and may appear anywhere in the pack; defer unresolved ones to a second pass.

### 5.7 Delta instructions

The inflated delta stream is: `source_size` varint, `target_size` varint (both plain LSB-first 7-bit varints, `shift` starting at 0 — *not* the type+size form), then instructions until the stream ends.

```c
static int apply_delta(const unsigned char *base, unsigned long base_len,
                       const unsigned char *d, const unsigned char *dend,
                       unsigned char *dst, unsigned long dst_len)
{
    unsigned long src_sz, tgt_sz;
    unsigned char *out = dst;
    d = get_varint(d, dend, &src_sz);
    d = get_varint(d, dend, &tgt_sz);
    if (src_sz != base_len || tgt_sz != dst_len) return -1;
    while (d < dend) {
        unsigned c = *d++;
        if (c & 0x80) {                        /* COPY from base */
            unsigned long off = 0, sz = 0;
            if (c & 0x01) off  =  *d++;
            if (c & 0x02) off |= (unsigned long)(*d++) << 8;
            if (c & 0x04) off |= (unsigned long)(*d++) << 16;
            if (c & 0x08) off |= (unsigned long)(*d++) << 24;
            if (c & 0x10) sz   =  *d++;
            if (c & 0x20) sz  |= (unsigned long)(*d++) << 8;
            if (c & 0x40) sz  |= (unsigned long)(*d++) << 16;
            if (sz == 0) sz = 0x10000;         /* size 0 means 65536 */
            if (off + sz > base_len) return -1;
            memcpy(out, base + off, sz); out += sz;
        } else if (c) {                        /* INSERT literal, 1..127 bytes */
            memcpy(out, d, c); out += c; d += c;
        } else return -1;                      /* c == 0 is invalid */
    }
    return ((unsigned long)(out - dst) == tgt_sz) ? 0 : -1;
}
```

A delta's resulting object has the **same type as its base**. Resolve type transitively down the chain to the first non-delta base.

### 5.8 Object IDs

```
oid = SHA-1( "<type> <decimal-size>\0" + <content> )
```

`<type>` is the ASCII word `blob`, `tree`, `commit`, or `tag`. The hash is over the **uncompressed** object with this header — never over the zlib bytes. Example: `blob 16\0what is up, doc?` → `bd9dbf5aae1a3862dd1526723246b20206e5fc37`.

Verify every reconstructed object against its computed OID. Verify the 20-byte pack trailer against SHA-1 of all preceding pack bytes. These two checks are the integrity guarantee, and BearSSL's `br_sha1_*` provides the hash.

### 5.9 Tree and commit objects

**Tree** content — entries back to back, no separators, no trailing newline:

```
<mode-octal-ascii> SP <filename> NUL <20-byte-binary-sha>
```

Modes: `100644` regular, `100755` executable, `120000` symlink, `40000` directory (**five digits, no leading zero** — `git ls-tree` *displays* `040000` but the stored bytes are `40000`), `160000` gitlink/submodule (skippable).

**Commit** content — line-oriented; read headers until the first blank line:

```
tree <40-hex>\n
parent <40-hex>\n        (0..N lines)
author ...\n
committer ...\n
\n
<message>
```

We need `tree` (always first) and `parent`. Stop walking parents at any SHA listed in `.git/shallow`.

### 5.10 Landing on disk

**Explode every object to a loose file.** Do not write a `.idx`.

```
.git/objects/<first-2-hex>/<remaining-38-hex>
   contents = zlib( "<type> <size>\0" + <content> )
```

The zlib framing uses our stored-block writer (§4.2). Stock git reads loose objects natively, so the result is a real repository that `git status`, `git log`, and `git fsck` accept — with no `.idx` implementation, no fanout table, and no large-offset handling.

The `.idx` v2 format is specified (magic `\377tOc`, version 2, 256×4 fanout, sorted SHA table, CRC table, offset table with MSB-flagged large-offset indirection, two trailing SHA-1s) and is written down in the appendix in case we later want to keep packs as packs. **It is not on the v1 path.**

**Minimum `.git` that stock git accepts:**

```
.git/
├── HEAD                  "ref: refs/heads/main\n"   (from the symref= capability)
├── config                [core] repositoryformatversion=0, filemode=true,
│                                 bare=false, logallrefupdates=true
├── refs/heads/main       "<40-hex>\n"
├── objects/xx/yyy…       exploded loose objects
└── shallow               one 40-hex commit OID per line, \n-terminated
                          (present ONLY for a shallow clone)
```

### 5.11 Authentication

Private repos and push: **HTTP Basic over HTTPS** with a personal access token.

```
Authorization: Basic base64("x-access-token:" + <PAT>)
```

Send it proactively to skip the 401 round-trip. `401` means retry with credentials; `403` means authenticated but forbidden — do not retry.

### 5.12 Push (v3, deferred but unblocked)

Discovery mirrors §5.2 with `service=git-receive-pack`. The update request's **first** command line carries the capabilities:

```
<old-sha> SP <new-sha> SP <refname> NUL <caps>\n
<old-sha> SP <new-sha> SP <refname>\n        (subsequent, no caps)
0000
PACK…                                         (packfile immediately after the flush)
```

`zero-id` is 40 ASCII `0`s. A packfile MUST be sent for any create/update, even an empty one (`PACK` + version 2 + count 0 + 20-byte trailer = 32 bytes).

**Push is not gated on writing a compressor.** The pack's object streams are zlib, and stored blocks (§4.2) are valid zlib. Receive-pack capabilities are limited to `report-status`, `report-status-v2`, `delete-refs`, `ofs-delta`, `atomic`, `push-options`.

---

## 6. macShell — the terminal

### 6.1 The transcript view

**Decision: hand-drawn QuickDraw cell grid over a line ring buffer, with a Control Manager scrollbar.**

- **Not TextEdit.** `TERec.teLength` is a signed 16-bit `short` — a hard 32,767-byte ceiling. A terminal with scrollback exceeds it immediately. TE remains correct for the single-line *input* field, which is exactly how MacSurf already uses it.
- **Not MLTE.** `TXNObject`/ATSUI is available under CarbonLib on OS 9 and does give >32K Unicode text with scrollbars, but it is an *editor* model: no cheap `(row, col)` addressing, no in-place line rewrite, and it drags in ATSUI. (MacSurf's own ATSUI history counsels caution. *Unverified:* MLTE stability under heavy use on real G3/G4 hardware.)
- **The cell grid** gives exact cursor addressing, in-place rewrite for `\r` overwrite and progress lines, trivial redraw, no size ceiling, and it reuses MacSurf's proven QuickDraw and scrollbar work.

**Scrollbar:** `kControlScrollBarProc = 384`. **Not** `kControlScrollBarLiveProc = 386` — the live-tracking CDEF crashes on real G3/G4 hardware (documented in `window.c`).

### 6.2 Font and metrics

Monaco is the guaranteed-present monospaced font on OS 9 (Courier is the other standard fixed-pitch face).

```c
short fnum; FontInfo fi; short cellW, cellH, ascent;
GetFNum("\pMonaco", &fnum);
TextFont(fnum); TextFace(normal); TextSize(9);
GetFontInfo(&fi);
ascent = fi.ascent;
cellH  = fi.ascent + fi.descent + fi.leading;
cellW  = CharWidth('M');            /* derive from a real glyph, never from ' ' */
/* cell (row,col) baseline: */
MoveTo(left + col*cellW, top + row*cellH + ascent);
```

Derive `cellW` from `'M'`, **not** from a space — MacSurf has an open bug where `CharWidth(' ')` returns 0 in some face/size combinations. Reset port state (`RGBForeColor(black)`, `RGBBackColor(white)`, font, size) before drawing; QuickDraw port state is sticky and has bitten this project before.

### 6.3 Keyboard

From a `keyDown`/`autoKey` `EventRecord`:

```c
charCode = message & charCodeMask;         /* 0x000000FF — translated character */
keyCode  = (message & keyCodeMask) >> 8;   /* 0x0000FF00 — raw virtual key code */
```

Modifiers: `cmdKey=0x0100`, `shiftKey=0x0200`, `alphaLock=0x0400`, `optionKey=0x0800`, `controlKey=0x1000`.

Use `charCode` for text; reserve raw `keyCode` for navigation: Return `0x24`, Enter `0x4C`, Tab `0x30`, Delete `0x33`, Fwd-Delete `0x75`, Escape `0x35`, Left `0x7B`, Right `0x7C`, Down `0x7D`, Up `0x7E`, Home `0x73`, End `0x77`, PageUp `0x74`, PageDown `0x79`.

**Ctrl-C** arrives either as `(modifiers & controlKey)` with `charCode=='C'`, or directly as `charCode == 0x03` (ETX). Accept both.

Keep the `WaitNextEvent` mask a tight whitelist of classic event kinds — feeding unexpected event classes to CarbonLib's dispatcher has crashed this project before (the mouse-wheel saga).

### 6.4 Cursor blink and line editing

No timer thread. Everything runs off the `nullEvent` pass:

```c
if (TickCount() >= gNextBlink) {
    invert_cursor_cell();
    gNextBlink = TickCount() + GetCaretTime();
}
```

Pass `GetCaretTime()` as `WaitNextEvent`'s sleep so we wake in time to blink but yield CPU otherwise.

Line editing is an edit buffer plus an insertion index: printable → insert and repaint from index; Left/Right → move and clamp; Delete → remove before index; Home/End → 0 / len; Return → commit, push to history ring, clear. Up/Down walk a history ring and replace the buffer. All synchronous, in-loop, no re-entrancy.

### 6.5 Working directory

**OS 9 has no per-process cwd.** `HGetVol`/`HSetVol` expose a *process-global* default directory that other Toolbox calls mutate; Carbon discourages relying on it.

Keep the working directory as application state: **an `FSRef` to the directory**. `cd <name>` = `FSMakeFSRefUnicode` from the current ref. `cd ..` = `FSGetCatalogInfo(..., &parentRef, ...)`. **Never call `HSetVol`.**

### 6.6 Cooperative concurrency and Ctrl-C

A running command is a function call on our own stack. There is no signal delivery and no thread to kill. Abort is cooperative:

1. Long-running commands periodically call `svc->yield()` (which pumps the event loop) and check `svc->interrupted()`.
2. The keyDown handler sets `gInterrupt = 1` on Ctrl-C.
3. The command sees the flag and **returns early**. That return *is* the abort.

Yield points are places the command *chose* to check: per I/O chunk, per file in a checkout, per object in a pack walk. This is the same discipline NetSurf's fetcher ring already follows in MacSurf.

For network I/O, reuse the existing pattern: `OTUseSyncIdleEvents(ep, true)` plus a notifier that calls `YieldToAnyThread()` on `kOTSyncIdleEvent`. Optionally run commands on a cooperative Thread Manager thread (`NewThread(kCooperativeThread, …)`) that yields. Both keep `WaitNextEvent` serviced.

---

## 7. Filesystem — writing a checkout onto HFS+

### 7.1 Use FSRef, not FSSpec

`FSCreateFileUnicode`, `FSCreateDirectoryUnicode`, `FSMakeFSRefUnicode`, `FSGetCatalogInfo`, `FSSetCatalogInfo`, `FSpMakeFSRef` are all annotated *CarbonLib 1.0 and later / InterfaceLib 9.0 and later* in `CarbonCore/Files.h`. OS 9.2.2 ships CarbonLib 1.4–1.6. They are present.

`FSSpec`'s name field is a `Str63` Pascal string in a Mac text encoding, not Unicode. A git tree with UTF-8 names cannot be represented through it. **Represent every node as an `FSRef` plus an `HFSUniStr255` name.**

### 7.2 Do not use FSPathMakeRef

`FSPathMakeRef`/`FSRefMakePath` exist under CarbonLib 1.1 and *do* run on OS 9 (confirmed by the FreeType-Carbon maintainers in 2003). Do not build the checkout on them, for one decisive reason:

> `FSPathMakeRef` requires **forward-slash** POSIX paths, but **MSL's `fopen()` expects colon-separated HFS paths.** MacSurf already writes files through MSL `fopen` (cookie store, disk cache). Two path worlds, two separators, no round-trip.

Split the git path on `/` ourselves and walk components. There is no `mkdir -p` primitive; the component loop *is* `mkdir -p`:

```c
/* for each slash-delimited component: */
utf8_to_hfsunistr255(component, &uname);
err = FSMakeFSRefUnicode(&cur, uname.length, uname.unicode,
                         kTextEncodingUnknown, &child);
if (err == fnfErr) {                                  /* -43: doesn't exist */
    if (is_last && is_file)
        err = FSCreateFileUnicode(&cur, uname.length, uname.unicode,
                                  kFSCatInfoNone, NULL, &child, NULL);
    else
        err = FSCreateDirectoryUnicode(&cur, uname.length, uname.unicode,
                                       kFSCatInfoNone, NULL, &child, NULL, NULL);
}
if (err != noErr) return err;                          /* incl. dupFNErr — §7.4 */
cur = child;
```

### 7.3 Names and limits

- **`:` is the path separator and is illegal inside a filename.** Reject or escape it.
- **`/` is a *legal* character in an HFS+ filename** at the volume-format level — the mirror image of Unix. Since we split on `/` ourselves, treat it as separator-only.
- **Max filename: 255 UTF-16 code units** (the old HFS 31-char limit is gone). Note that NFD decomposition and surrogate pairs each consume 2 units.
- **No hard path-depth limit** in the HFS+ format. Since we navigate by FSRef + component and never materialize a long path string, deep trees are fine.

### 7.4 The two HFS+ traps

**Case-insensitivity.** Standard OS 9 HFS+ is case-insensitive, case-preserving. A tree containing both `README` and `readme` cannot exist on disk. `FSCreateFileUnicode` returns **`dupFNErr` (-48)** on the second. Detect it: on `dupFNErr`, resolve the existing entry and compare names — if they differ only by case, report a **case collision** explicitly rather than silently overwriting. (This is exactly the class of problem git flags with `core.ignorecase`.)

**Unicode normalization.** HFS+ stores names in a variant of **NFD (decomposed)**. Git stores raw bytes, usually NFC. A name written as precomposed `é` (U+00E9) reads back as `e` + combining acute (U+0065 U+0301). Normalize on the way in and on the way out, or `git status` reports phantom modifications. This is git's `core.precomposeunicode` problem, and the OS will not solve it for us.

### 7.5 Line endings and type/creator

**Line endings are entirely the app's job.** No OS facility. On checkout, translate LF→CR for text blobs; on hashing/status, translate CR→LF before computing the OID, or every file reads as modified. Decide text-vs-binary the way git does — `.gitattributes`, else a NUL-byte heuristic.

This is where MacSurf's `sed 's/$/\r/'` shipping rule becomes code.

**Type/creator**, so sources open in CodeWarrior on double-click:

```c
FSCatalogInfo ci;
FileInfo *fi = (FileInfo *)ci.finderInfo;
FSGetCatalogInfo(&ref, kFSCatInfoFinderInfo, &ci, NULL, NULL, NULL);
fi->fileType    = 'TEXT';
fi->fileCreator = 'CWIE';        /* CodeWarrior. Four-char codes ARE case-sensitive. */
FSSetCatalogInfo(&ref, kFSCatInfoFinderInfo, &ci);
```

Read-modify-write so the Finder flags and window location survive.

---

## 8. Extensibility — the plugin ABI

### 8.1 Commands as CFM shared libraries

There is no `fork`, no `exec`, and no `PATH`. Commands are code loaded into our address space. Two tiers:

- **Builtins** — entries in a function-pointer table, compiled into macShell.
- **Plugins** — CFM shared library fragments (`.shlb`) in a `Tools:` folder, loaded on demand.

The plugin tier is what makes `pkg install wget` mean something concrete: **a package *is* a `.shlb` plus a manifest.**

```c
OSErr GetDiskFragment(const FSSpec *fileSpec, UInt32 offset, UInt32 length,
                      ConstStr63Param fragName, CFragLoadOptions options,
                      CFragConnectionID *connID, Ptr *mainAddr, Str255 errName);

OSErr FindSymbol(CFragConnectionID connID, ConstStr255Param symName,
                 Ptr *symAddr, CFragSymbolClass *symClass);

OSErr CloseConnection(CFragConnectionID *connID);
```

Header: `<CodeFragments.h>`. Bridge FSRef → FSSpec via `FSGetCatalogInfo(..., NULL, &spec, ...)`.

### 8.2 The transition-vector rule

On PowerPC CFM, **a function pointer is the address of a transition vector** — an 8-byte `{ codeAddress, tocAddress }` pair. Cross-fragment glue loads GPR2 (RTOC) from the vector before branching.

`FindSymbol` on an exported function returns `symClass == kTVectorCFragSymbol` and `symAddr` = **the address of that transition vector**. That is precisely what a C function pointer is. So:

```c
typedef long (*CmdProc)(HostServices *svc, int argc, char **argv);
Ptr p; CFragSymbolClass cls; CmdProc cmd;
FindSymbol(conn, "\pmacsh_cmd", &p, &cls);   /* cls == kTVectorCFragSymbol */
cmd = (CmdProc)p;                             /* CORRECT — p is already a TVector */
result = cmd(&services, argc, argv);
```

**The crash class** is casting a *raw code address* to a function pointer. The glue then reads 8 bytes of instructions as `{code, toc}`, sets RTOC to garbage, and branches into nonsense — PC/LR in low or garbage memory. Always call through the `kTVectorCFragSymbol` address.

This is adjacent to, but distinct from, two crashes this project has already paid for:
- MacSurf's `project_ot_transition_vectors` — resolving OT by `FindSymbol` + hand-rolled pointer crashed on TOC mismatch. Fix: link the import library, call through the prototype.
- The UPP/`RoutineDescriptor` crash in `window.c` — under CarbonLib, MixedMode still expects a real RoutineDescriptor; casting a bare function pointer to a UPP branches to `bl 0`.

**Rule: keep host↔plugin calls on compiler-generated glue. Never hand-build a pointer.**

### 8.3 HostServices, not `main(argc, argv)`

There is no process, no real `stdout`, and nowhere for a bare `main` to write. Pass a services struct:

```c
typedef struct HostServices {
    UInt32  abiVersion;                                   /* plugin checks; bump on change */
    void  (*out)(void *ctx, const char *utf8, long len);  /* -> terminal transcript */
    void  (*err)(void *ctx, const char *utf8, long len);
    int   (*interrupted)(void *ctx);                      /* Ctrl-C poll  (§6.6) */
    void  (*yield)(void *ctx);                            /* cooperative yield (§6.6) */
    OSErr (*cwd)(void *ctx, FSRef *out);                  /* working directory (§6.5) */
    void   *ctx;
} HostServices;

pascal long macsh_cmd(HostServices *svc, int argc, char **argv);
```

The plugin's "stdout" is `svc->out`. Its exit status is the return value. It stays responsive by polling `svc->interrupted()` and calling `svc->yield()` in its inner loops.

**Do not retarget MSL's `printf`/`stdout`.** Overriding `InstallConsole`/`WriteCharsToConsole` is global, fragile, and re-entrancy-hostile. The explicit callback is cleaner and versionable.

Plugins run unsandboxed in our address space. A bad plugin can corrupt the host. That is the OS 9 bargain and it is the same one CodeWarrior itself makes.

---

## 9. Remote access and agent control

The goal: a developer or an AI agent on another machine issues commands to the Mac, receives output, and can drive a full CodeWarrior build — unattended.

### 9.1 The Mac dials out. Nothing listens.

This is not a preference. It is settled by hardware evidence already in this project.

> **Verified on real OS 9.1 / G3 hardware over 14 rounds (2026-05-19):** Carbon CFM applications cannot `OTBind` an endpoint to a caller-supplied `InetAddress`. Every combination of address, `qlen`, protocol stack, sync/async dispatch, and `InContext` variant returns `kOTBadAddressErr (-3150)`. Tested across ports 8765/12345/0, `INADDR_ANY` and the real LAN IP, plain `tcp` and `tilisten,tcp`, with and without `OTSetBlocking`.
>
> **Verified working:** outbound TCP via `OTOpenEndpointInContext` + `OTBind(NULL, NULL)` + `OTConnect`.
>
> — `project_carbon_ot_passive_bind`, commits `fixes16..fixes34` on macTLS `main`

The rule that memory records is blunt and correct: *if you see `-3150` from `OTBind` in a Carbon CFM app, don't iterate on bind args — the failure is the platform.*

Therefore the remote session is a **reverse channel**: the Mac makes an outbound HTTPS connection and long-polls for work. This uses precisely the path that is green on hardware, wraps it in macTLS's verified TLS 1.3, traverses NAT and firewalls with no configuration, and needs no inbound rule anywhere.

It is also, independently, the right architecture. It is how every modern CI runner and agent worker connects.

### 9.2 The session abstraction

Command code must never know where its input came from or where its output goes. One interface, three implementations:

```c
typedef struct macsh_session {
    /* Block until a command line is available. Returns length, 0 on EOF, <0 on error.
       MUST pump the event loop while waiting. */
    long (*read_command)(void *ctx, char *buf, long cap);

    /* Stream output as it is produced — do not buffer to completion. */
    void (*write_out)(void *ctx, const char *utf8, long len);
    void (*write_err)(void *ctx, const char *utf8, long len);

    /* Report the finished command's exit status. */
    void (*command_done)(void *ctx, long status);

    /* Non-blocking: has an interrupt arrived on this channel? */
    int  (*interrupted)(void *ctx);

    void (*close)(void *ctx);
    void  *ctx;
} macsh_session;
```

The local console is one implementation. The reverse-HTTPS agent is another. Serial is a third. **They can be attached simultaneously** — the transcript mirrors to every open session, so a human at the Mac watches an agent work in real time and can hit Ctrl-C.

`HostServices` (§8.3) is populated *from* the active session, so plugin commands inherit remote operation for free without knowing it exists.

### 9.3 Transport A — reverse HTTPS agent *(primary)*

```
Mac                                    relay                         agent (Linux/AI)
 │                                       │                                  │
 ├─ GET  /v1/session/<id>/next ─────────▶│◀── POST /v1/session/<id>/cmd ────┤
 │   (long-poll, server holds ~25 s,     │      {"cmd": "cw build"}         │
 │    204 No Content on timeout)         │                                  │
 │◀── 200  "cw build\n" ─────────────────┤                                  │
 │                                       │                                  │
 ├─ POST /v1/session/<id>/out ──────────▶│                                  │
 │   (streamed, repeated while running)  ├── GET /v1/session/<id>/out ─────▶│
 │                                       │                                  │
 ├─ POST /v1/session/<id>/exit  {"s":0} ▶│                                  │
```

- **Auth:** `Authorization: Bearer <token>` on every request, both directions.
- **TLS:** mandatory. macTLS verifies the relay certificate against the 121 baked-in Mozilla anchors. No plaintext fallback, ever.
- **Framing:** deliberately dumb. One command per line in, raw bytes out, an integer exit status at the end. JSON only where structure genuinely helps. A line protocol is debuggable with `curl`.
- **Yielding:** a 25-second long-poll must not freeze the UI. The existing `OTUseSyncIdleEvents` + `YieldToAnyThread` notifier already handles exactly this shape of blocking call, and macTLS's async engine (`ostls_async.c`) is built around it.
- **The relay** is a couple hundred lines of anything — Go, Python, a few nginx `proxy_pass` rules over a queue. It can live on `macsurf.org`, which already exists. On a LAN, it can be a process on the Linux box.

### 9.4 Transport B — LAN listener *(fallback)*

Passive bind is not *entirely* dead. `OTBind(NULL, NULL)` with `qlen = 1` **does** succeed, and Open Transport assigns an ephemeral port (49417, 49423, 49433 observed across runs). A listener is therefore possible on a trusted wired LAN, at the cost of a port that changes every launch — so the Mac must announce it (print it to the transcript, or register it with the relay).

Use this only when the relay is unavailable. It does not traverse NAT and it inverts the trust model.

### 9.5 Transport C — serial console *(bring-up)*

Modem/printer port, or a USB-serial adapter. No network stack at all.

This is the channel you want **when the network stack is the thing under test** — during M3 and M4, when `ostls_http` may be exactly what's broken. It is cheap to implement and disproportionately valuable for debugging. Build it early.

### 9.6 Security model

**This is remote code execution on the user's machine, by design.** Treat it accordingly.

- **Off by default.** Enabled by explicit user action, per launch. Never auto-start on boot.
- **Bearer token**, ≥128 bits, generated by macEntropy. Stored in a Preferences file, **never in the source tree** and never in a fix tar.
- **TLS only**, with real certificate verification against the baked-in anchors. Reject on any verification failure. Do not add a "skip verify" flag.
- **The relay is the trust boundary.** Assume it can be compromised. Scope every token to a single session; rotate on each attach.
- **Visible indicator** in the terminal whenever a remote session is attached, naming the peer, plus a local key chord that kills it immediately.
- **One session may hold the command lock at a time.** Concurrent writers to one address space with no memory protection is not a thing to be clever about.

### 9.7 Driving CodeWarrior — the `cw` builtin

macShell is a Mac application, so it can send Apple Events directly with `AESend`. **No MacPerl, no ToolServer, no external scripting layer.**

The event vocabulary is documented by Mozilla's `Moz/CodeWarriorLib.pm` (extracted from `Classilla9.3.4b.src.sit`; target application signature `'CWIE'`):

| `cw` subcommand | Apple Event | Effect |
|---|---|---|
| `cw import <xml> <mcp>` | `core/crel`, `kocl:type(PRJD)` | **build a `.mcp` from an XML export** |
| `cw export <mcp> <xml>` | `CWIE/EXPT` | dump a `.mcp` back to XML |
| `cw build [target]` | `CWIE/MAKE` | build a target |
| `cw clean` | `CWIE/RMOB` | remove object code |
| `cw errors <file>` | `MMPR/SvMs` | save the Errors & Warnings window to a text file |
| `cw open <file>` | `aevt/odoc` | open a project |
| `cw quit` | `aevt/quit` | quit the IDE |

`cw import` is what makes a pulled repository buildable with no hand-configuration: the tracked XML project export becomes a `.mcp` automatically after every `git pull`. `cw export` closes the loop — a contributor who adds a source file in the IDE regenerates the XML, and *that* is the diff in their pull request.

> *Unverified:* `CodeWarriorLib.pm` was written for CW Pro 4 in 1998 and is exercised by Classilla against CW 7.1. The `CWIE` suite has been stable across Pro versions, so these events very probably work on CW8 — but verify. **Test `CWIE/EXPT` first; it is read-only.**

### 9.8 The agent loop

With §9.3 and §9.7 in place, an agent on Linux runs this against the G3 with nobody in the room:

```sh
git pull                              # macgit — or fetch + untar, pre-M5
cw import MacSurf.xml MacSurf.mcp     # regenerate the project from tracked XML
cw build                              # CWIE/MAKE
cw errors errors.txt                  # MMPR/SvMs
cat errors.txt                        # -> streamed back over the session
open MacSurf                          # LaunchApplication
# ... exercise the browser ...
cat "MacSurf Debug.log"               # the crash-surviving forensic channel
```

Every one of those outputs streams back through `macsh_session` to the agent, which decides what to do next.

This is the loop the project has never had. It is what turns "fixes688–715 are hardware-UNVERIFIED, awaiting a G3 pass" into a job that runs while you sleep.

---

## 10. macPkg

Once §4 (tar + gzip + inflate), §5 (HTTPS fetch), and §8 (plugin ABI) exist, a package manager is a small amount of glue:

- **Package** = `.tar.gz` containing a `.shlb`, a manifest, and any resources.
- **Index** = a plain-text file at a known HTTPS URL: name, version, URL, size, SHA-1.
- **Signature** = the index is signed; BearSSL (already linked, EC and RSA verify available) checks it.
- **`pkg install <name>`** = fetch index → verify signature → resolve name → fetch package → verify SHA-1 → gunzip → untar into `Tools:` → done.
- **`pkg list` / `pkg update` / `pkg remove`** follow trivially.

There is no shared-library dependency hell on OS 9 the way there is with ELF — most classic Mac software is drag-install. A package manager here is a **verified catalog, downloader, and unpacker**, which is a far smaller problem than `apt` solves.

---

## 11. Milestones

Each milestone is independently useful and independently shippable. Risk is deliberately back-loaded: everything genuinely novel happens on Linux, where debugging is cheap, before it touches a Mac.

**M4b is the ordering keystone.** It depends only on M3 and M4, and once it lands, every milestone after it can be developed and tested by an agent driving the G3 remotely. Do not defer it.

### M0 — Skeleton and host harness
Repository, build files, `git_platform` abstraction (§3.3), Linux host implementation with libcurl + `fopen`, CI running the C89 syntax gate (`gcc -std=c89 -pedantic-errors`) and the Retro68 PPC gate.
**Acceptance:** `make check` passes on Linux; the platform struct compiles unchanged under both gates.

### M1 — Compression and archives *(Linux)*
`puff.c` integrated. Adler-32, CRC-32, stored-block zlib writer. gzip header skipper. ustar reader handling `'x'`/`'g'`/`'L'`/`'K'`.
**Acceptance:** unpack a real GitHub `codeload` tarball of this repository byte-identically against system `tar -xzf`. Round-trip a buffer through the stored-block writer and system `zlib` inflate.

### M2 — macGit core *(Linux)*
Ref discovery, shallow-clone POST, pkt-line, sideband demux, packfile parse, OFS/REF delta resolution, OID verification, loose-object explode, `.git` skeleton.
**Acceptance:** `macgit clone --depth 1 https://github.com/<user>/macsurf` produces a directory where stock `git fsck` reports no errors, `git status` reports a clean tree, and `git log -1` shows the right commit. Repeat against Codeberg. Repeat against a private repo with a PAT.

**This is the milestone that proves the project.** It happens entirely on Linux.

### M3 — macShell on OS 9
Carbon app skeleton (reuse MacSurf's `'carb'` resource, Appearance init, event loop). QuickDraw cell grid + ring-buffer scrollback + proc-384 scrollbar. Line editor with history. FSRef filesystem layer (§7). Builtins: `ls cd pwd cat mkdir rm echo help`.
**Acceptance:** launches on a G3, navigates the disk, prints a file, survives a Ctrl-C in a deliberate infinite loop. Smoke-tested in SheepShaver first, verified on hardware.

### M4 — Network commands on OS 9
Link `macTLS/os9/`. Port M1's compression to CW8. Commands: `fetch <url>`, `gunzip`, `untar`.
**Acceptance:** on a G3, `fetch` a `codeload.github.com` tarball of this repository over HTTPS, `gunzip`, `untar`, and open the result in CodeWarrior. **At this point MacSurf contributors have a source-update path that requires no Linux box.**

### M4b — Remote session and CodeWarrior control
The `macsh_session` abstraction (§9.2). Reverse-HTTPS long-poll client over macTLS, bearer auth, streamed output (§9.3). A relay service on the Linux side. The serial console (§9.5) — build it first, it debugs everything else. The `cw` builtin sending Apple Events to `'CWIE'` (§9.7).

**Acceptance, in three steps:**
1. From a Linux shell, run `ls` on the G3 and get its output back.
2. Unattended `cw build` returns the CodeWarrior error log to Linux.
3. An agent completes a full `pull → cw import → cw build → cw errors → open → cat "MacSurf Debug.log"` cycle with **no human at the Mac.**

**This is the milestone that changes how the project is developed.** Everything after it is built and verified remotely.

### M5 — macGit on OS 9
Bind M2's core to the Mac platform: `ostls_http` for transport, FSRef for the checkout, CR translation, type/creator, case-collision and NFD handling.
**Acceptance:** on a G3, `git clone --depth 1 https://github.com/<user>/macsurf`, then `cw import`, then `cw build`, and get a working browser — **with the whole cycle driven from Linux over the M4b session.** This is the goal stated at the outset.

### M6 — Incremental pull
`have` line negotiation, `multi_ack_detailed`, thin-pack resolution against the local object store, ref update, fast-forward merge of a linear history.
**Acceptance:** `git pull` on the Mac brings a stale checkout current, and the resulting `.git` is accepted by stock git.

### M7 — Plugin ABI and macPkg
`GetDiskFragment` + `FindSymbol` loader, `HostServices` struct, a first out-of-tree command shipped as a `.shlb`. Signed index, `pkg install/list/update/remove`.
**Acceptance:** `pkg install wget` on a clean machine yields a working `wget`.

### M8 — Push *(optional)*
`git-receive-pack`, stored-block pack writer, `report-status` parsing.
**Acceptance:** commit and push from a Mac; the commit appears on GitHub.

---

## 12. Repository preparation

Before the Mac clones this repository, prepare it:

- The pack is **82 MiB across 27,474 objects**, of which roughly 30 MB is `.sit` archives under `builds/`. Strip them from history (`git filter-repo` / BFG). A depth-1 clone skips history but still carries every blob at the tip.
- Make CodeWarrior access paths `{Project}`-relative with no user-specific components. The current `{Project}::patrick:macsurf-source Folder:` bakes a machine-specific folder name into the project; `{Project}::::::` reaches the repository root from `frontends/macos9/`.
- Add `.gitattributes` marking text files, so both macGit's CR translation and any host-side smudge filter agree on what is text.

---

## 13. Licensing

| Component | License | Use |
|---|---|---|
| `puff.c` | zlib | **Vendor directly.** Permissive, attribution in header. |
| zlib (fallback) | zlib | Vendor if needed. |
| BearSSL | MIT | Already vendored. |
| GUSI | zlib | Optional; permissive. |
| posix9 | Apache 2.0 | Compatible; reference, or upstream collaboration. |
| vt100-emulator | *verify* | Only if a VT core is wanted later. Check COPYRIGHT. |
| **MacRelix** | **AGPLv3** | **Design reference only. Do not copy code.** |
| MPW / ToolServer / SIOUX / ZTerm | proprietary | Reference only. |

---

## 14. Open questions to settle before M0

1. **Project name and repository home.** Inside `macsurf/`, or its own repository like `macTLS`? Recommendation: **its own repository.** macShell is not a browser feature, and macTLS has already demonstrated that a nested project with its own CW target works.
2. **Does macShell depend on macTLS, or vendor a copy?** Recommendation: depend on it, as MacSurf does — one TLS stack, one set of fixes.
3. **Is `git` a builtin or the first plugin?** Building it as a plugin from day one forces the ABI to be real. Building it in is simpler. Recommendation: **builtin through M6, then extract it to a plugin as M7's proof.**
4. **Where does the relay live?** `macsurf.org` already exists and already serves plain HTTP for Mac-side downloads. Recommendation: a small HTTPS endpoint there for the public case, plus a LAN-local relay binary for development. Decide before M4b.
5. **How many concurrent remote sessions?** Recommendation: **one command lock, many observers.** Multiple writers into a single address space with no memory protection is not worth the cleverness. Attach-as-observer is cheap and useful.
6. **Reach out to Josh Juran (MacRelix) and Scott (posix9)?** Both have solved adjacent problems. AGPL blocks code reuse from MacRelix but not conversation. Recommendation: yes, after M2 exists and there is something concrete to show.
7. **Retro68 as a second toolchain?** posix9 targets it; MacSurf does not. A Retro68 cross-build on Linux would let CI compile the Mac-side code without a Mac. Recommendation: use it as a **syntax gate only** (as MacSurf already does), not as a shipping toolchain.

---

## Appendix A — Verified byte layouts

Collected for implementation reference. Sources: `gitprotocol-common.adoc`, `gitprotocol-pack.adoc`, `gitprotocol-http.adoc`, `gitprotocol-capabilities.adoc`, `gitformat-pack.adoc`, `gitrepository-layout.adoc`; git source `patch-delta.c`, `packfile.c`, `object-file.c`; RFC 1950, RFC 1951, RFC 1952; POSIX.1-1988 ustar; Apple TN1150.

**Empty zlib stream (stored block):** `78 01 01 00 00 FF FF 00 00 00 01`

**Empty packfile:** `50 41 43 4B 00 00 00 02 00 00 00 00` + 20-byte SHA-1 trailer

**pkt-line magic values:** `0000` flush · `0001` delim (v2) · `0002` response-end (v2) · `0003` reserved · `0004` legal empty line

**Sideband codes:** `\x01` pack data · `\x02` progress · `\x03` fatal

**Pack object types:** `1` commit · `2` tree · `3` blob · `4` tag · `6` OFS_DELTA · `7` REF_DELTA

**Tree modes:** `100644` file · `100755` exec · `120000` symlink · `40000` tree · `160000` gitlink

**Delta copy opcode bitmask:** bit0–3 → offset bytes 1–4 (LE) · bit4–6 → size bytes 1–3 (LE) · `size == 0` means `0x10000`

**Pack `.idx` v2** *(not on the v1 path)*: magic `FF 74 4F 63` · version 2 · `fanout[256]` u32 BE · `N×20` sorted SHA · `N×4` CRC32 · `N×4` offsets (MSB set → index into large-offset table) · `M×8` large offsets · 20-byte pack checksum · 20-byte idx checksum

**Key error codes:** `fnfErr` = −43 (file not found) · `dupFNErr` = −48 (duplicate filename → case collision)

**Virtual key codes (US):** Return `0x24` · Enter `0x4C` · Tab `0x30` · Delete `0x33` · Fwd-Del `0x75` · Esc `0x35` · Left `0x7B` · Right `0x7C` · Down `0x7D` · Up `0x7E` · Home `0x73` · End `0x77` · PgUp `0x74` · PgDn `0x79`

---

## Appendix B — Source index

**Specifications**
- git protocol: [gitprotocol-pack](https://git-scm.com/docs/gitprotocol-pack), [gitprotocol-http](https://git-scm.com/docs/gitprotocol-http), [gitprotocol-common](https://git-scm.com/docs/gitprotocol-common), [gitprotocol-capabilities](https://git-scm.com/docs/gitprotocol-capabilities)
- git formats: [gitformat-pack](https://git-scm.com/docs/gitformat-pack), [gitrepository-layout](https://git-scm.com/docs/gitrepository-layout), [Pro Git ch. 10](https://git-scm.com/book/en/v2/Git-Internals-Packfiles)
- [RFC 1950 (zlib)](https://www.rfc-editor.org/rfc/rfc1950) · [RFC 1951 (DEFLATE)](https://www.rfc-editor.org/rfc/rfc1951) · [RFC 1952 (gzip)](https://www.rfc-editor.org/rfc/rfc1952)
- [GNU tar / ustar](https://www.gnu.org/software/tar/manual/html_node/Standard.html)
- [Apple TN1150 — HFS Plus Volume Format](https://developer.apple.com/library/archive/technotes/tn/tn1150.html)

**Code**
- [puff.c](https://github.com/madler/zlib/blob/master/contrib/puff/puff.c) · [puff.h](https://github.com/madler/zlib/blob/master/contrib/puff/puff.h) · [pufftest.c](https://github.com/madler/zlib/blob/master/contrib/puff/pufftest.c)
- [MacRelix / metamage_1](https://github.com/jjuran/metamage_1) *(AGPL — reference only)*
- [posix9](https://github.com/Scottcjn/posix9/) *(Apache 2.0)*
- [GUSI](https://sourceforge.net/projects/gusi/) *(zlib)*
- [vt100-emulator](https://github.com/JulienPalard/vt100-emulator) *(C89; verify license)*
- [cvsgui / MacCVS Pro](https://cvsgui.sourceforge.net/)

**Toolbox**
- [Carbon `CarbonCore/Files.h`](https://github.com/phracker/MacOSX-SDKs/blob/master/MacOSX10.6.sdk/System/Library/Frameworks/CoreServices.framework/Versions/A/Frameworks/CarbonCore.framework/Versions/A/Headers/Files.h) — availability annotations
- [Mac OS Runtime Architectures (PDF)](https://developer.apple.com/library/archive/documentation/mac/pdf/MacOS_RT_Architectures.pdf) — CFM, transition vectors
- [MacTech, "Calling CFM Code"](http://preserve.mactech.com/articles/mactech/Vol.13/13.08/CallingCFMCode/index.html)
- [Thread Manager (MacTech)](http://preserve.mactech.com/articles/mactech/Vol.10/10.11/ThreadManager/index.html)
- [Inside Macintosh: Text — TextEdit](https://developer.apple.com/library/archive/documentation/mac/pdf/Text/TextEdit.pdf) — the 32K `teLength` ceiling

**Prior art narrative**
- [MacRelix origins](https://www.metamage.com/text/relix/origins.html) · [macrelix.org](https://www.macrelix.org/) · [HN discussion](https://news.ycombinator.com/item?id=40338443)
- [MacTech, "Porting Command Line Interface Programs to the Macintosh"](http://preserve.mactech.com/articles/mactech/Vol.16/16.07/CommandLinePorting/index.html) — the MPW in-process tool model

**Local, in this working tree**
- `Moz/CodeWarriorLib.pm` — the `'CWIE'` Apple Event vocabulary of §9.7. Extract from `Classilla9.3.4b.src.sit` at `mozsrc/mozilla/build/mac/build_scripts/Moz/CodeWarriorLib.pm`.
- `classilla/wiki/HowToBuild.wiki`, `HowToSubmit.wiki` — how a solo maintainer shipped an OS 9 browser without a version-control system, and why.
- `macTLS/os9/ostls_http.h`, `ostls_async.h` — the HTTPS client this project links.
- macTLS commits `fixes16..fixes34` — the 14-round negative result establishing §9.1. A definitive reference for what does *not* work.

---

## Appendix C — Marked uncertainties

Everything else in this document is verified against a primary source or inspected in this repository. These are not:

1. **MLTE stability under heavy use on real G3/G4 hardware.** It is documented as shipping in CarbonLib for OS 9. Its robustness in a terminal workload is unverified. §6.1's cell-grid recommendation sidesteps the question entirely.
2. **`FSRefMakePath`'s exact volume representation on OS 9.** Confirmed slash-separated and confirmed to interoperate poorly with MSL's colon paths. The precise root format was not established. Immaterial — §7.2 routes around it.
3. **Virtual key codes** are the standard US-layout ADB codes and match MacSurf's existing handling, but are layout-dependent in principle. Use `charCode` for text; reserve raw `keyCode` for the navigation set.
4. **`vt100-emulator`'s license.** Check its COPYRIGHT file before vendoring. Not on the v1 path.
5. **The stored-block encoder's exact bytes** should be diffed against real `zlib deflate(level=0)` output once, before shipping the object writer. They match zlib's `Z_NO_COMPRESSION` mode by specification.
6. **The `CWIE` Apple Event suite on CodeWarrior 8.** `CodeWarriorLib.pm` targets CW Pro 4 (1998); Classilla drives CW 7.1 with it. The suite is stable across Pro versions, but this is unverified on CW8. **Test `CWIE/EXPT` first — it is read-only.** Everything in §9.7 rests on it.
7. **macTLS behaviour under a 25-second long-poll.** `ostls_async.c` is built for page fetches, not for a deliberately-held connection. Audit its read timeout and keep-alive handling before committing to §9.3's poll interval; a shorter hold with more round-trips is an acceptable fallback.
8. **Serial port access under Carbon on OS 9.** The Communications Toolbox and the classic Serial Driver both exist, but which is cleanest from a Carbon CFM app was not researched. §9.5 is a recommendation, not a verified path.
