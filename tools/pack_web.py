#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Gzip the SPA into PlatformIO's data/ tree for the web partition."""

import gzip
from pathlib import Path

ROOT = Path(globals().get("PROJECT_DIR", Path.cwd()))
SRC = ROOT / "assets" / "web" / "index.html"
DEST = ROOT / "data" / "index.html.gz"


def pack():
    raw = SRC.read_bytes()
    compressed = gzip.compress(raw, mtime=0)
    DEST.parent.mkdir(parents=True, exist_ok=True)
    DEST.write_bytes(compressed)
    print(
        "[web] packed "
        f"{SRC.relative_to(ROOT)} -> {DEST.relative_to(ROOT)}: "
        f"{len(raw)} -> {len(compressed)} B ({len(compressed) * 100.0 / len(raw):.1f}%)"
    )


pack()