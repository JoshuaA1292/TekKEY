"""Backtest mode: the same race on a chosen day of history, faster than real time, with no look-ahead.

A `MarketClock` advances at `speed` x wall time and can be paused. The `BacktestFeed` holds the
window's one-minute Coinbase candles and emits each minute as four prints, open, the extreme
nearer the open, the other extreme, close, at :00 / :15 / :30 / :45 of that minute, each only
once the clock has passed its time. The conductor pauses the clock while the two models decide,
so both act on the same prints and neither sees one that arrives during thinking. The transcript,
the market update's 1 / 5 / 15 minute changes, the charts and the round cadence all run on market
time. What the model is told is exactly what it would have been told live at that moment.

Data: `GET /products/ETH-USD/candles?granularity=60&start=&end=` paged by 300 candles and cached
under lab/data/backtest-<from>-<to>.json. `pick_volatile_day` chooses the widest-ranging UTC day
of the last N days from daily candles.
"""
from __future__ import annotations

import datetime as dt
import json
import logging
import threading
import time
from pathlib import Path
from typing import Callable

import httpx

log = logging.getLogger("race.backtest")

CANDLES = "https://api.exchange.coinbase.com/products/{product}/candles"
DATA_DIR = Path(__file__).resolve().parents[1] / "lab" / "data"
UA = {"User-Agent": "bounded-key-race/1.0"}


