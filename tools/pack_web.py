#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Gzip the SPA into PlatformIO's data/ tree for the web partition."""

import gzip
from pathlib import Path

ROOT = Path(globals().get("PROJECT_DIR", Path.cwd()))
PAGES = ("index.html", "onboard.html")  # onboard.html: T-8.7, self-contained CSS


def pack():
    for name in PAGES:
        src = ROOT / "assets" / "web" / name
        dest = ROOT / "data" / (name + ".gz")
        raw = src.read_bytes()
        compressed = gzip.compress(raw, mtime=0)
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(compressed)
        print(
            "[web] packed "
            f"{src.relative_to(ROOT)} -> {dest.relative_to(ROOT)}: "
            f"{len(raw)} -> {len(compressed)} B ({len(compressed) * 100.0 / len(raw):.1f}%)"
        )


pack()