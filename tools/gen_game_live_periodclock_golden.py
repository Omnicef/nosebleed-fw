#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
#
# T-6.8 — golden period/clock live cards for NHL, NBA and soccer leagues.
#
# Reproduces game_strip._render_card_live_periodclock() for the no-logo
# fallback path, avoiding Marquee's driver import.

from __future__ import annotations

import sys
from dataclasses import dataclass
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

REPO = Path(__file__).resolve().parent.parent
MARQUEE = Path("/home/anthony/VSCode/Marquee")
OUT_HEADER = REPO / "test" / "test_native" / "golden_game_live_periodclock.h"
OUT_DIR = REPO / "test" / "out"

CARD_W = 64
PANEL_H = 32
LOGO_H = 30

WHITE = (255, 255, 255)
DIM = (110, 110, 110)
OUTLINE = (0, 0, 0)
NBA_COLLEGES = {"nba", "mens-college-basketball", "womens-college-basketball"}

_font_cache: dict[str, ImageFont.ImageFont] = {}


@dataclass(frozen=True)
class Game:
    league: str
    away_abbr: str
    home_abbr: str
    away_color: str
    home_color: str
    away_score: int
    home_score: int
    period: int | None
    clock: str
    status_display: str


@dataclass(frozen=True)
class CardCase:
    name: str
    game: Game


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


def load_score() -> ImageFont.ImageFont:
    if "score" not in _font_cache:
        _font_cache["score"] = ImageFont.load(str(MARQUEE / "marquee/matrix/fonts/spleen-6x12.pil"))
    return _font_cache["score"]


def draw_outlined(draw: ImageDraw.Draw, x: int, y: int, text: str, font, fill) -> None:
    for dx in (-1, 0, 1):
        for dy in (-1, 0, 1):
            if dx or dy:
                draw.text((x + dx, y + dy), text, font=font, fill=OUTLINE)
    draw.text((x, y), text, font=font, fill=fill)


def paste_logo_bleed(draw: ImageDraw.Draw, side: str, abbr: str, color: str) -> None:
    font = load_small()
    y = (PANEL_H - LOGO_H) // 2
    text_y = y + max(0, (LOGO_H - 8) // 2)
    abbr = abbr[:3]
    if side == "left":
        draw.text((1, text_y), abbr, font=font, fill=hex_to_rgb(color))
    else:
        tw = int(draw.textlength(abbr, font=font))
        draw.text((CARD_W - tw - 1, text_y), abbr, font=font, fill=hex_to_rgb(color))


def nhl_period(period: int | None) -> str:
    if period is None:
        return ""
    labels = {1: "P1", 2: "P2", 3: "P3", 4: "OT", 5: "SO"}
    return labels.get(period, f"OT{period - 3}")


def nba_period(period: int | None) -> str:
    if period is None:
        return ""
    if period <= 4:
        return f"Q{period}"
    if period == 5:
        return "OT"
    return f"{period - 4}OT"


def soccer_period(period: int | None, status_display: str | None) -> str:
    sd = (status_display or "").lower()
    if "halftime" in sd or "half time" in sd:
        return "HT"
    if period == 1:
        return "1ST"
    if period == 2:
        return "2ND"
    if period is not None:
        return f"P{period}"
    return ""


def soccer_clock(game: Game) -> str:
    if game.clock:
        c = game.clock.strip()
        return c if c.endswith("'") else c + "'"
    digits = "".join(ch for ch in (game.status_display or "") if ch.isdigit())[:3]
    return digits + "'" if digits else ""


def format_period_clock(game: Game) -> tuple[str, str]:
    if game.league == "nhl":
        return nhl_period(game.period), game.clock or ""
    if game.league in NBA_COLLEGES:
        return nba_period(game.period), game.clock or ""
    return soccer_period(game.period, game.status_display), soccer_clock(game)


def render_card(case: CardCase) -> Image.Image:
    game = case.game
    card = Image.new("RGB", (CARD_W, PANEL_H), (0, 0, 0))
    draw = ImageDraw.Draw(card)
    score_font = load_score()
    small_font = load_small()

    paste_logo_bleed(draw, "left", game.away_abbr, game.away_color)
    paste_logo_bleed(draw, "right", game.home_abbr, game.home_color)

    score_text = f"{game.away_score}-{game.home_score}"
    sw = int(draw.textlength(score_text, font=score_font))
    score_y = max(0, (PANEL_H // 4) - 4)
    draw_outlined(draw, (CARD_W - sw) // 2, score_y, score_text, score_font, WHITE)

    period_str, clock_str = format_period_clock(game)
    if period_str:
        pw = int(draw.textlength(period_str, font=small_font))
        period_y = PANEL_H * 18 // 32
        draw_outlined(draw, (CARD_W - pw) // 2, period_y, period_str, small_font, DIM)

    if clock_str:
        cw = int(draw.textlength(clock_str, font=small_font))
        clock_y = PANEL_H * 24 // 32
        draw_outlined(draw, (CARD_W - cw) // 2, clock_y, clock_str, small_font, WHITE)

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
        CardCase("NHL", Game("nhl", "LAL", "VGK", "e4393c", "22a7de", 3, 2, 3, "14:22", "3rd")),
        CardCase("NBA", Game("nba", "BOS", "LAL", "22a7de", "e4393c", 101, 98, 5, "2:30", "OT")),
        CardCase("SOCCER", Game("epl", "ARS", "CHE", "e4393c", "22a7de", 1, 0, 2, "", "67'")),
        CardCase("HALFTIME", Game("eng.1", "LIV", "MCI", "e4393c", "22a7de", 2, 2, 2, "", "HALF TIME")),
    ]

    OUT_HEADER.parent.mkdir(parents=True, exist_ok=True)
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    out = [
        "// SPDX-License-Identifier: GPL-3.0-only\n"
        "//\n"
        "// GENERATED by tools/gen_game_live_periodclock_golden.py — golden period/clock live cards (T-6.8).\n"
        f"// RGB565 grids {CARD_W}x{PANEL_H}; no-logo fallback abbreviations.\n"
        "#pragma once\n"
        "#include <cstdint>\n"
        "inline constexpr int GOLDEN_GAME_LIVE_PERIODCLOCK_W = 64, GOLDEN_GAME_LIVE_PERIODCLOCK_H = 32;\n"
    ]
    for case in cases:
        grid = grid_for(render_card(case))
        out.append(array_text(f"GOLDEN_GAME_LIVE_PERIODCLOCK_{case.name}", grid))
        save_png(OUT_DIR / f"golden_game_live_periodclock_{case.name.lower()}.png", grid)

    OUT_HEADER.write_text("".join(out))
    print(f"golden_game_live_periodclock.h ({len(cases)} cards) + PNGs written")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
