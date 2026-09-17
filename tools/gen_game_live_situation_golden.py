#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
#
# T-6.6 — golden MLB live cards with situation overlays.
#
# Reproduces game_strip._render_card_live() plus indicators.draw_indicator()
# for the no-logo fallback path, avoiding Marquee's driver import.

from __future__ import annotations

import sys
from dataclasses import dataclass
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

REPO = Path(__file__).resolve().parent.parent
MARQUEE = Path("/home/anthony/VSCode/Marquee")
OUT_HEADER = REPO / "test" / "test_native" / "golden_game_situation.h"
OUT_DIR = REPO / "test" / "out"

CARD_W = 64
PANEL_H = 32
LOGO_H = 13
AWAY = ("KC", "e4393c", 2)
HOME = ("LAR", "22a7de", 4)

WHITE = (255, 255, 255)
DIM = (140, 140, 140)
ACCENT = (255, 210, 0)
ARROW = (210, 210, 210)
INNING = (150, 150, 150)
COUNT = (255, 255, 255)
TRI_W = 5
TRI_H = 3


@dataclass(frozen=True)
class Situation:
    on_first: bool
    on_second: bool
    on_third: bool
    balls: int | None
    strikes: int | None
    outs: int | None


@dataclass(frozen=True)
class CardCase:
    name: str
    title: str
    period: int | None
    sit: Situation | None


def q565(rgb):
    r, g, b = rgb
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def hex_to_rgb(color: str):
    c = color.lstrip("#").zfill(6)
    return (int(c[0:2], 16), int(c[2:4], 16), int(c[4:6], 16))


