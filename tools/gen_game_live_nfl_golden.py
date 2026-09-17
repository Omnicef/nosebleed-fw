#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
#
# T-6.7 — golden NFL live cards with gridiron field strip.
#
# Reproduces game_strip._render_card_live_nfl() plus gridiron.draw_gridiron()
# for the no-logo fallback path, avoiding Marquee's driver import.

from __future__ import annotations

import sys
from dataclasses import dataclass
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

REPO = Path(__file__).resolve().parent.parent
MARQUEE = Path("/home/anthony/VSCode/Marquee")
OUT_HEADER = REPO / "test" / "test_native" / "golden_game_live_nfl.h"
OUT_DIR = REPO / "test" / "out"

CARD_W = 64
PANEL_H = 32
NFL_LOGO_H = 12
FIELD_STRIP_H = 8
GOALPOST_W = 5
GOALPOST_H = 5

AWAY_ID = "12"
HOME_ID = "34"
AWAY_ABBR = "KC"
HOME_ABBR = "LAR"
AWAY_COLOR = "e4393c"
HOME_COLOR = "22a7de"
AWAY_SCORE = 7
HOME_SCORE = 10
PERIOD = 2
CLOCK = "12:34"

WHITE = (255, 255, 255)
DIM = (110, 110, 110)
BROWN = (150, 80, 30)
GRASS_LINE = (0, 130, 0)
GOALPOST = (255, 220, 0)
NFL_ORDINALS = {1: "1ST", 2: "2ND", 3: "3RD", 4: "4TH"}


@dataclass(frozen=True)
class Situation:
    down: int | None
    distance: int | None
    yard_line: int | None
    possession: str | None
    is_red_zone: bool = False


@dataclass(frozen=True)
class CardCase:
    name: str
    has_situation: bool
    sit: Situation | None


_font_cache: dict[str, ImageFont.ImageFont] = {}


def q565(rgb):
    r, g, b = rgb
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def hex_to_rgb(color: str):
    c = color.lstrip("#").zfill(6)
    return (int(c[0:2], 16), int(c[2:4], 16), int(c[4:6], 16))


def load_small() -> ImageFont.ImageFont:
    if "small" not in _font_cache:
        _font_cache["small"] = ImageFont.load(str(MARQUEE / "marquee/matrix/fonts/spleen-5x8.pil"))
    return _font_cache["small"]


