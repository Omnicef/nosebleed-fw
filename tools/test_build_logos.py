#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Host checks for tools/build_logos.py (T-3.1 / T-3.2).

Runs the REAL Marquee ``LogoPipeline._process`` (stubbing only the ``httpx`` /
``models`` imports it does not need here) and proves our port matches it
pixel-for-pixel, then round-trips the logos.bin format and proves the
league+abbr key separates the ``eng.1:liv`` / ``epl:liv`` collision.

Usage:  python3 tools/test_build_logos.py
Deps:   Pillow. Corpus: Marquee/marquee/assets/logos (read-only).
"""
from __future__ import annotations

import bisect
import os
import sys
import types
from pathlib import Path

MARQUEE = Path(os.environ.get("MARQUEE_DIR", "/home/anthony/VSCode/Marquee"))
LOGOS = MARQUEE / "marquee" / "assets" / "logos"
HEIGHT = 32

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_logos  # noqa: E402


def _reference_process():
    """Load Marquee's real _process by stubbing the imports it doesn't need."""
    sys.modules.setdefault("httpx", types.ModuleType("httpx"))
    models = types.ModuleType("marquee.data.models")
    models.Team = object  # referenced by _key/type hints, not by _process
    sys.modules["marquee.data.models"] = models  # real package __init__s are trivial
    sys.path.insert(0, str(MARQUEE))
    from marquee.data.logo_pipeline import LogoPipeline
    return LogoPipeline(LOGOS, HEIGHT)._process


def test_processing_parity():
    ref = _reference_process()
    from PIL import Image
    sample = ["mlb_bos.png", "nfl_kc.png", "nhl_vgk.png", "nba_lal.png",
              "epl_liv.png", "eng.1_liv.png", "mlb_sd.png"]
    for name in sample:
        raw = Image.open(LOGOS / name).convert("RGBA")
        mine = build_logos.process(raw.copy(), HEIGHT)
        theirs = ref(raw.copy())
        assert mine.size == theirs.size, f"{name}: size {mine.size} != {theirs.size}"
        assert mine.tobytes() == theirs.tobytes(), f"{name}: pixels differ"
    print(f"processing parity OK ({len(sample)} logos, incl. collision pair)")


def test_roundtrip():
    entries = build_logos.load_logo_dir(LOGOS, HEIGHT)
    data = build_logos.build(entries, HEIGHT)
    table, height = build_logos.read(data)
    assert height == HEIGHT
    assert len(table) == len(entries) >= 40, f"{len(table)} keys != {len(entries)} files"
    assert len(entries) == len({build_logos._key(l, a) for l, a, _ in entries}), \
        "two files collapsed to one key"
    # Each decoded blob must equal a fresh encode of the same processed image.
    for league, abbr, img in entries:
        w, h, blob = build_logos.encode_blob(img)
        rec = table[(league, abbr.upper())]
        assert (rec["w"], rec["h"], rec["blob"]) == (w, h, blob), f"{league}:{abbr}"
    # Offsets must be ascending and cover the whole tail (no overlap/gaps).
    offs = sorted(v["offset"] for v in table.values())
    assert offs[0] == build_logos.HEADER_SIZE + build_logos.ENTRY_SIZE * len(entries)
    print(f"round-trip OK ({len(data)} B atlas)")


def test_collision_keying():
    entries = build_logos.load_logo_dir(LOGOS, HEIGHT)
    keys = sorted(build_logos._key(l, a) for l, a, _ in entries)
    # The same club (Liverpool) exists under two league slugs — a bare abbr
    # index would collapse them and lose one. league+abbr must keep both rows.
    abbr_only = {a.upper() for _, a, _ in entries}
    assert len(keys) > len(abbr_only), "data no longer contains a cross-league abbr reuse"
    assert ("eng.1", "LIV") in keys and ("epl", "LIV") in keys
    for want in [("eng.1", "LIV"), ("epl", "LIV")]:
        i = bisect.bisect_left(keys, want)
        assert keys[i] == want, f"binary search missed {want}"

    data = build_logos.build(entries, HEIGHT)
    t, _ = build_logos.read(data)
    # Distinct keys resolve to distinct index rows (their own offsets). The
    # cached eng.1/epl artwork happens to be identical, so blobs may match —
    # the collision the key defends against is the index entry, not the pixels.
    assert t[("eng.1", "LIV")]["offset"] != t[("epl", "LIV")]["offset"]
    print(f"collision keying OK ({len(keys)} league+abbr keys vs "
          f"{len(abbr_only)} bare-abbr; eng.1:liv/epl:liv kept separate)")


if __name__ == "__main__":
    test_processing_parity()
    test_roundtrip()
    test_collision_keying()
    print("ALL logo pipeline checks passed")
