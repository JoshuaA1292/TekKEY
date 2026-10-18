"""The tape: live ETH-USD from Coinbase, one callback per trade print.

Source, in order: the Coinbase Exchange WebSocket `ticker` channel (every trade, no key); if it
fails three times in a row, the REST ticker polled every 2 s until the socket comes back; and,
only when `RACE_TAPE=replay[:speed]` is set, the newest cached minute slice under lab/data/
played in a loop (labelled `replay` on every print and on the page). The feed never invents a
price: without Coinbase and without a cache it reports `disconnected` and the race waits.
"""
from __future__ import annotations

import glob
import json
import logging
import os
import threading
import time
from pathlib import Path
from typing import Callable

import httpx

log = logging.getLogger("race.feed")

WS_URL = os.environ.get("RACE_WS_URL", "wss://ws-feed.exchange.coinbase.com")
REST_URL = "https://api.exchange.coinbase.com/products/{product}/ticker"
TAPE_DIR = Path(__file__).resolve().parents[1] / "lab" / "data"


class Feed(threading.Thread):
    def __init__(self, on_price: Callable[[float, int, str], None], product: str = "ETH-USD", mode: str | None = None):
        super().__init__(name="race-feed", daemon=True)
        self.on_price = on_price
        self.product = product
        self.mode = mode or os.environ.get("RACE_TAPE", "live")
        self._stop = threading.Event()
        self.lock = threading.Lock()
        self.st = {"source": "connecting", "connected": False, "last_price": None, "last_t": None, "ticks": 0,
                   "reconnects": 0, "error": None, "product": product, "mode": self.mode}

    def stop(self) -> None:
        self._stop.set()

    def status(self) -> dict:
        with self.lock:
            return dict(self.st)

    def _set(self, **kw) -> None:
        with self.lock:
            self.st.update(kw)

    def _emit(self, price: float, t_ms: int, source: str) -> None:
        with self.lock:
            self.st.update(last_price=price, last_t=t_ms, ticks=self.st["ticks"] + 1, source=source, connected=True, error=None)
        try:
            self.on_price(price, t_ms, source)
        except Exception:
            log.exception("on_price failed")

    # ---- loops -------------------------------------------------------------------------------
    def run(self) -> None:
        if self.mode.startswith("replay"):
            return self._replay()
        failures = 0
        while not self._stop.is_set():
            try:
                self._ws()
                failures = 0
            except Exception as e:
                failures += 1
                self._set(connected=False, error=f"{type(e).__name__}: {str(e)[:120]}", source="reconnecting")
                log.warning("feed: websocket dropped (%s); attempt %d", e, failures)
                with self.lock:
                    self.st["reconnects"] += 1
            if self._stop.is_set():
                return
            if failures >= 3:
                self._rest(seconds=30)
            else:
                self._stop.wait(min(10, 1 + failures * 2))

    def _ws(self) -> None:
        from websockets.sync.client import connect
        with connect(WS_URL, open_timeout=10, close_timeout=2, max_size=1 << 20) as ws:
            ws.send(json.dumps({"type": "subscribe", "product_ids": [self.product], "channels": ["ticker", "heartbeat"]}))
            self._set(source="coinbase-ws", connected=True, error=None)
            last_msg = time.time()
            while not self._stop.is_set():
                try:
                    raw = ws.recv(timeout=5)
                except TimeoutError:
                    if time.time() - last_msg > 40:
                        raise RuntimeError("no message for 40 s")
                    continue
                last_msg = time.time()
                m = json.loads(raw)
                if m.get("type") == "ticker" and m.get("price"):
                    t = m.get("time")
                    t_ms = _iso_ms(t) if t else int(time.time() * 1000)
                    self._emit(float(m["price"]), t_ms, "coinbase-ws")
                elif m.get("type") == "error":
                    raise RuntimeError(m.get("message") or "ws error")

    def _rest(self, seconds: float) -> None:
        self._set(source="coinbase-rest")
        until = time.time() + seconds
        last = None
        while not self._stop.is_set() and time.time() < until:
            try:
                r = httpx.get(REST_URL.format(product=self.product), timeout=5, headers={"User-Agent": "bounded-key-race/1.0"})
                r.raise_for_status()
                d = r.json()
                px = float(d["price"])
                if px != last:
                    last = px
                    self._emit(px, _iso_ms(d.get("time")) if d.get("time") else int(time.time() * 1000), "coinbase-rest")
            except Exception as e:
                self._set(connected=False, error=f"rest: {type(e).__name__}: {str(e)[:100]}")
            self._stop.wait(2.0)

    def _replay(self) -> None:
        speed = 60.0
        if ":" in self.mode:
            try:
                speed = float(self.mode.split(":", 1)[1])
            except ValueError:
                pass
        files = sorted(p for p in glob.glob(str(TAPE_DIR / "tape-*.json")) if not p.endswith("tape-sample.json")) or \
            sorted(glob.glob(str(TAPE_DIR / "tape-sample.json")))
        if not files:
            self._set(source="replay", connected=False, error="no cached tape under lab/data/")
            return
        candles = sorted(json.loads(Path(files[-1]).read_text())["candles"], key=lambda c: c[0])
        self._set(source="replay", connected=True, error=None, replay_file=os.path.basename(files[-1]), replay_speed=speed)
        i = 0
        while not self._stop.is_set():
            c = candles[i % len(candles)]
            self._emit(float(c[4]), int(time.time() * 1000), "replay")
            i += 1
            self._stop.wait(max(0.2, 60.0 / speed))


def _iso_ms(s: str) -> int:
    """Coinbase's RFC3339 with nanoseconds -> unix ms."""
    from datetime import datetime, timezone
    s = s.rstrip("Z")
    if "." in s:
        head, frac = s.split(".", 1)
        s = head + "." + (frac + "000000")[:6]
    return int(datetime.fromisoformat(s).replace(tzinfo=timezone.utc).timestamp() * 1000)
