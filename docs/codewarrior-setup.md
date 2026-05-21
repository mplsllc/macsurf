# CodeWarrior 8 Setup Guide for MacSurf

Step-by-step guide to building MacSurf on a real Power Mac running Mac OS 9.

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

---

## 2. Opening the Project File

1. Copy the entire `macsurf/browser/netsurf/` directory tree to the Mac (see Section 6 for transfer method)
2. Navigate to `netsurf:frontends:macos9:` in the Finder
3. Double-click `MacSurf.mcp`, CodeWarrior will open it and display the project window
4. If CodeWarrior prompts "Cannot find file..." for any source file, the directory structure was not preserved during transfer. Verify that the `utils`, `content`, and `desktop` folders exist three levels up from the `macos9` folder
5. The project window should show six groups in the left pane:
   - NetSurf Core Utils (10 files)
   - NetSurf Content (7 files)
   - NetSurf Desktop (5 files)
   - MacSurf Frontend (11 files)
   - POSIX Shims (5 files)
   - Libraries (3 items)

---

## 3. Verifying Target Settings

1. Go to **Edit > MacSurf Settings...** (or press Cmd-J)
2. In the settings dialog, verify:

**Target Settings panel:**
- Linker: MacOS PPC Linker
- Output Directory: `:` (current directory)

**C/C++ Language panel:**
- Source model: C (not C++)
- C99 Extensions: enabled

**C/C++ Preprocessor panel:**
- Prefix text should contain:
  ```
  #define __MACOS9__               1
  #define WITHOUT_DUKTAPE          1
  #define NO_IPV6                  1
  #define TARGET_API_MAC_CARBON    1
  ```

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

### What will succeed

The POSIX shim files and frontend files should compile without errors, these have been syntax-checked on Linux with equivalent flags. Files that compiled clean in our Linux syntax-check rounds:

- All 5 `utils/` files (Round 2, zero errors, zero warnings)
- All 3 `content/` files (Round 2, zero errors)
- All 5 `desktop/` files (Round 3, zero errors)
- All 11 frontend files (Round 4, zero errors)

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

**MSL library path issues.** CodeWarrior 8 ships MSL libraries in slightly different directory structures depending on the installer version. If "MSL C.Carbon.Lib" is not found:
1. Use Sherlock (Cmd-F in Finder) to search for "MSL C.Carbon.Lib" on the boot volume
2. Note the actual path
3. In the project, remove the broken library reference and re-add from the correct location

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
