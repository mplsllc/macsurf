#!/usr/bin/env python3
"""
Generate Classic Mac OS icon family resources from a transparent PNG.

Output:
  * tools/MacSurfIcon.r (Rez text — human-readable, can be #included
    from MacSurf.r when building under Rez)
  * browser/netsurf/frontends/macos9/MacSurf.rsrc (binary resource
    fork — what CW8 actually links into the application; replaces the
    previous carb-only fork)

Resources emitted at ID 128:
  ICN#  — 32x32 1-bit icon + 1-bit mask
  icl4  — 32x32 4-bit colour icon
  icl8  — 32x32 8-bit colour icon
  ics#  — 16x16 1-bit small icon + 1-bit mask
  ics4  — 16x16 4-bit colour small icon
  ics8  — 16x16 8-bit colour small icon

Plus FREF (128) and BNDL (128) so Finder maps the application's
creator code 'MPLS' to icon family 128, and 'carb' (0) so CarbonLib
keeps loading.

Why this matters:
  * Classic Mac applications need an icon FAMILY (not just the high-
    resolution OS X .icns blob). Finder picks ICN# / icl8 / icl4
    depending on the user's colour depth and small/large view mode.
  * The BNDL/FREF binding tells Finder "this app's creator is 'MPLS'
    and its icon is ID 128". Without it, the desktop database can't
    associate any icon with the application.

Palette choices:
  * 8-bit  — Apple's Macintosh System 8-bit palette
              (6x6x6 RGB cube + four 10-step ramps R/G/B/gray).
  * 4-bit  — Apple's Macintosh System 4-bit palette
              (white, yellow, orange, red, magenta, purple, blue,
               cyan, green, dark-green, brown, tan, light-grey,
               medium-grey, dark-grey, black).
  * 1-bit  — author-image black mask (alpha & luma threshold).

Usage:
  python3 tools/png_to_mac_icon_rez.py \\
      --src puffpuff.png \\
      --rez tools/MacSurfIcon.r \\
      --rsrc browser/netsurf/frontends/macos9/MacSurf.rsrc \\
      --icon-id 128 \\
      --creator MPLS

Dependencies: Pillow (`pip install Pillow`).
"""

from __future__ import annotations

import argparse
import os
import struct
import sys
from typing import List, Tuple

try:
    from PIL import Image
except ImportError:
    sys.stderr.write(
        "error: Pillow is required. install with: pip install Pillow\n"
    )
    sys.exit(2)


# -------------------------------------------------------------------
# Mac System 8-bit palette
# -------------------------------------------------------------------
def build_mac_8bit_palette() -> List[Tuple[int, int, int]]:
    """Apple Macintosh System 8-bit ('default') colour table.

    Layout:
      0..215:  6x6x6 RGB cube, components {255,204,153,102,51,0},
               iterated R outer, G middle, B inner. Index 0 is
               (255,255,255) (white); index 215 is (0,0,0) (black).
      216..225: pure red ramp, 238,221,187,170,136,119,85,68,34,17
      226..235: pure green ramp, same values
      236..245: pure blue ramp, same values
      246..255: pure gray ramp, same values (index 255 = (17,17,17))

    Note: the canonical Apple table actually puts pure black at the
    very last index. The cube-only path already has black at 215.
    We keep both for compatibility; quantisation never matters which
    index it picks for matching colours.
    """
    pal: List[Tuple[int, int, int]] = []
    steps = (255, 204, 153, 102, 51, 0)
    for r in steps:
        for g in steps:
            for b in steps:
                pal.append((r, g, b))
    assert len(pal) == 216
    ramp = (238, 221, 187, 170, 136, 119, 85, 68, 34, 17)
    for v in ramp:
        pal.append((v, 0, 0))
    for v in ramp:
        pal.append((0, v, 0))
    for v in ramp:
        pal.append((0, 0, v))
    for v in ramp:
        pal.append((v, v, v))
    assert len(pal) == 256
    return pal