def paste_logo(draw: ImageDraw.Draw, x: int, y: int, logo_h: int, abbr: str, color: str, align: str) -> int:
    font = load_small()
    text = abbr[:3]
    lw = int(draw.textlength(text, font=font)) + 1
    paste_x = x if align == "left" else x - lw
    paste_y = y + max(0, (logo_h - 8) // 2)
    draw.text((paste_x, paste_y), text, font=font, fill=hex_to_rgb(color))
    return lw


def draw_possession_football(draw: ImageDraw.Draw, x: int, y: int) -> None:
    draw.ellipse([x, y, x + 5, y + 3], fill=BROWN)
    draw.line([(x + 2, y + 1), (x + 2, y + 2)], fill=WHITE)


def goalpost(draw: ImageDraw.Draw, x_base: int, y_base: int) -> None:
    for dy in range(3):
        draw.point((x_base + 1, y_base + dy), fill=GOALPOST)
        draw.point((x_base + 3, y_base + dy), fill=GOALPOST)
    for dx in range(1, 4):
        draw.point((x_base + dx, y_base + 2), fill=GOALPOST)
    for dy in range(3, GOALPOST_H):
        draw.point((x_base + 2, y_base + dy), fill=GOALPOST)


def draw_gridiron(draw: ImageDraw.Draw, x0: int, y0: int, width: int, height: int, sit: Situation) -> None:
    if height < 2 or width < 2 * GOALPOST_W + 1:
        return

    bottom = y0 + height - 1
    draw.line([(x0, bottom), (x0 + width - 1, bottom)], fill=GRASS_LINE)
    goalpost(draw, x0, y0)
    goalpost(draw, x0 + width - GOALPOST_W, y0)

    if sit.yard_line is None:
        return

    yl = max(0, min(100, sit.yard_line))
    field_left = x0 + GOALPOST_W
    field_right = x0 + width - 1 - GOALPOST_W
    if field_right <= field_left:
        return

    ball_x = field_left + int(yl / 100.0 * (field_right - field_left))
    ball_x = max(field_left, min(field_right, ball_x))
    ball_y = bottom - 2
    draw.ellipse([ball_x - 2, ball_y - 1, ball_x + 2, ball_y + 1], fill=BROWN)


def quarter_str(period: int | None) -> str:
    if period is None:
        return ""
    if period <= 4:
        return NFL_ORDINALS.get(period, f"{period}TH")
    if period == 5:
        return "OT"
    return f"{period - 4}OT"


def down_distance_str(sit: Situation) -> str:
    if sit.down is None:
        return ""
    d = NFL_ORDINALS.get(sit.down, f"{sit.down}TH")
    if sit.distance is not None:
        return f"{d}&{sit.distance}"
    return d


def render_card(case: CardCase) -> Image.Image:
    card = Image.new("RGB", (CARD_W, PANEL_H), (0, 0, 0))
    draw = ImageDraw.Draw(card)
    font = load_small()
    sit = case.sit if case.has_situation else None

    away_lw = paste_logo(draw, 0, 0, NFL_LOGO_H, AWAY_ABBR, AWAY_COLOR, "left")
    away_str = str(AWAY_SCORE)
    score_y_away = max(0, (NFL_LOGO_H - 8) // 2)
    draw.text((away_lw + 2, score_y_away), away_str, font=font, fill=WHITE)
    asw = int(draw.textlength(away_str, font=font))
    if sit is not None and sit.possession == AWAY_ID:
        draw_possession_football(draw, away_lw + 2 + asw + 1, score_y_away)

    home_lw = paste_logo(draw, 0, NFL_LOGO_H, NFL_LOGO_H, HOME_ABBR, HOME_COLOR, "left")
    home_str = str(HOME_SCORE)
    score_y_home = NFL_LOGO_H + max(0, (NFL_LOGO_H - 8) // 2)
    draw.text((home_lw + 2, score_y_home), home_str, font=font, fill=WHITE)
    hsw = int(draw.textlength(home_str, font=font))
    if sit is not None and sit.possession == HOME_ID:
        draw_possession_football(draw, home_lw + 2 + hsw + 1, score_y_home)

    rx = CARD_W - 1
    ry = 0
    q = quarter_str(PERIOD)
    if q:
        qw = int(draw.textlength(q, font=font))
        draw.text((rx - qw, ry), q, font=font, fill=DIM)
    ry += 8

    if CLOCK:
        cw = int(draw.textlength(CLOCK, font=font))
        draw.text((rx - cw, ry), CLOCK, font=font, fill=WHITE)
    ry += 8

    if sit is not None:
        dd = down_distance_str(sit)
        if dd:
            ddw = int(draw.textlength(dd, font=font))
            draw.text((rx - ddw, ry), dd, font=font, fill=WHITE)

    if sit is not None:
        field_y = PANEL_H - FIELD_STRIP_H
        if field_y >= 0:
            draw_gridiron(draw, 0, field_y, CARD_W, FIELD_STRIP_H, sit)

    return card


def grid_for(card: Image.Image):
    px = card.load()
    return [q565(px[x, y]) for y in range(PANEL_H) for x in range(CARD_W)]


def array_text(name: str, grid):
    rows = []
    for y in range(PANEL_H):
        cells = ", ".join(f"0x{grid[y * CARD_W + x]:04X}" for x in range(CARD_W))
        rows.append(f"    {cells},")
    return f"inline constexpr uint16_t {name}[{CARD_W * PANEL_H}] = {{\n" + "\n".join(rows) + "\n};\n"


def save_png(path: Path, grid) -> None:
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
        CardCase("AWAY", True, Situation(1, 10, 25, AWAY_ID)),
        CardCase("HOME", True, Situation(1, 10, 25, HOME_ID)),
        CardCase("NOSIT", False, None),
        CardCase("REDZONE", True, Situation(2, 7, 85, AWAY_ID, True)),
    ]

    OUT_HEADER.parent.mkdir(parents=True, exist_ok=True)
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    out = [
        "// SPDX-License-Identifier: GPL-3.0-only\n"
        "//\n"
        "// GENERATED by tools/gen_game_live_nfl_golden.py — golden NFL live cards (T-6.7).\n"
        f"// RGB565 grids {CARD_W}x{PANEL_H}; fallback '{AWAY_ABBR}' vs '{HOME_ABBR}'.\n"
        "#pragma once\n"
        "#include <cstdint>\n"
        "inline constexpr int GOLDEN_GAME_LIVE_NFL_W = 64, GOLDEN_GAME_LIVE_NFL_H = 32;\n"
    ]
    for case in cases:
        grid = grid_for(render_card(case))
        out.append(array_text(f"GOLDEN_GAME_LIVE_NFL_{case.name}", grid))
        save_png(OUT_DIR / f"golden_game_live_nfl_{case.name.lower()}.png", grid)

    OUT_HEADER.write_text("".join(out))
    print(f"golden_game_live_nfl.h ({len(cases)} cards) + PNGs written")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
