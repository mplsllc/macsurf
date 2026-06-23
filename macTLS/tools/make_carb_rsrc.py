#!/usr/bin/env python3
"""
Generate a Mac OS classic resource fork containing a single zero-length
'carb' resource at ID 0. CodeWarrior 8 links .rsrc files directly into
the output resource fork with no Rez step, so this is sufficient to
make a CFM binary identify itself as a Carbon fragment to CarbonLib.

Why this matters (from MacSurf CLAUDE.md):
  "Without 'carb', CFM treats the binary as classic PEF, CarbonLib does
   not load as a dependency, and any *InContext OT call enters an
   uninitialized CarbonLib client context and crashes."

Resource fork format reference: Apple "Inside Macintosh: More Macintosh
Toolbox", chapter 1 (Resource Manager). Layout:

    [resource data area]   -- 0 bytes for our case
    [zero padding to 0x100]
    [resource map]         -- 50 bytes for a single empty resource

The map contains:
    16 bytes  reserved for in-memory copy of header (zero on disk;
              loaded by Resource Manager when the file is opened)
     4 bytes  next-map handle (file: 0)
     2 bytes  file ref num     (file: 0)
     2 bytes  file attrs       (0)
     2 bytes  type list offset from map start (28)
     2 bytes  name list offset from map start (50)
     -- type list --
     2 bytes  (num types - 1)  (0 = 1 type)
     4 bytes  resource type    ('carb')
     2 bytes  (num resources - 1) (0 = 1 instance)
     2 bytes  offset to ref list from type list start (10)
     -- ref list --
     2 bytes  resource ID      (0)
     2 bytes  name offset      (0xFFFF = no name)
     1 byte   attrs
     3 bytes  data offset (24-bit, into the data area; we use 0)
     4 bytes  reserved
     -- name list --
     (empty)

Usage:
  python3 tools/make_carb_rsrc.py path/to/output.rsrc

Verified against browser/netsurf/frontends/macos9/MacSurf.rsrc.
"""

import struct
import sys


def build_carb_rsrc():
    """Return a 306-byte bytes object containing a single 'carb'(0)
    zero-length resource. Byte-identical (modulo the file ref num and
    attribute fields, which are zero in both) to MacSurf.rsrc."""

    # Resource data area: zero bytes. But the header reserves the
    # 0..0x100 range for data, so we pad to 256.
    data_offset = 0x100
    data_length = 0
    map_offset = 0x100
    map_length = 0x32  # 50 bytes

    # 16-byte fork header
    header = struct.pack(
        ">IIII",
        data_offset,
        map_offset,
        data_length,
        map_length,
    )

    # Pad to data_offset (the data area is empty so it's all zero).
    padding = b"\x00" * (data_offset - len(header))

    # Resource map.
    type_list_offset = 28        # from map start; immediately after the
                                 # 16-byte header echo + 12 bytes of map
                                 # admin (handle, refnum, attrs, two
                                 # offsets).
    name_list_offset = 50        # from map start; immediately after the
                                 # type+ref list block.

    # First 16 bytes of the map are reserved for the in-memory copy of
    # the fork header. On disk they are conventionally zero; the
    # Resource Manager fills them in when it opens the file. MacSurf's
    # MacSurf.rsrc follows this convention.
    map_reserved = b"\x00" * 16
    map_admin = struct.pack(
        ">IHHHH",
        0,                       # next-map handle (file: always 0)
        0,                       # file ref num (file: always 0)
        0,                       # file attributes
        type_list_offset,
        name_list_offset,
    )

    # Type list (one entry, 'carb', ref-list offset 10 bytes into type
    # list).
    type_list = struct.pack(
        ">H4sHH",
        0,                       # (num types - 1), 0 means 1 type
        b"carb",
        0,                       # (num resources of this type - 1)
        10,                      # offset to ref list from type list start
    )

    # Ref list (one 12-byte entry for ID 0).
    ref_list = struct.pack(
        ">HHB3sI",
        0,                       # resource ID
        0xFFFF,                  # name offset = none
        0,                       # attribute byte
        b"\x00\x00\x00",         # data offset (24-bit) = 0
        0,                       # 4 reserved bytes
    )

    # Name list: empty.
    name_list = b""

    rmap = map_reserved + map_admin + type_list + ref_list + name_list
    assert len(rmap) == map_length, "map length mismatch: %d != %d" % (
        len(rmap), map_length)

    fork = header + padding + rmap
    assert len(fork) == data_offset + map_length, \
        "fork length mismatch"
    return fork


def main():
    if len(sys.argv) != 2:
        sys.stderr.write(
            "usage: make_carb_rsrc.py <output.rsrc>\n"
            "\n"
            "Writes a 306-byte resource fork containing 'carb'(0), suitable\n"
            "for inclusion in any Carbon CFM CodeWarrior 8 project on OS 9.\n"
        )
        sys.exit(2)

    out_path = sys.argv[1]
    fork = build_carb_rsrc()
    with open(out_path, "wb") as f:
        f.write(fork)
    print("Wrote %s (%d bytes)" % (out_path, len(fork)))


if __name__ == "__main__":
    main()