# -------------------------------------------------------------------
# Mac System 4-bit palette
# -------------------------------------------------------------------
def build_mac_4bit_palette() -> List[Tuple[int, int, int]]:
    """Apple Macintosh System 4-bit colour table (16 entries).

    Order matters — these indices are what 'icl4'/'ics4' bytecode
    references."""
    return [
        (0xFF, 0xFF, 0xFF),  # 0  white
        (0xFC, 0xF3, 0x05),  # 1  yellow
        (0xFF, 0x64, 0x02),  # 2  orange
        (0xDD, 0x08, 0x06),  # 3  red
        (0xF2, 0x08, 0x84),  # 4  magenta
        (0x46, 0x00, 0xA5),  # 5  purple
        (0x00, 0x00, 0xD4),  # 6  blue
        (0x02, 0xAB, 0xEA),  # 7  cyan
        (0x1F, 0xB7, 0x14),  # 8  green
        (0x00, 0x64, 0x11),  # 9  dark green
        (0x56, 0x2C, 0x05),  # 10 brown
        (0x90, 0x71, 0x3A),  # 11 tan
        (0xC0, 0xC0, 0xC0),  # 12 light gray
        (0x80, 0x80, 0x80),  # 13 medium gray
        (0x40, 0x40, 0x40),  # 14 dark gray
        (0x00, 0x00, 0x00),  # 15 black
    ]


def _dist_sq(a: Tuple[int, int, int], b: Tuple[int, int, int]) -> int:
    dr = a[0] - b[0]
    dg = a[1] - b[1]
    db = a[2] - b[2]
    return dr * dr + dg * dg + db * db


def quantize(rgb: Tuple[int, int, int],
             palette: List[Tuple[int, int, int]]) -> int:
    best = 0
    best_d = _dist_sq(rgb, palette[0])
    for i in range(1, len(palette)):
        d = _dist_sq(rgb, palette[i])
        if d < best_d:
            best_d = d
            best = i
    return best


# -------------------------------------------------------------------
# Icon family generation
# -------------------------------------------------------------------
class IconFamily:
    """All six resource payloads for a single icon ID."""

    def __init__(self) -> None:
        self.icn_pound: bytes = b""  # 32x32 1-bit icon + mask, 256B
        self.icl4: bytes = b""        # 32x32 4-bit, 512B
        self.icl8: bytes = b""        # 32x32 8-bit, 1024B
        self.ics_pound: bytes = b""   # 16x16 1-bit + mask, 64B
        self.ics4: bytes = b""        # 16x16 4-bit, 128B
        self.ics8: bytes = b""        # 16x16 8-bit, 256B


def _composite_onto_white(img: Image.Image) -> Image.Image:
    """Flatten RGBA onto white background (for colour-icon paint).

    Mac icon resources have no alpha channel; transparency is carried
    by the parallel 1-bit mask. Inside the colour icon itself we
    paint the source RGB premultiplied against a known background so
    semi-transparent edges blend correctly when Finder uses the
    mask to overlay the icon on its workspace."""
    if img.mode != "RGBA":
        img = img.convert("RGBA")
    bg = Image.new("RGB", img.size, (255, 255, 255))
    bg.paste(img, mask=img.split()[3])
    return bg


def _mask_from_alpha(img: Image.Image, threshold: int = 64) -> Image.Image:
    """Produce a 1-bit mask. Alpha >= threshold -> opaque (255)."""
    if img.mode != "RGBA":
        img = img.convert("RGBA")
    alpha = img.split()[3]
    return alpha.point(lambda p: 255 if p >= threshold else 0).convert("1")


def _icon_1bit_from_rgb(rgb: Image.Image,
                        mask: Image.Image,
                        luma_threshold: int = 128) -> Image.Image:
    """1-bit icon plane: pixels where rgb is dark AND the mask is set.

    The icon plane is OR-painted by Finder on top of the workspace
    background using XOR semantics: where the mask is set AND the
    icon plane is set the pixel is black; where the mask is set AND
    the icon plane is clear the pixel is white; where the mask is
    clear the workspace shows through. That gives black-and-white
    icons a clean two-tone appearance even on coloured desktops."""
    gray = rgb.convert("L")
    raw = gray.point(lambda p: 255 if p < luma_threshold else 0)
    return Image.composite(raw, Image.new("L", rgb.size, 0), mask).convert("1")


