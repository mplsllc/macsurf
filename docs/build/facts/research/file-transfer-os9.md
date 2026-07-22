# Moving Files To and From Classic Mac OS 9 (Preserving Mac Metadata)

Classic Mac OS files are not flat byte streams. A single file can carry two
"forks" plus Finder metadata (type/creator codes, flags), and most non-Apple
systems and protocols store only one of them. Every transfer method below
exists to solve that one problem: getting a Mac file onto foreign storage (or a
network wire) and back without losing the resource fork or Finder info. Facts
below were verified by fetching the cited sources.

## The fork problem

- A classic Mac file has two forks: a **data fork** (unstructured data, treated
  like a normal PC file) and a **resource fork** (structured data — icons,
  window/menu definitions, and on classic Mac OS the application's executable
  machine code itself). https://en.wikipedia.org/wiki/Resource_fork
- Resource forks are natively supported only on Apple filesystems: **MFS, HFS,
  HFS Plus, and APFS** (APFS is macOS-only). They are *not* supported on UFS,
  FAT/FAT32, or plain SMB/NFS without a workaround — which is exactly why files
  copied to non-Apple storage lose data. https://en.wikipedia.org/wiki/Resource_fork

## Encoding formats (one file, both forks)

- **BinHex 4.0** (`.hqx`) encodes the resource fork, data fork, *and* Finder
  metadata into a single **7-bit ASCII text** stream — three input bytes map to
  four 6-bit values, similar to Base64 but with a different alphabet (the first
  64 printable ASCII characters, including space). It run-length-encodes the
  data and generates separate CRCs for the data fork, resource fork, and header.
  Being 7-bit-safe, it survived email and ASCII-only transfer paths.
  https://en.wikipedia.org/wiki/BinHex
- **MacBinary** prepends a single 128-byte header carrying Finder metadata, then
  the data fork and resource fork, in one `.bin` file. **BinHex 5.0** used
  MacBinary to combine the forks but saw little adoption. (BinHex 4.0 stayed the
  popular form.) https://en.wikipedia.org/wiki/BinHex
- **StuffIt** (`.sit`) was the dominant Mac compressor from 1987 (created by
  Raymond Lau; Aladdin Systems formed 1988) until Mac OS X. `.sit` archives
  **save the resource forks** of the files inside them, which is what let Mac
  files live on non-Mac systems. `.sea` is a self-extracting executable variant;
  **StuffIt Expander** is the free decompressor. https://en.wikipedia.org/wiki/StuffIt

## AppleSingle / AppleDouble (the "._" files)

- **AppleSingle** packs both forks plus Finder info into one file.
  **AppleDouble** splits them: the data fork keeps its original name and format
  (so Unix tools can edit it), and a companion header file holds the resource
  fork and Finder info. https://en.wikipedia.org/wiki/AppleSingle_and_AppleDouble_formats
- The AppleDouble companion is named by **prepending `._`** to the original
  filename (e.g. `foo` → `._foo`); the leading dot hides it from many tools. On
  filesystems without native fork support (the page names NFS and WebDAV) macOS
  uses this scheme; Mac OS X zip/`ditto` put the same metadata under a
  `__MACOSX` directory. https://en.wikipedia.org/wiki/AppleSingle_and_AppleDouble_formats
- Note: the Wikipedia page does *not* itself document FAT32 as a trigger, nor
  the `XATTR_RESOURCEFORK_NAME` / `XATTR_FINDERINFO_NAME` attribute names — those
  appeared only in search snippets, so treat them as unverified here.

## Network file sharing (netatalk / AFP)

- **netatalk** is an open-source AFP (Apple Filing Protocol) file server for
  Unix-like hosts (Linux/BSD/Solaris). A classic Mac connects via **Chooser →
  AppleShare → "Server IP Address…"**, entering the server's IP. netatalk
  preserves Mac metadata (resource forks) using filesystem extended attributes
  or AppleDouble files. https://netatalk.io/ and https://netatalk.io/docs/Connect-to-AFP-Server
- **Mac OS 8.1 and later support AFP over TCP out of the box**; older systems
  need **AppleShare Client 3.7.0+** (and Open Transport 1.3 is recommended).
  https://netatalk.io/docs/Connect-to-AFP-Server
- The **netatalk 3.x branch dropped classic AppleTalk file sharing over
  ethernet**, so to share with a classic Mac over AppleTalk you need the **2.x
  branch** (4.x re-adds AppleTalk). The reliable path for OS 8.1/9, regardless,
  is AFP over **TCP/IP** by IP address.
  https://marmanold.com/retro/linux-fileshare-for-classic-macintoshes/

## Disk images and HFS volumes on Linux

- A disk image holds a whole HFS/HFS+ volume, so it preserves forks intact.
  Classic OS 9 mounts images with Apple's **Disk Copy** (NDIF `.img`/`.smi`
  self-mounting images were the OS 9-era format). On Linux, `hfsprogs`/`hfsutils`
  and FUSE drivers can read/write HFS and HFS+, and `genisoimage` can build HFS
  images. Transferring archives *inside* a disk image avoids fork loss during the
  hop. https://www.gryphel.com/c/image/ and https://www.macintoshrepository.org/articles/75-how-to-mount-a-disk-image-under-mac-os-7-8-or-9

## Beginner gotchas / things that surprise people

- **Copying a Mac app to a USB stick (FAT) "destroys" it.** The resource fork —
  which on classic Mac OS *is* the executable code — is dropped, leaving a dead
  data fork. Use `.sit`/`.hqx`/disk image, or an AFP/netatalk share.
- **The `._filename` files aren't junk you created.** They are AppleDouble
  metadata sidecars; deleting them strips the resource fork from the real file.
- **netatalk version matters.** Modern (3.x) builds won't do classic AppleTalk;
  pick 2.x (or 4.x) and prefer AFP-over-TCP for OS 8.1/9.
- **`.sea` is a program, not an archive format.** It only self-extracts on a
  Mac; it won't help on Linux without StuffIt tooling.
- **BinHex is text, not compression.** `.hqx` is bigger than the original; its
  job is 7-bit safety, not size. `.sit` is the compressor.

## Sources

- https://en.wikipedia.org/wiki/Resource_fork
- https://en.wikipedia.org/wiki/BinHex
- https://en.wikipedia.org/wiki/StuffIt
- https://en.wikipedia.org/wiki/AppleSingle_and_AppleDouble_formats
- https://netatalk.io/
- https://netatalk.io/docs/Connect-to-AFP-Server
- https://marmanold.com/retro/linux-fileshare-for-classic-macintoshes/
- https://www.gryphel.com/c/image/
- https://www.macintoshrepository.org/articles/75-how-to-mount-a-disk-image-under-mac-os-7-8-or-9