def _iso(t: int) -> str:
    return dt.datetime.fromtimestamp(t, dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def parse_when(s: str) -> int:
    """'2026-09-18' | '2026-09-18T06:00Z' | unix seconds -> unix seconds (UTC)."""
    s = str(s).strip()
    if s.isdigit():
        return int(s)
    s = s.rstrip("Z")
    if "T" not in s:
        s += "T00:00:00"
    return int(dt.datetime.fromisoformat(s).replace(tzinfo=dt.timezone.utc).timestamp())


def fetch_window(start_s: int, end_s: int, product: str = "ETH-USD", granularity: int = 60, timeout: float = 15.0) -> list[list]:
    """Candles [time, low, high, open, close, volume] ascending, cached per window."""
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    cache = DATA_DIR / f"backtest-{product}-{start_s}-{end_s}-{granularity}.json"
    if cache.exists():
        return json.loads(cache.read_text())["candles"]
    out: dict[int, list] = {}
    step = 300 * granularity
    t = start_s
    while t < end_s:
        e = min(end_s, t + step)
        r = httpx.get(CANDLES.format(product=product), params={"granularity": granularity, "start": _iso(t), "end": _iso(e)}, timeout=timeout, headers=UA)
        if r.status_code == 429:
            time.sleep(1.0)
            continue
        r.raise_for_status()
        for c in r.json():
            if start_s <= int(c[0]) < end_s:
                out[int(c[0])] = c
        t = e
        time.sleep(0.25)
    candles = [out[k] for k in sorted(out)]
    if not candles:
        raise ValueError("no candles in the window")
    cache.write_text(json.dumps({"product": product, "granularity": granularity, "start": start_s, "end": end_s,
                                 "fetched_t": int(time.time() * 1000), "candles": candles}))
    return candles


def pick_volatile_day(days: int = 30, product: str = "ETH-USD") -> dict:
    """The UTC day with the widest (high - low) / close over the last `days` complete days."""
    end = dt.datetime.now(dt.timezone.utc).replace(hour=0, minute=0, second=0, microsecond=0)
    start = end - dt.timedelta(days=days + 1)
    r = httpx.get(CANDLES.format(product=product), params={"granularity": 86400, "start": start.isoformat(), "end": end.isoformat()}, timeout=15, headers=UA)
    r.raise_for_status()
    best = None
    for c in r.json():
        t, lo, hi, op, cl = int(c[0]), float(c[1]), float(c[2]), float(c[3]), float(c[4])
        if t + 86400 > end.timestamp():
            continue
        rng = (hi - lo) / cl
        if best is None or rng > best["range_pct"] / 100:
            best = {"start": t, "end": t + 86400, "low": lo, "high": hi, "open": op, "close": cl, "range_pct": round(rng * 100, 2), "date": _iso(t)[:10]}
    return best


def prints_of_candle(c: list) -> list[tuple[int, float]]:
    """(t_ms, price) x 4: open, the extreme nearer the open, the other extreme, close."""
    t, lo, hi, op, cl = int(c[0]), float(c[1]), float(c[2]), float(c[3]), float(c[4])
    first, second = (hi, lo) if abs(hi - op) <= abs(lo - op) else (lo, hi)
    return [(t * 1000, op), (t * 1000 + 15_000, first), (t * 1000 + 30_000, second), (t * 1000 + 45_000, cl)]


class MarketClock:
    """Market time that runs at `speed` x wall time and can be paused (while the models decide)."""

    def __init__(self, start_ms: int, speed: float):
        self.speed = float(speed)
        self._market = int(start_ms)
        self._wall = time.monotonic()
        self._paused = False
        self._lock = threading.Lock()

    def now_ms(self) -> int:
        with self._lock:
            if self._paused:
                return self._market
            return int(self._market + (time.monotonic() - self._wall) * 1000 * self.speed)

    def pause(self) -> None:
        with self._lock:
            if not self._paused:
                self._market = int(self._market + (time.monotonic() - self._wall) * 1000 * self.speed)
                self._paused = True

    def resume(self) -> None:
        with self._lock:
            if self._paused:
                self._wall = time.monotonic()
                self._paused = False

    @property
    def paused(self) -> bool:
        return self._paused


class BacktestFeed(threading.Thread):
    """Emits the window's prints as the market clock reaches them. `on_price(price, t_ms, source)`
    receives market time, never wall time. `on_done()` fires after the last print."""

    def __init__(self, on_price: Callable[[float, int, str], None], candles: list[list], clock: MarketClock,
                 on_done: Callable[[], None] | None = None, product: str = "ETH-USD"):
        super().__init__(name="race-backtest-feed", daemon=True)
        self.on_price, self.clock, self.on_done, self.product = on_price, clock, on_done, product
        self.prints = [p for c in candles for p in prints_of_candle(c)]
        self.i = 0
        self._stop = threading.Event()
        self.lock = threading.Lock()
        self.st = {"source": "backtest", "connected": True, "last_price": None, "last_t": None, "ticks": 0, "reconnects": 0, "error": None,
                   "product": product, "mode": "backtest", "total_prints": len(self.prints), "done": False,
                   "window": {"from_ms": self.prints[0][0], "to_ms": self.prints[-1][0]} if self.prints else None}

    def stop(self) -> None:
        self._stop.set()

    def status(self) -> dict:
        with self.lock:
            d = dict(self.st)
        d["market_t"] = self.clock.now_ms()
        d["paused"] = self.clock.paused
        d["speed"] = self.clock.speed
        d["progress"] = self.i / max(1, len(self.prints))
        return d

    def run(self) -> None:
        while not self._stop.is_set() and self.i < len(self.prints):
            t_ms, px = self.prints[self.i]
            if self.clock.now_ms() < t_ms:
                self._stop.wait(min(0.5, max(0.01, (t_ms - self.clock.now_ms()) / 1000 / self.clock.speed)))
                continue
            self.i += 1
            with self.lock:
                self.st.update(last_price=px, last_t=t_ms, ticks=self.st["ticks"] + 1)
            try:
                self.on_price(px, t_ms, "backtest")
            except Exception:
                log.exception("on_price failed")
        with self.lock:
            self.st["done"] = self.i >= len(self.prints)
        if self.st["done"] and self.on_done:
            try:
                self.on_done()
            except Exception:
                log.exception("on_done failed")
