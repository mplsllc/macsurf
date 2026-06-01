# CodeWarrior 8 Setup Guide for MacSurf

Step-by-step guide to building MacSurf on a real Power Mac running Mac OS 9.

> **Quickest path:** the complete, ready-to-open project ships as a StuffIt pack in
> [`builds/`](../builds/) (see [builds/README.md](../builds/README.md)). It bundles
> the full source tree, the CodeWarrior project with target settings already
> configured, and the MSL libraries it links against, so you can expand it on the
> Mac and go straight to Build. The sections below cover installing CodeWarrior,
> what the project contains, and assembling the source by hand if you prefer.

---

## 1. Installing CodeWarrior 8

**Requirements:** Power Mac G3 or G4, Mac OS 9.1 or later, 128MB RAM recommended, CD-ROM drive.

1. Insert the CodeWarrior Pro 8 CD
2. Double-click the installer, it will walk through license acceptance and destination selection
3. Install to the default location (`Macintosh HD:Metrowerks CodeWarrior:`)
4. When prompted for components, ensure these are checked:
   - **MacOS PowerPC C/C++ Compiler**
   - **MacOS PowerPC Linker**
   - **MSL C Libraries** (Metrowerks Standard Library)
   - **Universal Headers** (Mac OS Universal Interfaces)
5. Skip the Java, Windows, and Palm OS tools, they are not needed
6. After installation completes, open CodeWarrior IDE once to confirm it launches. It will create preferences in the System Folder
7. Verify the Universal Headers are present at:
   ```
   Metrowerks CodeWarrior:MacOS Support:Headers:Universal Headers:
   ```
   You should see folders like `CIncludes`, `PInterfaces`, etc. If missing, run the installer again and check the Universal Headers checkbox

**No CD?** CodeWarrior Pro 8 is on Macintosh Garden. The project targets the 8.3 update, which is cumulative: install base **Pro 8.0**, then apply the **8.1**, **8.2**, and **8.3** updaters in that order (each refuses to run until the prior one is installed). You also need **CarbonLib 1.6 or later** in the System Folder.

---

## 2. Opening the Project File

The project is distributed in [`builds/`](../builds/) as a StuffIt pack containing the full source tree, the CodeWarrior project file (with target settings already set), and the bundled MSL libraries.

1. Expand the pack on the Mac with StuffIt Expander.
2. Open the included CodeWarrior project (double-click it, or File > Open).
3. The project spans NetSurf core plus the vendored libraries (libcss, libdom, libhubbub, libparserutils, libwapcaplet, Duktape) and the macTLS TLS code, plus the macOS 9 frontend: several hundred source files, grouped by subsystem in the left pane.
4. If CodeWarrior shows files in red ("Cannot find file..."), the source tree was not expanded where the project's access paths expect it. Re-expand the pack so the `macsurf-source Folder` tree sits at the path the project references.

If you are assembling the project by hand from a source checkout instead, the rest of this guide (target settings, access paths, transfer) walks through it, but the pack is the simplest path and the authoritative reference for the current file list and settings.

---

## 3. Verifying Target Settings

If you opened the project from the `builds/` pack, these settings are already configured; this section is a reference, useful mainly for hand-assembly. The access-path list below is from an earlier, smaller layout; the real project carries many more paths, so treat the pack's project file as authoritative.

1. Go to **Edit > MacSurf Settings...** (or press Cmd-J)
2. In the settings dialog, verify:

**Target Settings panel:**
- Linker: MacOS PPC Linker
- Output Directory: `:` (current directory)

**C/C++ Language panel:**
- Source model: C (not C++)
- C99 Extensions: enabled

**Prefix file (C/C++ Language panel):**
- The project sets a prefix *file*, `macsurf_prefix.h`, in the **Prefix File** field rather than inline preprocessor text. That header handles the platform setup (`__MACOS9__`, `NO_IPV6`, `TARGET_API_MAC_CARBON`, defining away `inline` for C89, and so on).
- Note `WITHOUT_DUKTAPE` is **not** defined: Duktape is compiled into the project (`duktape.c` is in the file list), so the engine is linked in. (Older setups used inline prefix text with `WITHOUT_DUKTAPE` defined to exclude it.)

