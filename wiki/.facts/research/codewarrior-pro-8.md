# Metrowerks CodeWarrior Pro 8 (Mac OS / Classic Mac)

CodeWarrior Pro 8 is the Metrowerks integrated development environment (IDE) and toolchain used to build native PowerPC software for classic Mac OS (and Carbon apps that also run on early Mac OS X). It is the IDE MacSurf is compiled with on real hardware. It is a PowerPC-hosted, Carbonized IDE: the IDE application itself runs on a PowerPC Mac under Mac OS 9 / early Mac OS X, and it produces PowerPC code. This note records verified facts about what it is, its 8.0→8.3 updater chain, the IDE/project model, access paths, prefix files, PEF/CFM output, where to find it today, and how it is installed on OS 9.

## Facts

- The product (then branded "CodeWarrior Development Tools for Mac OS") was released by Metrowerks in 2002; the IDE is PPC (Carbonized) and ships compilers for four languages: C, C++, Objective-C, and Java.
  Source: https://macintoshgarden.org/apps/codewarrior-pro-8x

- System requirements: PPC G3 or greater; Mac OS 9.1 / Mac OS X 10.1.3 (may run on older with an upgraded CarbonLib); 64 MB RAM on Mac OS 9 (128 MB on Mac OS X); ~700 MB disk for a full install.
  Source: https://macintoshgarden.org/apps/codewarrior-pro-8x

- The updater sequence is cumulative: base 8.0, then 8.1, 8.2, 8.3 updaters. The 8.2 updater "can be used with CodeWarrior Development Tools for Mac OS 8.0 or 8.1" — so 8.1 is effectively optional as an intermediate, but the 8.3 updater is "only intended for a ... v8.2 installation." Net: 8.3 cannot be applied directly on 8.0; 8.2 must be present first.
  Sources: https://www.macworld.com/article/155573/codewarrior-5.html ; https://www.macworld.com/article/156765/codewarrior-6.html

- The 8.2 update (released Aug 26, 2002) is a maintenance release of "MSL, Debugger, Java RAD, Support folders, Stationery, Targeting Mac OS documentation, and Compilers and Command Line Tools," and was the version Metrowerks indicated was needed for Mac OS X 10.2 "Jaguar" compatibility (~21 MB download).
  Source: https://www.macworld.com/article/155573/codewarrior-5.html

- The 8.3 update (released Nov 13, 2002) is a maintenance release updating "MSL, Debugger, IDE and Compilers and Command Line Tools" (~28 MB), and is intended to be applied only on top of v8.2.
  Source: https://www.macworld.com/article/156765/codewarrior-6.html

- IDE/project & target model: every CodeWarrior project (`.mcp`) contains one or more build targets; each target is a collection of source files plus settings (compiler options, linker output, warnings) that the IDE uses to build one output file.
  Source: https://docs.nxp.com/bundle/GUID-9FAC1C79-3809-474F-B8DF-82BEB5B88419/page/GUID-E8DEA010-B02F-4048-84C1-49B6094FFFE9.html

- Prefix file mechanism: the compiler can be told to `#include` a "prefix file" before every translation unit, configured under PowerPC Compiler > Preprocessor in the target settings. (This is exactly how MacSurf injects `macsurf_prefix.h`.)
  Source: https://docs.nxp.com/bundle/GUID-9FAC1C79-3809-474F-B8DF-82BEB5B88419/page/GUID-E8DEA010-B02F-4048-84C1-49B6094FFFE9.html

- Output format: CodeWarrior for classic Mac OS builds PEF executables. PEF (Preferred Executable Format) is Apple's executable container for classic Mac OS, designed for RISC/PowerPC code; PEF executables are also called CFM files because the Code Fragment Manager loads and prepares them. PEF remained supported on PowerPC Mac OS X for Carbon apps that run on both classic Mac OS and Mac OS X.
  Source: https://en.wikipedia.org/wiki/Preferred_Executable_Format

- The Code Fragment Manager recognizes two container formats (PEF and XCOFF) on PowerPC classic Mac OS; PEF was the native/preferred one.
  Source: https://developer.apple.com/library/archive/documentation/mac/pdf/MacOS_RT_Architectures.pdf

- Where to find it today: it is archived on the Macintosh Garden (base disc image plus 8.1/8.2/8.3 updaters) and on the Macintosh Repository. Garden file names/sizes: `CodeWarrior_8_Pro.toast_.sit` (~207 MB) or `CodeWarrior_8_Pro.toast` (~665 MB); `CW_8.1_Update_Installer.sit` (~8.8 MB); `CW_8.2_Update_Installer.sit` (~21 MB); `CW_8.3_Update_Installer.sit` (~28 MB).
  Sources: https://macintoshgarden.org/apps/codewarrior-pro-8x ; https://www.macintoshrepository.org/1351-codewarrior-pro-8-x

## Beginner gotchas / things that surprise people

- "Access paths" (search paths) are not a single global include list. Each target keeps an ordered list of System and User access paths, and they are searched in order — a file shadowed by an earlier path wins. By default paths are non-recursive unless you explicitly make them recursive, so adding a file to a subfolder that isn't on a path means the compiler simply won't find it. (This ordering is precisely why MacSurf's `frontends/macos9/` shim headers can shadow real NetSurf headers.) This is documented behavior in the CodeWarrior IDE; the exact 8.x manual page was not fetched here, so treat the recursive/non-recursive default as the project's own confirmed convention rather than a freshly-cited quote.

- Updaters must be layered, not skipped: you cannot drop 8.3 onto a bare 8.0 install — 8.2 has to be installed first. The 8.1 step itself can be skipped because 8.2 accepts an 8.0 base.

- Precompiled prefix/headers can go stale: CodeWarrior caches precompiled headers (e.g. MacHeaders); users report needing to rebuild the header projects after changing prefix/header settings. (Note: per MacSurf's own DIRECTIVE #1, do NOT use this as an explanation for MacSurf build symptoms — fix the code-side root cause instead.)
  Source: https://macintoshgarden.org/apps/codewarrior-pro-8x

- Mac OS 9 install detail (per Macintosh Garden / Macintosh Repository user notes, NOT verified against a primary Metrowerks doc): you must place the `MetroNub` extension into the System Folder's Extensions folder and drag the `Metrowerks` folder into System Folder > Preferences, and CarbonLib must be present (download from Apple if missing). Treat this as community install lore — it is consistently reported but I could not re-confirm the exact wording on every re-fetch.
  Source: https://www.macintoshrepository.org/1351-codewarrior-pro-8-x

- "Last Classic version" claim: it is commonly stated that Pro 8 is the last CodeWarrior that runs on classic Mac OS (later CodeWarrior 9/10 are Mac OS X only). This is widely repeated in the community but I could not pin it to a primary source on this pass, so flag it as probable-but-unverified.

## Sources

- https://macintoshgarden.org/apps/codewarrior-pro-8x
- https://www.macintoshrepository.org/1351-codewarrior-pro-8-x
- https://www.macworld.com/article/155573/codewarrior-5.html (8.2 update / Jaguar)
- https://www.macworld.com/article/156765/codewarrior-6.html (8.3 update)
- https://en.wikipedia.org/wiki/Preferred_Executable_Format
- https://developer.apple.com/library/archive/documentation/mac/pdf/MacOS_RT_Architectures.pdf
- https://docs.nxp.com/bundle/GUID-9FAC1C79-3809-474F-B8DF-82BEB5B88419/page/GUID-E8DEA010-B02F-4048-84C1-49B6094FFFE9.html