def draw_diamond(draw, x0, y0, width, height, sit, accent=ACCENT):
    cx = x0 + width // 2
    y_top = y0 + height * 14 // 32
    y_mid = y0 + height * 20 // 32
    spread = max(1, width * 5 // 64)
    bases = {
        "second": (cx, y_top),
        "first": (cx + spread, y_mid),
        "third": (cx - spread, y_mid),
    }
    occupied = {
        "second": bool(sit.on_second),
        "first": bool(sit.on_first),
        "third": bool(sit.on_third),
    }
    r = 2
    for base, (bx, by) in bases.items():
        outer = [(bx, by - r), (bx + r, by), (bx, by + r), (bx - r, by)]
        draw.polygon(outer, outline=DIM)
        if occupied[base]:
            inner = [(bx, by - r + 1), (bx + r - 1, by), (bx, by + r - 1), (bx - r + 1, by)]
            draw.polygon(inner, fill=accent)


def draw_outs(draw, cx, y, sit):
    if sit is None or sit.outs is None:
        return
    r = 2
    for i, dx in enumerate([cx - 5, cx + 2]):
        draw.ellipse([dx - r, y - r, dx + r, y + r], outline=DIM)
        if i < sit.outs:
            draw.rectangle([dx - 1, y - 1, dx + 1, y + 1], fill=ACCENT)


def draw_inning(draw, x, y, inning, inning_top):
    if inning_top is None and inning is None:
        return
    tri_cx = x + TRI_W // 2
    tri_y = y + (8 - TRI_H) // 2
    if inning_top is True:
        draw.polygon(
            [(tri_cx, tri_y), (tri_cx - TRI_W // 2, tri_y + TRI_H), (tri_cx + TRI_W // 2, tri_y + TRI_H)],
            fill=ARROW,
        )
    elif inning_top is False:
        draw.polygon(
            [(tri_cx - TRI_W // 2, tri_y), (tri_cx + TRI_W // 2, tri_y), (tri_cx, tri_y + TRI_H)],
            fill=ARROW,
        )
    if inning is not None:
        draw.text((x + TRI_W + 2, y), str(inning), font=load_small(), fill=INNING)


def draw_count(draw, x_right, y, sit):
    if sit is None or sit.balls is None or sit.strikes is None:
        return
    count_str = f"{sit.balls}-{sit.strikes}"
    cw = int(draw.textlength(count_str, font=load_small()))
    draw.text((x_right - cw, y), count_str, font=load_small(), fill=COUNT)


def draw_mlb_situation(draw, game, sit):
    draw_diamond(draw, 0, 0, CARD_W, PANEL_H, sit)
    cx = CARD_W // 2
    draw_outs(draw, cx, PANEL_H * 26 // 32, sit)
    info_y = PANEL_H * 24 // 32
    low = game["status_display"].lower()
    inning_top = True if "top" in low else False if ("bot" in low or "bottom" in low) else None
    draw_inning(draw, 2, info_y, game["period"], inning_top)
    draw_count(draw, CARD_W - 1, info_y, sit)


_font_cache = {}


def load_small():
    if "small" not in _font_cache:
        _font_cache["small"] = ImageFont.load(str(MARQUEE / "marquee/matrix/fonts/spleen-5x8.pil"))
    return _font_cache["small"]


def load_score():
    if "score" not in _font_cache:
        _font_cache["score"] = ImageFont.load(str(MARQUEE / "marquee/matrix/fonts/spleen-6x12.pil"))
    return _font_cache["score"]


def draw_card(case: CardCase):
    card = Image.new("RGB", (CARD_W, PANEL_H), (0, 0, 0))
    draw = ImageDraw.Draw(card)
    small = load_small()
    score_font = load_score()
    score_y = max(0, (LOGO_H - 12) // 2)

    for abbr, color, score, align in ((AWAY[0], AWAY[1], AWAY[2], "left"), (HOME[0], HOME[1], HOME[2], "right")):
        lw = int(draw.textlength(abbr, font=small)) + 1
        if align == "left":
            draw.text((0, max(0, (LOGO_H - 8) // 2)), abbr, font=small, fill=hex_to_rgb(color))
            draw.text((lw + 2, score_y), str(score), font=score_font, fill=WHITE)
        else:
            draw.text((CARD_W - lw, max(0, (LOGO_H - 8) // 2)), abbr, font=small, fill=hex_to_rgb(color))
            sw = int(draw.textlength(str(score), font=score_font))
            draw.text((CARD_W - lw - 2 - sw, score_y), str(score), font=score_font, fill=WHITE)

    if case.sit is None:
        return card

    game = {"status_display": case.title, "period": case.period}
    draw_mlb_situation(draw, game, case.sit)
    return card


def grid_for(card):
    px = card.load()
    return [q565(px[x, y]) for y in range(PANEL_H) for x in range(CARD_W)]


def array_text(name, grid):
    rows = []
    for y in range(PANEL_H):
        cells = ", ".join(f"0x{grid[y * CARD_W + x]:04X}" for x in range(CARD_W))
        rows.append(f"    {cells},")
    return f"inline constexpr uint16_t {name}[{CARD_W * PANEL_H}] = {{\n" + "\n".join(rows) + "\n};\n"


def save_png(path, grid):
    img = Image.new("RGB", (CARD_W, PANEL_H))
    ip = img.load()
    for y in range(PANEL_H):
        for x in range(CARD_W):
            v = grid[y * CARD_W + x]
            r5 = (v >> 11) & 31
            g6 = (v >> 5) & 63
            b5 = v & 31
            ip[x, y] = ((r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2))
    img.save(path)


def main() -> int:
    if not (MARQUEE / "marquee/matrix/fonts").exists():
        print(f"missing {MARQUEE / 'marquee/matrix/fonts'}", file=sys.stderr)
        return 1

    cases = [
        CardCase("MISSING", "Top 3rd", 3, None),
        CardCase("EMPTY", "Top 3rd", 3, Situation(False, False, False, 2, 1, 2)),
        CardCase("PARTIAL", "Bot 5th", 5, Situation(True, False, True, 1, 2, 1)),
        CardCase("LOADED", "Mid 7th", 7, Situation(True, True, True, 3, 2, 0)),
    ]

    OUT_HEADER.parent.mkdir(parents=True, exist_ok=True)
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    out = [
        "// SPDX-License-Identifier: GPL-3.0-only\n"
        "//\n"
        "// GENERATED by tools/gen_game_live_situation_golden.py — golden MLB live cards (T-6.6).\n"
        f"// RGB565 grids {CARD_W}x{PANEL_H}; fallback '{AWAY[0]}' vs '{HOME[0]}'.\n"
        "#pragma once\n"
        "#include <cstdint>\n"
        "inline constexpr int GOLDEN_GAME_SIT_W = 64, GOLDEN_GAME_SIT_H = 32;\n"
    ]
    for case in cases:
        grid = grid_for(draw_card(case))
        out.append(array_text(f"GOLDEN_GAME_SIT_{case.name}", grid))
        save_png(OUT_DIR / f"golden_game_sit_{case.name.lower()}.png", grid)

    OUT_HEADER.write_text("".join(out))
    print(f"golden_game_situation.h ({len(cases)} cards) + PNGs written")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
