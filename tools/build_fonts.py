#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
#
# T-2.3 — BDF bitmap fonts -> C glyph tables in lib/render/font_data.h.
#
# Source of truth: the BDFs in assets/fonts/. Pixel reference: Pillow rendering
# the *same* BDFs via ImageFont.load() — exactly what the Python draws with.
# Every glyph is baked into its FONTBOUNDINGBOX cell and cross-checked against
# Pillow before anything is written, so "byte-match ImageFont.load()" is proven,
# not assumed. The blit model is derived empirically from PIL (see WORKLOG.md):
#   * a string renders to (len * advance) x box_h, top-left pasted at draw.text
#     origin; glyph i lives in cell [i*advance, i*advance + advance);
#   * inside a cell the BBX raster sits at (xoff, ascent - yoff - height);
#   * text_width == len * advance.
#
# Usage: tools/build_fonts.py [--check]   (--check verifies, writes nothing)

from __future__ import annotations

import argparse
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
FONT_DIR = REPO / "assets" / "fonts"
OUT_HEADER = REPO / "lib" / "render" / "font_data.h"

FONTS_TO_BUILD = [
    ("spleen-5x8", "spleen_5x8"),
    ("spleen-6x12", "spleen_6x12"),
    ("tom-thumb", "tom_thumb"),
]

FIRST, LAST = 32, 126  # inclusive ASCII range emitted
MAX_ROWS = 12          # largest FONTBOUNDINGBOX height


class FontDef:
    def __init__(self):
        self.box_w = self.box_h = self.ascent = 0
        self.advance = 0
        # code -> (bw, bh, xoff, yoff, rows[ int per bitmap row, low bw bits ])
        self.glyphs: dict[int, tuple[int, int, int, int, list[int]]] = {}


def parse_bdf(path: Path) -> FontDef:
    f = FontDef()
    lines = path.read_bytes().split(b"\n")
    i, n = 0, len(lines)
    while i < n:
        ln = lines[i]
        if ln.startswith(b"FONTBOUNDINGBOX"):
            _, w, h, _x, _y = ln.split()
            f.box_w, f.box_h = int(w), int(h)
        elif ln.startswith(b"FONT_ASCENT"):
            f.ascent = int(ln.split()[1])
        elif ln.startswith(b"STARTCHAR"):
            i += 1
            props: dict[str, str] = {}
            while not lines[i].startswith(b"BITMAP"):
                if b" " in lines[i]:
                    k = lines[i].split(b" ")[0].decode()
                    v = lines[i].split(b" ", 1)[1].decode().strip()
                    props[k] = v
                i += 1
            i += 1
            rows = []
            while not lines[i].startswith(b"ENDCHAR"):
                tok = lines[i].strip().replace(b" ", b"")
                if tok:
                    rows.append(int(tok, 16))
                i += 1
            if "ENCODING" in props and "BBX" in props:
                code = int(props["ENCODING"])
                bw, bh, xoff, yoff = (int(x) for x in props["BBX"].split())
                nbytes = (bw + 7) // 8
                shift = nbytes * 8 - bw
                rr = [(r >> shift) & ((1 << bw) - 1) for r in rows]
                f.glyphs[code] = (bw, bh, xoff, yoff, rr)
                if f.advance == 0 and "DWIDTH" in props:
                    f.advance = int(props["DWIDTH"].split()[0])
        i += 1
    return f


def into_cell(fd: FontDef, code: int) -> list[int]:
    cell = [0] * fd.box_h
    if code not in fd.glyphs:
        return cell
    bw, bh, xoff, yoff, rows = fd.glyphs[code]
    top = fd.ascent - yoff - bh
    shift = fd.box_w - bw - xoff
    if shift < 0:
        return cell
    for r in range(bh):
        cr = top + r
        if 0 <= cr < fd.box_h:
            cell[cr] |= (rows[r] if r < len(rows) else 0) << shift
    return cell


