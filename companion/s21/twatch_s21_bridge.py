#!/usr/bin/env python3
"""
twatch_s21_bridge.py — S21 <-> LilyGO T-Watch S3 BLE Companion Bridge & Plugin API.

Runs on the Galaxy S21 (Termux / Python) to:
  1. Maintain BLE NUS connection to 'Bangle.js oma' (98:C3:77:AC:23:98 / 6E400001-...)
     via bleak (or coexist with Gadgetbridge intents if Gadgetbridge holds GATT).
  2. Track live watch presence/RSSI in ~/.local/state/twatch-s21/state.json and
     surface an Android notification so the S21 always knows watch link state.
  3. Sync clock/timezone on connect, handle watch->phone media & findPhone events,
     and serve a local plugin HTTP API on 127.0.0.1:8798 (/status, /notify, /scheme).
"""

from __future__ import annotations

import asyncio
import collections
import datetime as dt
import json
import os
import pathlib
import shutil
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

STATE_DIR = pathlib.Path.home() / ".local" / "state" / "twatch-s21"
STATE_FILE = STATE_DIR / "state.json"

WATCH_NAME = "Bangle.js oma"
WATCH_MAC_HINT = "98:C3:77:AC:23:99"
NUS_RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # write Phone -> Watch
NUS_TX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # notify Watch -> Phone

SCHEMES = {
    "everforest": ("Everforest", "181D20", "2D353B", "D3C6AA", "475258", "7FBBB3"),
    "starship":   ("starship",   "121518", "1E242B", "E6EDF3", "4E5965", "FF9F0A"),
    "tokyonight": ("TokyoNight", "1A1B26", "24283B", "C0CAF5", "565F89", "7AA2F7"),
    "catppuccin": ("Catppuccin", "1E1E2E", "313244", "CDD6F4", "6C7086", "CBA6F7"),
    "gruvbox":    ("Gruvbox",    "1D2021", "32302F", "EBDBB2", "928374", "D79921"),
    "kanagawa":   ("Kanagawa",   "1F1F28", "2A2A37", "DCD7BA", "727169", "DCA561"),
    "rosepine":   ("RosePine",   "191724", "26233A", "E0DEF4", "6E6A86", "EBBCBA"),
    "mattemono":  ("MatteMono",  "0D0D0D", "1C1C1C", "E6E6E6", "666666", "FFFFFF"),
}

STATE = {
    "connected": False,
    "transport": "ble-scanning",
    "watch_name": WATCH_NAME,
    "address": WATCH_MAC_HINT,
    "rssi": -99,
    "rssi_history": [],
    "scheme": "starship",
    "last_seen": None,
    "last_watch_event": None,
    "tx_queue": [],
}
LOCK = threading.Lock()
RSSI_HIST: collections.deque[int] = collections.deque(maxlen=48)
TX_ASYNC_QUEUE: collections.deque[str] = collections.deque(maxlen=32)


def has_termux_api() -> bool:
    if not shutil.which("termux-notification"):
        return False
    try:
        r = subprocess.run(["pm", "list", "packages"], capture_output=True, text=True, timeout=2.5)
        return "com.termux.api" in r.stdout
    except Exception:
        return False


HAS_API = has_termux_api()


