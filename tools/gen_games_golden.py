#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""T-5.5 — generate golden files: run the Marquee's OWN norm_game() over
each fixture and record the result; C++ to_games() must match field for
field. Marquee is imported read-only (design reference, never modified).
logo_url/league are dropped — on nosebleed the atlas key (league+abbr) and
the cache slot replace them. Run: python3 tools/gen_games_golden.py
"""
import json
import os
import pathlib
import sys
import types

MARQUEE = pathlib.Path(os.environ.get("MARQUEE_REPO", "/home/anthony/VSCode/Marquee"))
sys.path.insert(0, str(MARQUEE))

httpx = types.ModuleType("httpx")  # espn_client imports it at module level
httpx.AsyncClient = object  # never instantiated here
sys.modules["httpx"] = httpx

from marquee.data.espn_client import norm_game  # noqa: E402

FIX = pathlib.Path(__file__).resolve().parent.parent / "test" / "fixtures"
OUT = FIX.parent / "golden"
OUT.mkdir(exist_ok=True)


def team(t):
    return {"id": t.id, "name": t.name, "abbr": t.abbreviation, "colour": t.color}


def sit(s):
    if s is None:
        return None
    return {
        "on_first": s.on_first, "on_second": s.on_second, "on_third": s.on_third,
        "balls": s.balls, "strikes": s.strikes, "outs": s.outs,
        "down": s.down, "distance": s.distance, "yard_line": s.yard_line,
        "possession": s.possession, "is_red_zone": s.is_red_zone,
    }


def game(g):
    return {
        "id": g.id, "status": g.status, "status_display": g.status_display,
        "period": g.period, "clock": g.clock,
        "away": team(g.away), "home": team(g.home),
        "away_score": g.away_score, "home_score": g.home_score,
        "start_utc": int(g.start_time.timestamp()), "situation": sit(g.situation),
    }


for fx in sorted(FIX.glob("*.json")):
    events = json.loads(fx.read_text())["events"]
    gold = [game(norm_game(ev, fx.stem)) for ev in events[:16]]
    (OUT / f"{fx.stem}.json").write_text(json.dumps(gold, indent=1, sort_keys=True) + "\n")
    print(f"{fx.stem}: {len(gold)} games")