**Access Paths panel:**
- User paths should list (in order):
  1. `{Project}/include`
  2. `{Project}/../../../include`
  3. `{Project}/../../../`
  4. `{Project}/shims`
- System paths should list:
  1. `{Compiler}/MacOS Support/Headers/Universal Headers`
  2. `{Compiler}/MacOS Support/Libraries/Runtime/Shared Support`

**PPC Processor panel:**
- Processor: 750 (G3), works on G3 and G4
- Struct Alignment: PowerPC

3. Click OK to save

---

## 4. First Build Attempt

Press Cmd-M (Project > Make) to start the build. Here is what to expect:

> The breakdown in this section dates from the early layered bring-up, when only a few dozen files were in the project. The current project builds the whole tree (several hundred files); from the `builds/` pack it compiles and links with the settings already configured. The troubleshooting notes below are mainly useful when assembling the project by hand.

### What will succeed

The POSIX shim files, the macOS 9 frontend, and the vendored libraries (libcss, libdom, libhubbub, libparserutils, libwapcaplet, Duktape) all compile under CW8 with the project's prefix file and settings in place.

### What will likely fail

**Missing library headers.** The project references headers from NetSurf dependency libraries (libwapcaplet, libcss, libdom, libparserutils, libhubbub). During Linux syntax checking, we copied these headers into `netsurf/include/`. Verify they are present on the Mac at:

```
netsurf:include:libwapcaplet:
netsurf:include:libcss:
netsurf:include:dom:
netsurf:include:parserutils:
netsurf:include:hubbub:
netsurf:include:nsutils:
netsurf:include:curl:
```

If any are missing, copy them from the corresponding `browser/lib*/include/` directories.

**CarbonLib linking.** If the linker reports "CarbonLib not found", you need to add it manually:
1. Go to Project > Add Files...
2. Navigate to `System Folder:Extensions:`
3. Select `CarbonLib`
4. Add it to the Libraries group

**MSL libraries.** The project links `MSL_C_Carbon.Lib` and `MSL_Runtime_PPC_D.Lib`; both ship inside the `builds/` pack (they are specific builds, not necessarily the ones in a stock CodeWarrior install). If the linker can't find them, make sure they sit on a library access path, e.g. alongside the other MSL libraries in `{Compiler}:MacOS Support:Libraries:Runtime:Libs:`. (Older notes here referenced `MSL C.Carbon.Lib`, the CodeWarrior 7-era name.)

### Expected error count

On a clean first build with all headers in place: **zero compile errors for the 24 source files listed in the project.** The first real errors will come at link time, unresolved symbols from library code (libcss, libdom, etc.) that is not yet compiled as part of the project. This is expected. The next step is to add the dependency library source files to the project or build them as separate library projects.

---

## 5. Reading CodeWarrior Error Output

### The Errors & Warnings window

After a build, CodeWarrior shows an **Errors & Warnings** window. Each entry shows:

```
File "utils.c", line 477: warning: implicit declaration of 'strdup'
```

- **Double-click** any error to jump to the exact line in the source editor
- Errors are red (stop icon), the file did not compile
- Warnings are yellow (caution icon), compiled but suspicious

### Common error patterns and what they mean

| Error Message | Likely Cause | Fix |
|---|---|---|
| `file not found: "libwapcaplet/libwapcaplet.h"` | Missing dependency library headers | Copy headers into `netsurf/include/` (see Section 4) |
| `undefined identifier 'PATH_MAX'` | `config.h` `__MACOS9__` block not active | Verify `__MACOS9__` is in the preprocessor prefix text (Section 3) |
| `implicit declaration of 'strdup'` | POSIX function not declared | Should be in `utils/config.h` under `__MACOS9__` guard, check that the config.h edits from `core-compile-attempt.md` Round 2 are present |
| `undefined identifier 'WindowRef'` | Mac Toolbox header not included | Ensure `#ifdef __MACOS9__` path includes `<MacWindows.h>` in the affected file |
| `cannot convert 'void' to 'int'` | `ns_close_socket` macro issue | Verify `utils/inet.h` has `((void)(s), 0)` not `((void)(s))` for the `__MACOS9__` case (fix from Round 3) |
| `link failed: unresolved 'lwc_intern_string'` | libwapcaplet object code not in project | Build libwapcaplet source or add its .c files to the project |
| `link failed: unresolved 'css_stylesheet_create'` | libcss object code not in project | Build libcss source or add its .c files to the project |

