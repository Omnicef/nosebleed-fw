#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build-time team-logo atlas (T-3.1 / T-3.2).

Ports the *processing* half of the Marquee ``logo_pipeline.py`` (which is the
design reference) and serialises the result into the ``logos.bin`` format from
PLAN.md §3. This runs on the laptop only; the device never processes images.

Processing per logo (verbatim port of ``LogoPipeline._process``):
    trim getbbox -> contain-resize to ``height`` (LANCZOS) ->
    binary alpha threshold @128 -> UnsharpMask + saturation/contrast boost.

Then each logo is quantised to RGB565 colour + a 1-bit alpha mask and packed
with a sorted (league, abbr) index for on-device binary search.

File format (little-endian):
    header  magic "NBLG" | u16 version | u16 count | u16 logo_height | u16 rsvd
    index   count * { league[8] | abbr[4] | u32 offset | u8 w | u8 h | u16 rsvd }
    blobs   per entry at ``offset``: w*h*2 RGB565 bytes, then ceil(w/8)*h mask
            bytes; mask is MSB-first, one bit per pixel (1 = opaque).

Keys: ``league`` is the ESPN slug as-is (e.g. ``eng.1``, ``epl``); ``abbr`` is
upper-cased and must fit in 4 bytes. The two fields together are the lookup key
— that is what keeps ``eng.1:liv`` distinct from ``epl:liv``.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

from PIL import Image, ImageEnhance, ImageFilter

MAGIC = b"NBLG"
VERSION = 1
HEADER = struct.Struct("<4sHHHH")     # magic, version, count, logo_height, rsvd
ENTRY = struct.Struct("<8s4sIHHH")     # league, abbr, offset, w, h, rsvd
HEADER_SIZE = HEADER.size              # 12
ENTRY_SIZE = ENTRY.size                # 20

# Processing knobs — keep identical to Marquee logo_pipeline.py.
RESAMPLE = Image.LANCZOS
SHARPEN = True
ALPHA_THRESHOLD = 128


def process(img: Image.Image, height: int) -> Image.Image:
    """Trim -> contain-resize -> alpha-threshold -> sharpen. Pure port."""
    bbox = img.getbbox()
    if bbox:
        img = img.crop(bbox)

    scale = height / max(img.width, img.height)
    new_w = max(1, round(img.width * scale))
    new_h = max(1, round(img.height * scale))
    img = img.resize((new_w, new_h), RESAMPLE)

    r, g, b, a = img.split()
    a = a.point(lambda v: 255 if v >= ALPHA_THRESHOLD else 0)

    if SHARPEN:
        rgb = Image.merge("RGB", (r, g, b))
        rgb = rgb.filter(ImageFilter.UnsharpMask(radius=0.6, percent=140, threshold=2))
        rgb = ImageEnhance.Color(rgb).enhance(1.3)
        rgb = ImageEnhance.Contrast(rgb).enhance(1.1)
        r, g, b = rgb.split()

    return Image.merge("RGBA", (r, g, b, a))


def encode_blob(img: Image.Image) -> tuple[int, int, bytes]:
    """RGBA image -> (w, h, blob) where blob = RGB565 pixels + 1-bit mask."""
    w, h = img.size
    px = img.load()
    out = bytearray(w * h * 2)
    for i in range(w * h):
        x, y = i % w, i // w
        r, g, b, a = px[x, y]
        v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        out[2 * i] = v & 0xFF
        out[2 * i + 1] = v >> 8
    stride = (w + 7) // 8
    mask = bytearray(stride * h)
    for y in range(h):
        for x in range(w):
            if px[x, y][3] > 0:
                mask[y * stride + (x >> 3)] |= 0x80 >> (x & 7)
    return w, h, bytes(out) + bytes(mask)


def _key(league: str, abbr: str) -> tuple[str, str]:
    return (league, abbr.upper())


def build(entries: list[tuple[str, str, Image.Image]], height: int) -> bytes:
    """entries: list of (league, abbr, processed RGBA Image). Returns atlas bytes."""
    enc = [(_key(league, abbr), img) for league, abbr, img in entries]
    enc.sort(key=lambda e: e[0])  # binary-search order

    index = bytearray()
    blobs = bytearray()
    data_start = HEADER_SIZE + ENTRY_SIZE * len(enc)
    for (league, abbr), img in enc:
        w, h, blob = encode_blob(img)
        offset = data_start + len(blobs)
        index += ENTRY.pack(
            league.encode("ascii")[:8], abbr.encode("ascii")[:4], offset, w, h, 0
        )
        blobs += blob
    header = HEADER.pack(MAGIC, VERSION, len(enc), height, 0)
    return header + bytes(index) + bytes(blobs)


def load_logo_dir(logo_dir: Path, height: int) -> list[tuple[str, str, Image.Image]]:
    out = []
    for p in sorted(logo_dir.glob("*.png")):
        league, abbr = parse_name(p.stem)
        out.append((league, abbr, process(Image.open(p).convert("RGBA"), height)))
    return out


def read(data: bytes) -> tuple[dict[tuple[str, str], dict], int]:
    """Parse logos.bin -> ({(league, abbr): {w, h, offset, blob}}, logo_height)."""
    magic, version, count, height, _ = HEADER.unpack_from(data, 0)
    if magic != MAGIC:
        raise ValueError(f"bad magic {magic!r}")
    out: dict[tuple[str, str], dict] = {}
    for i in range(count):
        base = HEADER_SIZE + ENTRY_SIZE * i
        league, abbr, offset, w, h, _ = ENTRY.unpack_from(data, base)
        league = league.rstrip(b"\x00").decode("ascii")
        abbr = abbr.rstrip(b"\x00").decode("ascii")
        blob_len = w * h * 2 + ((w + 7) // 8) * h
        out[(league, abbr)] = {
            "w": w, "h": h, "offset": offset, "blob": data[offset:offset + blob_len],
        }
    return out, height


def parse_name(stem: str) -> tuple[str, str]:
    """`'eng.1_liv'` -> ('eng.1', 'liv'). League slug has no underscore."""
    league, _, abbr = stem.partition("_")
    return league, abbr


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("logo_dir", type=Path, help="dir of <league>_<abbr>.png files")
    ap.add_argument("-H", "--height", type=int, default=32)
    ap.add_argument("-o", "--out", type=Path, default=Path("logos.bin"))
    args = ap.parse_args(argv)

    entries = load_logo_dir(args.logo_dir, args.height)
    if not entries:
        print(f"no <league>_<abbr>.png files in {args.logo_dir}", file=sys.stderr)
        return 1
    args.out.write_bytes(build(entries, args.height))
    print(f"logos.bin: {len(entries)} logos, height {args.height}, "
          f"{args.out.stat().st_size} B")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
