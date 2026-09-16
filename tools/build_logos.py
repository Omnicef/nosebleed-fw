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
import io
import json
import struct
import sys
import time
import urllib.error
import urllib.request
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


# --------------------------------------------------------------------------
# ESPN fetch (T-3.3). Be a good API citizen: /teams once per league, 0.4 s
# between logo downloads (never a burst), raw originals cached so a re-run
# does no network I/O, and fail-fast after repeated failures instead of
# hammering the CDN. UA facts from SPIKE_RESULTS: ESPN's CDN allows
# python-requests/* and rejects Mozilla/Chrome UAs with 403.
# --------------------------------------------------------------------------

ESPN_BASE = "https://site.api.espn.com/apis/site/v2/sports"
ESPN_PATHS = {  # internal league slug -> ESPN path (matches Marquee LEAGUE_SLUGS)
    "nfl": "football/nfl",
    "nba": "basketball/nba",
    "mlb": "baseball/mlb",
    "nhl": "hockey/nhl",
    "epl": "soccer/eng.1",
}
UA = {"User-Agent": "python-requests/2.31"}
PRO_LEAGUES = ["nfl", "nba", "mlb", "nhl", "epl"]
SKIP_LOGO_RELS = {"wordmark", "scoreboard"}
MAX_FETCH_FAILURES = 5  # abort the whole run instead of retrying hard


def http_get(url: str, tries: int = 3) -> bytes:
    last: Exception | None = None
    for attempt in range(tries):
        try:
            with urllib.request.urlopen(
                    urllib.request.Request(url, headers=UA), timeout=15) as r:
                return r.read()
        except Exception as exc:  # URLError, HTTPError, timeout
            last = exc
            if attempt < tries - 1:
                time.sleep(0.5 * (2 ** attempt))  # 0.5 s, 1.0 s
    raise RuntimeError(f"GET {url}: {last}")


def pick_squarest_logo(logos: list[dict]) -> str:
    """Aspect-ratio-closest-to-1:1 variant, skipping wordmark/scoreboard.
    Port of Marquee espn_client._pick_squarest_logo."""
    cands = [l for l in logos if not (SKIP_LOGO_RELS & set(l.get("rel", [])))] or logos

    def aspect(logo: dict) -> float:
        w, h = logo.get("width", 0), logo.get("height", 0)
        return max(w, h) / min(w, h) if w and h else float("inf")

    return min(cands, key=aspect).get("href", "")


def fetch_teams(league: str, cache_dir: Path) -> list[dict]:
    """[{abbr, color, url}] for one league; teams JSON cached to disk."""
    cache = cache_dir / f"teams_{league}.json"
    if not cache.exists():
        cache.write_bytes(http_get(f"{ESPN_BASE}/{ESPN_PATHS[league]}/teams"))
    raw = json.loads(cache.read_bytes())
    out = []
    for entry in raw["sports"][0]["leagues"][0]["teams"]:
        t = entry["team"]
        out.append({"abbr": t["abbreviation"].upper(),
                    "color": t.get("color", "ffffff"),
                    "url": pick_squarest_logo(t.get("logos", []))})
    return out


def fetch_all(leagues: list[str], cache_dir: Path, delay: float) -> int:
    """Download raw originals to cache_dir + a colors.json side table.
    Returns failure count. Caches make re-runs free."""
    failures = 0
    colors: dict[str, str] = {}
    color_file = cache_dir / "colors.json"
    if color_file.exists():
        colors = json.loads(color_file.read_text())
    for league in leagues:
        teams = fetch_teams(league, cache_dir)
        print(f"{league}: {len(teams)} teams")
        for t in teams:
            dest = cache_dir / f"{league}_{t['abbr'].lower()}.png"
            colors[f"{league}:{t['abbr']}"] = t["color"]
            if not dest.exists():
                try:
                    data = http_get(t["url"])
                    Image.open(io.BytesIO(data)).verify()  # reject HTML error pages
                    dest.write_bytes(data)
                except Exception as exc:
                    print(f"  FAIL {league}:{t['abbr']}: {exc}", file=sys.stderr)
                    failures += 1
                    if failures >= MAX_FETCH_FAILURES:
                        color_file.write_text(json.dumps(colors, sort_keys=True))
                        print(f"aborting: {failures} failures (fail-fast)",
                              file=sys.stderr)
                        return failures
                    time.sleep(1.0)  # one bad logo deserves a breather
                    continue
                time.sleep(delay)
        color_file.write_text(json.dumps(colors, sort_keys=True))
    return failures


def spotcheck(cache_dir: Path, marquee_dir: Path, height: int) -> int:
    """Same raw bytes in => byte-identical atlas entries out. A key whose raw
    bytes differ from the corpus is an upstream rebrand (ESPN changed the
    artwork since the corpus was cached) — reported, not an error."""
    ref_table, _ = read(build(load_logo_dir(marquee_dir, height), height))
    identical = changed = 0
    for key, rec in ref_table.items():
        stem = f"{key[0]}_{key[1].lower()}"
        mine, theirs = cache_dir / f"{stem}.png", marquee_dir / f"{stem}.png"
        if not mine.exists():
            continue
        if mine.read_bytes() != theirs.read_bytes():
            print(f"  source change: {key[0]}:{key[1]} (ESPN artwork != corpus)")
            changed += 1
            continue
        w, h, blob = encode_blob(process(Image.open(mine).convert("RGBA"), height))
        assert (w, h, blob) == (rec["w"], rec["h"], rec["blob"]), \
            f"{key}: atlas differs from Marquee-corpus render of identical source"
        identical += 1
    print(f"spot-check OK: {identical} shared keys byte-identical, "
          f"{changed} upstream artwork changes (expected: rebrands)")
    return identical


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("logo_dir", nargs="?", type=Path, default=Path("assets/logos"),
                    help="dir of <league>_<abbr>.png raw originals (also the fetch cache)")
    ap.add_argument("-H", "--height", type=int, default=32)
    ap.add_argument("-o", "--out", type=Path, default=Path("logos.bin"))
    ap.add_argument("--fetch", action="store_true",
                    help="fetch teams + raw logos from ESPN first (cached)")
    ap.add_argument("--league", action="append", choices=PRO_LEAGUES,
                    help="league to fetch (repeatable); default all five")
    ap.add_argument("--delay", type=float, default=0.4,
                    help="seconds between logo downloads")
    ap.add_argument("--spotcheck", type=Path, default=None, metavar="MARQUEE_LOGO_DIR",
                    help="byte-compare shared keys against a reference logo dir")
    args = ap.parse_args(argv)

    if args.fetch:
        if fetch_all(args.league or PRO_LEAGUES, args.logo_dir, args.delay):
            return 1

    entries = load_logo_dir(args.logo_dir, args.height)
    if not entries:
        print(f"no <league>_<abbr>.png files in {args.logo_dir}", file=sys.stderr)
        return 1
    data = build(entries, args.height)
    args.out.write_bytes(data)
    print(f"logos.bin: {len(entries)} logos, height {args.height}, "
          f"{args.out.stat().st_size} B")

    if args.spotcheck:
        spotcheck(args.logo_dir, args.spotcheck, args.height)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