### Mapping errors to the task list

The project is structured in compilation layers matching the research docs:

1. **POSIX Shims**, if these fail, fix the shim code first. Refer to `posix-portability.md` Section 2 for the implementation plan for each shim
2. **NetSurf Core Utils**, errors here usually mean a `config.h` macro is wrong or a shim header is missing. Refer to `core-compile-attempt.md` for the exact fixes applied in each round
3. **NetSurf Content**, depends on Utils compiling clean. Content-layer errors are usually missing library headers (libwapcaplet, libcss, libdom)
4. **NetSurf Desktop**, depends on Content and Utils. Desktop errors are usually missing library headers or POSIX function declarations
5. **MacSurf Frontend**, depends on all of the above. Frontend errors are usually missing Toolbox headers or Mac-specific API issues

Work bottom-up: fix shim errors first, then utils, then content, then desktop, then frontend.

---

## 6. Transferring Source Files from Linux to Mac

### FAT32 thumb drive method

This is the simplest and most reliable transfer method. Mac OS 9 reads FAT32 (called "DOS" in Mac OS 9) volumes natively via File Exchange.

**On Linux:**

1. Format a USB thumb drive as FAT32 (most are already FAT32):
   ```
   # Only if reformatting is needed — this erases the drive
   sudo mkfs.vfat -F 32 /dev/sdX1
   ```

2. Mount the drive and copy the source tree:
   ```
   mount /dev/sdX1 /mnt/usb
   cp -r browser/netsurf/ /mnt/usb/netsurf/
   sync
   umount /mnt/usb
   ```

3. **Line endings:** Mac OS 9 expects CR (`\r`) line endings, not LF (`\n`). CodeWarrior 8 handles both, so this is usually not an issue for compilation. But if you edit files in SimpleText or BBEdit Lite on the Mac side, convert first:
   ```
   find /mnt/usb/netsurf -name "*.c" -o -name "*.h" | xargs sed -i 's/$/\r/'
   ```

4. **Filename length:** FAT32 supports long filenames (up to 255 chars). All MacSurf filenames are well within this limit. No issues expected.

**On the Mac:**

1. Insert the thumb drive. It should appear on the desktop as a DOS volume (generic disk icon)
2. If it does not mount, open **Control Panels > File Exchange** and ensure "Mount DOS disks" is checked
3. Open the drive and drag the `netsurf` folder to your hard drive, placing it wherever you want the project to live
4. The Finder will copy all files, preserving the directory structure. FAT32 does not preserve Mac resource forks, but all MacSurf source files are plain text data-fork files, so nothing is lost

### After transfer, verify structure

Open the `netsurf` folder on the Mac and confirm this structure exists:

```
netsurf:
  frontends:
    macos9:
      MacSurf.mcp        ← the project file
      main.c
      window.c
      ...
      shims:
        mac_iconv.c
        mac_file_io.c
        ...
  utils:
    utils.c
    config.h
    ...
  content:
    llcache.c
    fetch.c
    ...
  desktop:
    browser.c
    ...
  include:
    libwapcaplet:
    libcss:
    dom:
    ...
```

The `MacSurf.mcp` project file uses relative paths (`../../../utils/utils.c` etc.), so the directory hierarchy must be intact. If you placed files in a different structure, update the access paths in the project settings (Section 3).

### Alternative: AppleTalk/FTP

If both machines are on the same network, Mac OS 9's built-in FTP Access (in the Internet control panel) or a third-party FTP client like Fetch can transfer files directly. But the thumb drive method avoids network configuration entirely and works with any Mac that has a USB port (all G3s and G4s do).
