#!/usr/bin/env python3
"""
twatch-s21 — Squared-Corner Omarchy TUI Interface for Galaxy S21 <-> LilyGO T-Watch S3.
Runs inside Termux or as an Omalauncher Wayland widget (`foot --app-id=omarchy.twatch -e twatch-s21`).
"""

from __future__ import annotations

import curses
import json
import pathlib
import sys
import time
import urllib.request

BLOCKS = " ▁▂▃▄▅▆▇█"
API = "http://127.0.0.1:8798"
STATE_FILE = pathlib.Path.home() / ".local" / "state" / "twatch-s21" / "state.json"

SCHEMES = [
    ("1", "everforest", "Everforest (#7FBBB3)"),
    ("2", "starship",   "Starship (#FF9F0A)"),
    ("3", "tokyonight", "Tokyo Night (#7AA2F7)"),
    ("4", "catppuccin", "Catppuccin (#CBA6F7)"),
    ("5", "gruvbox",    "Gruvbox (#D79921)"),
    ("6", "kanagawa",   "Kanagawa (#DCA561)"),
    ("7", "rosepine",   "Rose Pine (#EBBCBA)"),
    ("8", "mattemono",  "Matte Mono (#FFFFFF)"),
]


def fetch_state() -> dict:
    try:
        with urllib.request.urlopen(f"{API}/status", timeout=0.8) as r:
            return json.loads(r.read().decode("utf-8"))
    except Exception:
        if STATE_FILE.is_file():
            try:
                return json.loads(STATE_FILE.read_text(encoding="utf-8"))
            except Exception:
                pass
    return {"connected": False, "transport": "bridge-offline", "rssi": -99, "rssi_history": [], "scheme": "?"}


def post_api(path: str, payload: dict) -> str:
    req = urllib.request.Request(
        f"{API}{path}",
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=1.5) as r:
            return f"OK ({r.status})"
    except Exception as exc:
        return f"ERR: {exc}"


def sparkline(vals: list[int], width: int) -> str:
    if width <= 0:
        return ""
    tail = vals[-width:]
    pad = [-95] * max(0, width - len(tail)) + list(tail)
    out = []
    for rssi in pad:
        # Map -95..-40 dBm -> 0..8
        norm = max(0.0, min(1.0, (rssi + 95.0) / 55.0))
        out.append(BLOCKS[int(round(norm * 8))])
    return "".join(out)


def run_tui(stdscr: curses.window) -> None:
    curses.curs_set(0)
    stdscr.nodelay(True)
    stdscr.timeout(700)
    if curses.has_colors():
        curses.start_color()
        curses.use_default_colors()
        curses.init_pair(1, curses.COLOR_CYAN, -1)
        curses.init_pair(2, curses.COLOR_GREEN, -1)
        curses.init_pair(3, curses.COLOR_YELLOW, -1)

    msg = "Ready — [1-8] Color Scheme · [p] Ping Watch · [t] Sync Clock · [q] Quit"
    while True:
        st = fetch_state()
        h, w = stdscr.getmaxyx()
        stdscr.erase()

        c_acc = curses.color_pair(1) | curses.A_BOLD
        c_ok = curses.color_pair(2) | curses.A_BOLD
        c_warn = curses.color_pair(3) | curses.A_BOLD

        def put(y: int, x: int, s: str, attr: int = 0) -> None:
            if 0 <= y < h and x < w:
                try:
                    stdscr.addstr(y, x, s[: max(0, w - x - 1)], attr)
                except curses.error:
                    pass

        put(0, 0, "┌─ ■ S21 ⇄ T-WATCH S3 COMPANION " + "─" * max(0, w - 34) + "┐", c_acc)
        for ry in range(1, h - 1):
            put(ry, 0, "│", c_acc)
            put(ry, w - 2, "│", c_acc)
        put(h - 1, 0, "└" + "─" * max(0, w - 3) + "┘", c_acc)

        conn = bool(st.get("connected"))
        badge = "■ CONNECTED (BLE NUS)" if conn else f"□ {st.get('transport', 'scanning')}"
        put(1, 2, f"Link: {badge}", c_ok if conn else c_warn)
        put(2, 2, f"Peer: {st.get('watch_name', 'Bangle.js oma')}  [{st.get('address', '-')}]  RSSI: {st.get('rssi', -99)} dBm")
        put(3, 2, f"Last seen: {st.get('last_seen') or '-'}   Active scheme: {st.get('scheme', '-')}")

        put(4, 0, "├" + "─" * max(0, w - 3) + "┤", c_acc)
        put(5, 2, "BLE Signal Graph (-95 to -40 dBm):", c_acc)
        put(6, 2, sparkline(st.get("rssi_history") or [], max(10, w - 6)), c_ok if conn else c_warn)

        put(7, 0, "├" + "─" * max(0, w - 3) + "┤", c_acc)
        put(8, 2, "PUSH COLOR SCHEME TO WATCH:", c_acc)
        for idx, (k, slug, label) in enumerate(SCHEMES):
            put(9 + idx, 3, f"[{k}] {label}")

        put(18, 2, "[p] Send Test Ping Notification   [t] Sync S21 Clock → Watch   [q] Quit", c_warn)
        put(19, 2, f"Last Watch Event: {st.get('last_watch_event') or 'none'}")
        put(h - 2, 2, f"> {msg}", c_acc)
        stdscr.refresh()

        ch = stdscr.getch()
        if ch == -1:
            continue
        if ch in (27, ord("q"), ord("Q")):
            break
        if ch in (ord("p"), ord("P")):
            msg = "Ping → " + post_api("/notify", {"src": "S21", "title": "BORA S21", "body": "Link active!"})
        elif ch in (ord("t"), ord("T")):
            msg = "Clock Sync → " + post_api("/sync-clock", {})
        elif ord("1") <= ch <= ord("8"):
            slug = SCHEMES[ch - ord("1")][1]
            msg = f"Scheme {slug} → " + post_api("/scheme", {"scheme": slug})


if __name__ == "__main__":
    if "--status" in sys.argv:
        print(json.dumps(fetch_state(), indent=2))
        sys.exit(0)
    curses.wrapper(run_tui)
