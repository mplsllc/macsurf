#!/usr/bin/env python3
"""pef_carbon_imports.py -- list the symbols a Classic Mac OS PEF imports from CarbonLib.

PEF (Preferred Executable Format) is the CFM container used by PowerPC Mac OS.
otool/objdump don't understand it, so we parse the structure directly. All
multi-byte fields are big-endian (PPC).

Usage:  python3 pef_carbon_imports.py <binary> [--library CarbonLib] [--all]

By default the binary's DATA FORK is parsed. On Linux the fork you care about is
usually already a plain file; if you have an AppleDouble/MacBinary wrapper you
must extract the data fork first.
"""

import struct
import sys
import argparse

MAGIC_TAG1 = b"Joy!"
MAGIC_TAG2 = b"peff"

CONTAINER_HEADER_SIZE = 40
SECTION_HEADER_SIZE = 28
LOADER_INFO_HEADER_SIZE = 56
IMPORTED_LIBRARY_SIZE = 24

SECTION_KIND_LOADER = 4

# Symbol class is the top byte of each imported-symbol u32.
SYMBOL_CLASSES = {
    0x00: "code",
    0x01: "data",
    0x02: "tvect",   # transition vector (the normal case for a function)
    0x03: "toc",
    0x04: "glue",
}


def be16(buf, off):
    return struct.unpack_from(">H", buf, off)[0]


def be32(buf, off):
    return struct.unpack_from(">I", buf, off)[0]


def read_cstring(buf, off):
    end = buf.find(b"\x00", off)
    if end < 0:
        end = len(buf)
    return buf[off:end].decode("mac-roman", errors="replace")


def parse_pef(data):
    if len(data) < CONTAINER_HEADER_SIZE:
        raise ValueError("file too small to be a PEF container")
    if data[0:4] != MAGIC_TAG1 or data[4:8] != MAGIC_TAG2:
        raise ValueError("bad magic: expected 'Joy!peff', got %r" % data[0:8])

    architecture = data[8:12]
    if architecture != b"pwpc":
        # Not fatal, but worth flagging -- 'm68k' PEFs exist too.
        sys.stderr.write("warning: architecture is %r (expected 'pwpc')\n" % architecture)

    # NOTE: sectionCount is at offset 32 in the real PEF spec (offset 12 is
    # formatVersion). See Apple's "Mac OS Runtime Architectures", PEF chapter.
    section_count = be16(data, 32)

    # --- locate the Loader section among the section headers ---
    loader_hdr_off = CONTAINER_HEADER_SIZE
    loader = None
    for i in range(section_count):
        off = CONTAINER_HEADER_SIZE + i * SECTION_HEADER_SIZE
        if off + SECTION_HEADER_SIZE > len(data):
            raise ValueError("section header %d runs past end of file" % i)
        container_offset = be32(data, off + 20)
        section_kind = data[off + 24]
        if section_kind == SECTION_KIND_LOADER:
            loader = container_offset
            break

    if loader is None:
        raise ValueError("no Loader section (sectionKind==4) found")

    if loader + LOADER_INFO_HEADER_SIZE > len(data):
        raise ValueError("Loader section offset is out of range")

    # --- Loader Info Header ---
    imported_library_count = be32(data, loader + 24)
    total_imported_symbol_count = be32(data, loader + 28)
    loader_strings_offset = be32(data, loader + 40)

    strings_base = loader + loader_strings_offset

    # --- Imported Symbol table (needs to be read once, shared by all libs) ---
    lib_table_base = loader + LOADER_INFO_HEADER_SIZE
    sym_table_base = lib_table_base + imported_library_count * IMPORTED_LIBRARY_SIZE

    def symbol_name_and_class(index):
        entry = be32(data, sym_table_base + index * 4)
        sym_class = (entry >> 24) & 0xFF
        name_off = entry & 0x00FFFFFF
        name = read_cstring(data, strings_base + name_off)
        return name, SYMBOL_CLASSES.get(sym_class, "class0x%02X" % sym_class)

    libraries = []
    for i in range(imported_library_count):
        off = lib_table_base + i * IMPORTED_LIBRARY_SIZE
        name_off = be32(data, off + 0)
        old_ver = be32(data, off + 4)
        cur_ver = be32(data, off + 8)
        sym_count = be32(data, off + 12)
        first_sym = be32(data, off + 16)

        lib_name = read_cstring(data, strings_base + name_off)
        symbols = []
        for s in range(sym_count):
            idx = first_sym + s
            if idx >= total_imported_symbol_count:
                break
            symbols.append(symbol_name_and_class(idx))

        libraries.append({
            "name": lib_name,
            "old_version": old_ver,
            "current_version": cur_ver,
            "symbols": symbols,
        })

    return libraries


def main():
    ap = argparse.ArgumentParser(description="Extract imported symbols from a PEF binary.")
    ap.add_argument("binary", help="path to the PEF data fork")
    ap.add_argument("--library", "-l", default="CarbonLib",
                    help="library name to filter on (default: CarbonLib)")
    ap.add_argument("--all", "-a", action="store_true",
                    help="list every imported library, not just the filtered one")
    args = ap.parse_args()

    with open(args.binary, "rb") as f:
        data = f.read()

    try:
        libraries = parse_pef(data)
    except ValueError as e:
        sys.stderr.write("error: %s\n" % e)
        return 2

    if args.all:
        print("Imported libraries (%d):" % len(libraries))
        for lib in libraries:
            print("  %-24s v%08x  %d symbols"
                  % (lib["name"], lib["current_version"], len(lib["symbols"])))
        print()

    matches = [l for l in libraries if l["name"].lower() == args.library.lower()]
    if not matches:
        found = ", ".join(l["name"] for l in libraries) or "(none)"
        sys.stderr.write("error: library %r not imported. Imported: %s\n"
                         % (args.library, found))
        return 1

    for lib in matches:
        print("=== %s (%d imported symbols) ===" % (lib["name"], len(lib["symbols"])))
        for name, cls in sorted(lib["symbols"]):
            print("  %-8s %s" % (cls, name))

    return 0


if __name__ == "__main__":
    sys.exit(main())