def _pack_1bit(img: Image.Image) -> bytes:
    """Pack a 1-bit PIL image MSB-first into bytes (Mac BitMap order).

    Each row is padded to a whole byte; for 32-wide that's 4 bytes,
    for 16-wide 2 bytes."""
    if img.mode != "1":
        img = img.convert("1")
    w, h = img.size
    bytes_per_row = (w + 7) // 8
    out = bytearray(bytes_per_row * h)
    px = img.load()
    for y in range(h):
        for x in range(w):
            # PIL "1" mode: 0 = black/set, 255 = white/clear when
            # loaded from a thresholded L? Actually convert("1") makes
            # non-zero pixels = 255. We treat 255 as "set" (paint).
            if px[x, y]:
                out[y * bytes_per_row + (x // 8)] |= 0x80 >> (x & 7)
    return bytes(out)


def _pack_4bit(img: Image.Image,
               palette: List[Tuple[int, int, int]]) -> bytes:
    """Pack a 4-bit colour image to Mac icl4/ics4 layout.

    Each byte holds two pixels: high nibble = left, low nibble =
    right. Width is always even (32 or 16). Pixel order is row-major,
    top-to-bottom."""
    rgb = img.convert("RGB")
    w, h = rgb.size
    out = bytearray((w * h) // 2)
    px = rgb.load()
    for y in range(h):
        for x in range(0, w, 2):
            i_hi = quantize(px[x, y], palette) & 0x0F
            i_lo = quantize(px[x + 1, y], palette) & 0x0F
            out[(y * w + x) // 2] = (i_hi << 4) | i_lo
    return bytes(out)


def _pack_8bit(img: Image.Image,
               palette: List[Tuple[int, int, int]]) -> bytes:
    """Pack an 8-bit colour image to Mac icl8/ics8 layout — one byte
    per pixel, row-major."""
    rgb = img.convert("RGB")
    w, h = rgb.size
    out = bytearray(w * h)
    px = rgb.load()
    for y in range(h):
        for x in range(w):
            out[y * w + x] = quantize(px[x, y], palette)
    return bytes(out)


def build_family(src: Image.Image) -> IconFamily:
    """Generate all six icon resources from one source PNG."""
    pal8 = build_mac_8bit_palette()
    pal4 = build_mac_4bit_palette()
    fam = IconFamily()
    for size, target in ((32, "large"), (16, "small")):
        # PIL LANCZOS is a high-quality downsample appropriate for
        # both icon planes (we'll threshold mask afterwards anyway).
        scaled = src.resize((size, size), Image.LANCZOS)
        mask_img = _mask_from_alpha(scaled)
        rgb_img = _composite_onto_white(scaled)
        icon_1bit = _icon_1bit_from_rgb(rgb_img, mask_img)
        icon_bytes = _pack_1bit(icon_1bit)
        mask_bytes = _pack_1bit(mask_img)
        if target == "large":
            fam.icn_pound = icon_bytes + mask_bytes
            fam.icl4 = _pack_4bit(rgb_img, pal4)
            fam.icl8 = _pack_8bit(rgb_img, pal8)
        else:
            fam.ics_pound = icon_bytes + mask_bytes
            fam.ics4 = _pack_4bit(rgb_img, pal4)
            fam.ics8 = _pack_8bit(rgb_img, pal8)
    return fam


# -------------------------------------------------------------------
# Rez text emission
# -------------------------------------------------------------------
def _rez_hex_block(payload: bytes, indent: str = "\t") -> str:
    """Emit a Rez hex literal block. Rez accepts `$"AABBCC"` per byte
    line. We wrap at 32 bytes (64 hex chars) per line for readability
    plus a trailing comma-or-empty."""
    chunks = []
    for i in range(0, len(payload), 16):
        line = payload[i:i + 16]
        hex_chars = "".join("%02X" % b for b in line)
        chunks.append('%s$"%s"' % (indent, hex_chars))
    return "\n".join(chunks)


def emit_rez_text(fam: IconFamily, icon_id: int, creator: str) -> str:
    """Return the Rez source for the icon family + FREF + BNDL.

    The output is meant to be #included (or pasted) into MacSurf.r.
    Resource types referenced ('ICN#', 'icl8', etc.) are part of
    Apple's standard SysTypes.r / MacTypes.r distributed with
    CodeWarrior 8 and Rez 1.0 — no local type declarations needed."""
    lines: List[str] = []
    lines.append("/* -----------------------------------------------------------")
    lines.append(" * MacSurfIcon.r -- generated by tools/png_to_mac_icon_rez.py")
    lines.append(" * Do not hand-edit. Regenerate from puffpuff.png.")
    lines.append(" * ----------------------------------------------------------- */")
    lines.append("")
    table = [
        ("ICN#", fam.icn_pound),
        ("icl4", fam.icl4),
        ("icl8", fam.icl8),
        ("ics#", fam.ics_pound),
        ("ics4", fam.ics4),
        ("ics8", fam.ics8),
    ]
    for rtype, data in table:
        lines.append("data '%s' (%d, \"MacSurf icon\") {" % (rtype, icon_id))
        lines.append(_rez_hex_block(data))
        lines.append("};")
        lines.append("")
    lines.append("resource 'FREF' (%d, \"MacSurf APPL\") {" % icon_id)
    lines.append('\t\'APPL\', 0, ""')
    lines.append("};")
    lines.append("")
    lines.append("resource 'BNDL' (%d, \"MacSurf BNDL\") {" % icon_id)
    lines.append("\t'%s', 0," % creator)
    lines.append("\t{")
    lines.append("\t\t'ICN#', { 0, %d },"   % icon_id)
    lines.append("\t\t'FREF', { 0, %d }"    % icon_id)
    lines.append("\t}")
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


# -------------------------------------------------------------------
# Binary .rsrc fork builder
# -------------------------------------------------------------------
class ResourceFork:
    """Tiny resource-fork builder.

    Resources are inserted by (type, id, data, name=None, attrs=0).
    Call build() to get bytes() ready to write to disk.

    Layout (see Apple Inside Macintosh: Memory > Resource Manager):
        +0x000  16-byte fork header (data_off, map_off, data_len, map_len)
        +0x010  reserved (240 bytes) — application use; we zero-fill
        +0x100  resource data area
                  per-resource:
                    4-byte data length, then raw bytes
        +mapoff resource map (50 bytes admin + type list + ref lists +
                              name list)
    """

    def __init__(self) -> None:
        # Preserve insertion order so the type list is deterministic.
        self._records: List[dict] = []

    def add(self,
            rtype: bytes,
            rid: int,
            data: bytes,
            name: bytes = b"",
            attrs: int = 0) -> None:
        if len(rtype) != 4:
            raise ValueError("type must be 4 bytes")
        if not (-0x8000 <= rid <= 0x7FFF):
            raise ValueError("id out of range")
        self._records.append({
            "type": rtype,
            "id": rid & 0xFFFF,
            "data": data,
            "name": name,
            "attrs": attrs & 0xFF,
        })

    def build(self) -> bytes:
        # ----- pack resource data area -----
        data_blob = bytearray()
        for rec in self._records:
            rec["data_off"] = len(data_blob)
            data_blob += struct.pack(">I", len(rec["data"]))
            data_blob += rec["data"]

        data_offset = 0x100
        data_length = len(data_blob)
        map_offset = data_offset + data_length

        # ----- group records by type, preserving first-seen order -----
        type_order: List[bytes] = []
        type_groups: dict = {}
        for rec in self._records:
            t = rec["type"]
            if t not in type_groups:
                type_groups[t] = []
                type_order.append(t)
            type_groups[t].append(rec)
        for t in type_order:
            type_groups[t].sort(key=lambda r: r["id"])

        # ----- pack name list -----
        name_blob = bytearray()
        for rec in self._records:
            if rec["name"]:
                rec["name_off"] = len(name_blob)
                if len(rec["name"]) > 255:
                    raise ValueError("resource name too long")
                name_blob.append(len(rec["name"]))
                name_blob += rec["name"]
            else:
                rec["name_off"] = 0xFFFF

        # ----- compute reference-list offsets and the map layout -----
        # Type list: 2-byte (n_types - 1), then 8 bytes per type entry.
        n_types = len(type_order)
        type_list_size = 2 + 8 * n_types
        # Each ref-list entry is 12 bytes.
        ref_list_total = sum(12 * len(type_groups[t]) for t in type_order)

        # Type list offset is just after the 28-byte map admin block.
        type_list_offset = 28
        ref_lists_start = type_list_offset + type_list_size
        name_list_offset = ref_lists_start + ref_list_total

        # Walk the type entries to assign ref-list offsets.
        ref_off_cursor = ref_lists_start
        # Note: 'offset to ref list' is from the start of the TYPE
        # LIST, not the map.
        per_type_offsets = {}
        for t in type_order:
            per_type_offsets[t] = ref_off_cursor - type_list_offset
            ref_off_cursor += 12 * len(type_groups[t])

        # ----- build map admin -----
        map_admin = struct.pack(
            ">16sIHHHH",
            b"\x00" * 16,   # reserved (filled at load time)
            0,              # next-map handle
            0,              # file ref num
            0,              # file attrs
            type_list_offset,
            name_list_offset,
        )

        # ----- build type list -----
        type_list = bytearray()
        type_list += struct.pack(">H", n_types - 1)
        for t in type_order:
            type_list += struct.pack(
                ">4sHH",
                t,
                len(type_groups[t]) - 1,
                per_type_offsets[t],
            )

        # ----- build ref lists -----
        ref_lists = bytearray()
        for t in type_order:
            for rec in type_groups[t]:
                ref_lists += struct.pack(
                    ">HHB3sI",
                    rec["id"],
                    rec["name_off"],
                    rec["attrs"],
                    struct.pack(">I", rec["data_off"])[1:],  # 24-bit
                    0,
                )

        rmap = map_admin + bytes(type_list) + bytes(ref_lists) + bytes(name_blob)
        map_length = len(rmap)

        # ----- 16-byte fork header -----
        header = struct.pack(
            ">IIII",
            data_offset,
            map_offset,
            data_length,
            map_length,
        )

        # Pad the 0x10..0x100 reserved area.
        padding = b"\x00" * (data_offset - 16)

        return bytes(header + padding + bytes(data_blob) + rmap)


# -------------------------------------------------------------------
# BNDL / FREF binary builders
# -------------------------------------------------------------------
def build_fref_resource(file_type: bytes, icon_local_id: int) -> bytes:
    """Build the binary payload for a 'FREF' resource.

    FREF format:
        4-byte file type code  (e.g. 'APPL')
        2-byte icon local ID
        1-byte name-length-prefixed Pascal string (empty here)
    """
    if len(file_type) != 4:
        raise ValueError("file type must be 4 bytes")
    return struct.pack(">4sH", file_type, icon_local_id) + b"\x00"


def build_bndl_resource(creator: bytes,
                        signature_id: int,
                        icon_local_id: int,
                        icon_resource_id: int,
                        fref_local_id: int,
                        fref_resource_id: int) -> bytes:
    """Build the binary payload for a 'BNDL' resource.

    BNDL format:
        4-byte creator signature
        2-byte signature resource ID (normally 0)
        2-byte (count of resource-type entries minus 1)
            per entry:
                4-byte resource type ('ICN#' or 'FREF')
                2-byte (count of mappings minus 1)
                    per mapping:
                        2-byte local ID
                        2-byte resource ID
    """
    if len(creator) != 4:
        raise ValueError("creator must be 4 bytes")
    buf = bytearray()
    buf += struct.pack(">4sH", creator, signature_id)
    # Two resource-type entries: ICN# and FREF.
    buf += struct.pack(">H", 2 - 1)
    # ICN# mapping
    buf += b"ICN#"
    buf += struct.pack(">H", 1 - 1)
    buf += struct.pack(">HH", icon_local_id, icon_resource_id)
    # FREF mapping
    buf += b"FREF"
    buf += struct.pack(">H", 1 - 1)
    buf += struct.pack(">HH", fref_local_id, fref_resource_id)
    return bytes(buf)


# -------------------------------------------------------------------
# main
# -------------------------------------------------------------------
def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--src", required=True,
                    help="source 512x512 RGBA PNG (e.g. puffpuff.png)")
    ap.add_argument("--rez", required=True,
                    help="output Rez source (e.g. tools/MacSurfIcon.r)")
    ap.add_argument("--rsrc", required=True,
                    help="output binary resource fork "
                         "(e.g. browser/.../MacSurf.rsrc)")
    ap.add_argument("--icon-id", type=int, default=128,
                    help="resource ID for the icon family + FREF + BNDL "
                         "(default: 128)")
    ap.add_argument("--creator", default="MPLS",
                    help="four-character creator code, CASE-SENSITIVE "
                         "(default: MPLS)")
    ap.add_argument("--file-type", default="APPL",
                    help="four-character file type (default: APPL)")
    args = ap.parse_args()

    if len(args.creator) != 4:
        sys.stderr.write("error: creator must be exactly 4 characters\n")
        return 2
    if len(args.file_type) != 4:
        sys.stderr.write("error: file type must be exactly 4 characters\n")
        return 2

    src = Image.open(args.src)
    if src.mode != "RGBA":
        src = src.convert("RGBA")

    fam = build_family(src)

    # ---- Rez text ----
    rez_text = emit_rez_text(fam, args.icon_id, args.creator)
    os.makedirs(os.path.dirname(args.rez) or ".", exist_ok=True)
    with open(args.rez, "w") as f:
        f.write(rez_text)
    sys.stdout.write("wrote %s (%d bytes Rez text)\n" %
                     (args.rez, len(rez_text)))

    # ---- binary .rsrc ----
    fork = ResourceFork()
    # 'carb' (0) — keep CarbonLib happy.
    fork.add(b"carb", 0, b"")
    # Icon family.
    fork.add(b"ICN#", args.icon_id, fam.icn_pound)
    fork.add(b"icl4", args.icon_id, fam.icl4)
    fork.add(b"icl8", args.icon_id, fam.icl8)
    fork.add(b"ics#", args.icon_id, fam.ics_pound)
    fork.add(b"ics4", args.icon_id, fam.ics4)
    fork.add(b"ics8", args.icon_id, fam.ics8)
    # FREF + BNDL.
    fref = build_fref_resource(args.file_type.encode("ascii"), 0)
    fork.add(b"FREF", args.icon_id, fref)
    bndl = build_bndl_resource(
        creator=args.creator.encode("ascii"),
        signature_id=0,
        icon_local_id=0,
        icon_resource_id=args.icon_id,
        fref_local_id=0,
        fref_resource_id=args.icon_id,
    )
    fork.add(b"BNDL", args.icon_id, bndl)
    blob = fork.build()
    os.makedirs(os.path.dirname(args.rsrc) or ".", exist_ok=True)
    with open(args.rsrc, "wb") as f:
        f.write(blob)
    sys.stdout.write("wrote %s (%d bytes binary fork)\n" %
                     (args.rsrc, len(blob)))

    sys.stdout.write(
        "\nResource summary (ID %d):\n"
        "  carb  (0)   %4d bytes  -- CarbonLib loader marker\n"
        "  ICN#  (%d)  %4d bytes  -- 32x32 1-bit icon + mask\n"
        "  icl4  (%d)  %4d bytes  -- 32x32 4-bit colour\n"
        "  icl8  (%d)  %4d bytes  -- 32x32 8-bit colour\n"
        "  ics#  (%d)  %4d bytes  -- 16x16 1-bit small + mask\n"
        "  ics4  (%d)  %4d bytes  -- 16x16 4-bit small\n"
        "  ics8  (%d)  %4d bytes  -- 16x16 8-bit small\n"
        "  FREF  (%d)  %4d bytes  -- file type %s\n"
        "  BNDL  (%d)  %4d bytes  -- creator %s\n"
        % (args.icon_id,
           0,
           args.icon_id, len(fam.icn_pound),
           args.icon_id, len(fam.icl4),
           args.icon_id, len(fam.icl8),
           args.icon_id, len(fam.ics_pound),
           args.icon_id, len(fam.ics4),
           args.icon_id, len(fam.ics8),
           args.icon_id, len(fref), args.file_type,
           args.icon_id, len(bndl), args.creator))
    return 0


if __name__ == "__main__":
    sys.exit(main())