def android_notify(title: str, content: str) -> None:
    if not HAS_API:
        return
    subprocess.run(
        ["timeout", "3", "termux-notification", "--id", "twatch_s21", "--title", title, "--content", content, "--ongoing"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )


def save_state() -> None:
    STATE_DIR.mkdir(parents=True, exist_ok=True)
    with LOCK:
        STATE["rssi_history"] = list(RSSI_HIST)
        tmp = STATE_FILE.with_suffix(".tmp")
        tmp.write_text(json.dumps(STATE, indent=2), encoding="utf-8")
        tmp.replace(STATE_FILE)


def handle_watch_tx(line: str) -> None:
    line = line.strip()
    if not line.startswith("{"):
        return
    try:
        msg = json.loads(line)
    except Exception:
        return
    with LOCK:
        STATE["last_watch_event"] = f"{dt.datetime.now().strftime('%H:%M:%S')} {line}"
    save_state()

    t = msg.get("t")
    if t == "findPhone" and msg.get("n"):
        if HAS_API and shutil.which("termux-vibrate"):
            subprocess.run(["timeout", "3", "termux-vibrate", "-d", "800"], check=False)
        android_notify("⌚ T-Watch Ping!", "Your watch pinged this S21")
    elif t == "music":
        act = msg.get("n", "")
        key = {"play": "85", "pause": "85", "next": "87", "previous": "88", "volumeup": "24", "volumedown": "25"}.get(act)
        if key:
            subprocess.run(["input", "keyevent", key], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)


def queue_line(raw: str) -> None:
    if not raw.endswith("\n"):
        raw += "\n"
    TX_ASYNC_QUEUE.append(raw)


def sync_clock_payload() -> str:
    now = int(time.time())
    tz_off_hr = time.localtime().tm_gmtoff / 3600.0
    return f"setTime({now});E.setTimeZone({tz_off_hr:.2f})\n"


class ApiHandler(BaseHTTPRequestHandler):
    def log_message(self, fmt: str, *args) -> None:
        return

    def _send(self, code: int, payload: dict) -> None:
        raw = json.dumps(payload).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self) -> None:
        if self.path.startswith("/status"):
            with LOCK:
                data = dict(STATE)
                data["rssi_history"] = list(RSSI_HIST)
            self._send(200, data)
            return
        self._send(404, {"error": "not found"})

    def do_POST(self) -> None:
        length = int(self.headers.get("Content-Length") or "0")
        body = json.loads(self.rfile.read(length).decode("utf-8") or "{}") if length else {}
        if self.path == "/notify":
            src = str(body.get("src") or "S21")[:18]
            title = str(body.get("title") or "Ping")[:32]
            text = str(body.get("body") or "Connected to S21")[:72]
            queue_line(f'GB({{"t":"notify","src":"{src}","title":"{title}","body":"{text}"}})')
            self._send(200, {"queued": True})
            return
        if self.path == "/scheme":
            key = str(body.get("scheme") or "").lower().replace(" ", "").replace("-", "")
            if key not in SCHEMES:
                self._send(400, {"error": f"unknown scheme '{key}'", "choices": list(SCHEMES)})
                return
            name, bg, surf, txt, mut, acc = SCHEMES[key]
            queue_line(f"TH {name}|{bg}|{surf}|{txt}|{mut}|{acc}")
            with LOCK:
                STATE["scheme"] = name
            save_state()
            self._send(200, {"queued": True, "scheme": name})
            return
        if self.path == "/sync-clock":
            queue_line(sync_clock_payload())
            self._send(200, {"queued": True})
            return
        self._send(404, {"error": "not found"})


async def ble_loop() -> None:
    try:
        from bleak import BleakClient, BleakScanner
    except ImportError:
        with LOCK:
            STATE["transport"] = "gadgetbridge-intent-mode (pip install bleak for direct GATT)"
        save_state()
        while True:
            await asyncio.sleep(10)

    rx_buf = ""

    def on_notify(_sender: int, data: bytearray) -> None:
        nonlocal rx_buf
        rx_buf += data.decode("utf-8", "replace")
        while "\n" in rx_buf:
            line, rx_buf = rx_buf.split("\n", 1)
            handle_watch_tx(line)

    while True:
        try:
            devices = await BleakScanner.discover(timeout=5.0, return_adv=True)
            target = None
            best_rssi = -99
            for addr, (dev, adv) in devices.items():
                nm = (dev.name or adv.local_name or "").strip()
                if nm == WATCH_NAME or addr.upper() == WATCH_MAC_HINT:
                    target = dev
                    best_rssi = int(adv.rssi or -70)
                    break

            if not target:
                with LOCK:
                    was_conn = STATE["connected"]
                    STATE["connected"] = False
                    STATE["transport"] = "ble-scanning"
                save_state()
                if was_conn:
                    android_notify("⌚ T-Watch S3 Away", "Scanning for Bangle.js oma over BLE...")
                await asyncio.sleep(6.0)
                continue

            async with BleakClient(target) as client:
                with LOCK:
                    STATE["connected"] = True
                    STATE["transport"] = "ble-nus-direct"
                    STATE["address"] = target.address
                    STATE["rssi"] = best_rssi
                    STATE["last_seen"] = dt.datetime.now().isoformat(timespec="seconds")
                RSSI_HIST.append(best_rssi)
                save_state()
                android_notify("⌚ T-Watch S3 Connected", f"BLE link active ({best_rssi} dBm)")

                await client.start_notify(NUS_TX_UUID, on_notify)
                queue_line(sync_clock_payload())

                while client.is_connected:
                    while TX_ASYNC_QUEUE:
                        pkt = TX_ASYNC_QUEUE.popleft().encode("utf-8")
                        for off in range(0, len(pkt), 120):
                            await client.write_gatt_char(NUS_RX_UUID, pkt[off : off + 120], response=False)
                    with LOCK:
                        STATE["last_seen"] = dt.datetime.now().isoformat(timespec="seconds")
                    RSSI_HIST.append(best_rssi)
                    save_state()
                    await asyncio.sleep(1.0)
        except Exception as exc:
            with LOCK:
                STATE["connected"] = False
                STATE["transport"] = f"retry ({type(exc).__name__})"
            save_state()
            await asyncio.sleep(5.0)


def main() -> None:
    save_state()
    srv = ThreadingHTTPServer(("127.0.0.1", 8798), ApiHandler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    asyncio.run(ble_loop())


if __name__ == "__main__":
    main()