def pillow_cells(bdf: Path, fd: FontDef) -> dict[int, list[int]]:
    """Render each char via PIL exactly as the Python does; read the box ink."""
    import tempfile
    from PIL import BdfFontFile, ImageFont, Image, ImageDraw

    with open(bdf, "rb") as fp:
        compiled = BdfFontFile.BdfFontFile(fp)
    compiled.compile()
    with tempfile.TemporaryDirectory() as d:
        pil = Path(d) / "f.pil"
        compiled.save(str(pil))
        loaded = ImageFont.load(str(pil))
        out = {}
        probe = Image.new("1", (fd.advance, fd.box_h), 0)
        draw = ImageDraw.Draw(probe)
        for code in range(FIRST, LAST + 1):
            probe.paste(0, (0, 0, fd.advance, fd.box_h))
            draw.text((0, 0), chr(code), font=loaded, fill=1)
            px = probe.load()
            assert px is not None
            rows = []
            for rr in range(fd.box_h):
                row = 0
                for cc in range(fd.box_w):
                    if px[cc, rr]:
                        row |= 1 << (fd.box_w - 1 - cc)
                rows.append(row)
            out[code] = rows
    return out


def font_block(ident: str, fd: FontDef, cells: dict[int, list[int]]) -> str:
    lines = [
        f"static const Glyph GLY_{ident.upper()}[95] = {{  // ASCII {FIRST}..{LAST}, box {fd.box_w}x{fd.box_h}, adv {fd.advance}"
    ]
    for code in range(FIRST, LAST + 1):
        rows = cells[code]
        body = ",".join(f"0x{(rows[r] if r < len(rows) else 0) & 0xFF:02X}" for r in range(MAX_ROWS))
        # Comment labels are display-only. Keep them alphanumeric-only: a raw
        # backslash at end of line is a C line-continuation that would splice
        # (and thus delete) the next glyph's initializer; other punctuation is
        # only harmless in a // comment but this removes the whole class.
        c = chr(code)
        disp = c if c.isalnum() else ("sp" if code == 32 else "?")
        lines.append(f"    {{ {{ {body} }} }},  // {code} {disp}")
    lines.append("};")
    lines.append(
        f"const Font FONT_{ident.upper()} = {{ GLY_{ident.upper()}, {FIRST}, 95, "
        f"{fd.advance}, {fd.box_w}, {fd.box_h}, {fd.ascent} }};"
    )
    return "\n".join(lines)


HEADER = """\
// SPDX-License-Identifier: GPL-3.0-only
//
// GENERATED by tools/build_fonts.py — do not edit by hand.
// Glyph rasters baked into FONTBOUNDINGBOX cells (each row's low box_w bits,
// MSB-first), cross-checked pixel-for-pixel against Pillow's rendering of the
// same BDFs.

#pragma once

#include "font.h"

namespace nb {

"""


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    if not FONT_DIR.exists():
        print(f"missing {FONT_DIR}", file=sys.stderr)
        return 1

    blocks, failures = [], []
    for bdf_name, ident in FONTS_TO_BUILD:
        bdf = FONT_DIR / f"{bdf_name}.bdf"
        fd = parse_bdf(bdf)
        if fd.box_h > MAX_ROWS:
            print(f"{bdf_name}: box_h {fd.box_h} > MAX_ROWS {MAX_ROWS}", file=sys.stderr)
            return 1
        ref = pillow_cells(bdf, fd)
        cells = {c: into_cell(fd, c) for c in range(FIRST, LAST + 1)}
        for code in range(FIRST, LAST + 1):
            if cells[code] != ref[code]:
                failures.append(
                    f"{bdf_name} {code} {chr(code)!r}:\n"
                    f"      mine={cells[code]}\n      pil ={ref[code]}"
                )
        blocks.append(font_block(ident, fd, cells))

    if failures:
        print(f"build_fonts: PIL CROSS-CHECK FAILED ({len(failures)} glyphs)",
              file=sys.stderr)
        for f in failures[:12]:
            print("  " + f, file=sys.stderr)
        return 1

    if args.check:
        print("build_fonts: OK — every glyph matches Pillow (nothing written)")
        return 0

    OUT_HEADER.write_text(HEADER + "\n\n".join(blocks) + "\n\n}  // namespace nb\n")
    print("build_fonts: wrote lib/render/font_data.h")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
