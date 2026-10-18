"""The tape (lab/CONTRACT.md §4): real ETH minute prints, turned into the environment moves the
venue understands.

Source: Coinbase Exchange public candles (no key). Close prices become ticks at the
instrument's `tick_cents` (1 tick = $1 for ETH, so `round(close)`). A slice is 60 prints.
Consecutive prints become `{"env": "print", "at": p}` when |delta| <= 1 and
`{"env": "gap", "from": a, "to": b}` otherwise. A gap larger than the envelope's
`max_gap_ticks` is an assumption violation: the slice is still played and the test reports it.

What is real: the prints, the cache, and the path played on the live book by `replay --attach`
(RealVenue.drive). What is simulated: `TapeBook`, a small model of the §8.1 fill rules used only
where the real engine is not available: to give the fake venue an event path for a tape (the
fake replays scripted events; it has no matching engine) and for the race's raw side until
`venued` answers raw orders (CONTRACT.md §9.2). Every result built with it says `simulated`.
"""
from __future__ import annotations

import json
import logging
import math
import os
import time
from dataclasses import dataclass, field
from pathlib import Path

import httpx

log = logging.getLogger("lab.tape")

DATA_DIR = Path(__file__).resolve().parent / "data"
SAMPLE = DATA_DIR / "tape-sample.json"
COINBASE = "https://api.exchange.coinbase.com/products/{product}/candles"
SLICE = 60
KEEP_CACHED = 48


@dataclass
class Tape:
    instrument: str
    product: str
    source: str                    # "coinbase" | "cache" | "sample"
    from_t: int                    # unix seconds of the first candle
    to_t: int
    times: list[int]
    closes: list[float]
    prints: list[int]              # ticks
    tick_cents: int
    cached: bool
    fetched_t: int                 # unix ms when the candles were fetched
    file: str | None = None

    @property
    def max_gap(self) -> int:
        return max_gap_of(self.prints)

    def path(self, mark: int | None = None) -> list[dict]:
        """Environment moves for RealVenue.drive / FakeVenue.set_path. With `mark`, the slice is
        shifted so its first print lands on the mark (the gaps are unchanged)."""
        return path_from_prints(self.prints, mark)

    def violations(self, max_gap_ticks: int) -> list[dict]:
        out = []
        for i in range(1, len(self.prints)):
            a, b = self.prints[i - 1], self.prints[i]
            if abs(b - a) > max_gap_ticks:
                out.append({"i": i, "from": a, "to": b, "delta": b - a, "t": self.times[i]})
        return out

    def as_dict(self) -> dict:
        return {"instrument": self.instrument, "product": self.product, "source": self.source, "cached": self.cached,
                "from": self.from_t, "to": self.to_t, "n": len(self.prints), "prints": list(self.prints),
                "times": list(self.times), "tick_cents": self.tick_cents, "max_gap_ticks_seen": self.max_gap,
                "fetched_t": self.fetched_t, "file": self.file}


def to_ticks(close: float, tick_cents: int) -> int:
    return int(round(close * 100.0 / tick_cents))


def max_gap_of(prints: list[int]) -> int:
    return max((abs(b - a) for a, b in zip(prints, prints[1:])), default=0)


def path_from_prints(prints: list[int], mark: int | None = None) -> list[dict]:
    if not prints:
        return []
    off = (mark - prints[0]) if mark is not None else 0
    ps = [p + off for p in prints]
    out: list[dict] = [{"env": "print", "at": ps[0]}]
    for a, b in zip(ps, ps[1:]):
        if abs(b - a) <= 1:
            out.append({"env": "print", "at": b})
        else:
            out.append({"env": "gap", "from": a, "to": b})
    return out


def prints_of_path(path: list[dict]) -> list[int]:
    return [int(s["at"] if s["env"] == "print" else s["to"]) for s in path if "env" in s]


# ----------------------------------------------------------------------------- fetching and cache
def fetch_candles(product: str = "ETH-USD", granularity: int = 60, timeout: float = 10.0) -> list[list]:
    """Newest-first candles `[time, low, high, open, close, volume]` from Coinbase Exchange."""
    r = httpx.get(COINBASE.format(product=product), params={"granularity": granularity}, timeout=timeout,
                  headers={"User-Agent": "bounded-key-lab/1.0"})
    r.raise_for_status()
    data = r.json()
    if not isinstance(data, list) or not data:
        raise ValueError("coinbase returned no candles")
    return data


def _tape_from_candles(candles: list[list], instrument: str, product: str, tick_cents: int, source: str,
                       cached: bool, fetched_t: int, n: int = SLICE, file: str | None = None) -> Tape:
    rows = sorted(candles, key=lambda c: c[0])[-n:]
    times = [int(c[0]) for c in rows]
    closes = [float(c[4]) for c in rows]
    return Tape(instrument=instrument, product=product, source=source, from_t=times[0], to_t=times[-1], times=times,
                closes=closes, prints=[to_ticks(c, tick_cents) for c in closes], tick_cents=tick_cents, cached=cached,
                fetched_t=fetched_t, file=file)


def _cache_files(data_dir: Path) -> list[Path]:
    return sorted((p for p in data_dir.glob("tape-*.json") if p.name != SAMPLE.name), key=lambda p: p.name)


def _load_file(path: Path, tick_cents: int, source: str) -> Tape:
    d = json.loads(path.read_text())
    return _tape_from_candles(d["candles"], d.get("instrument", "ETH"), d.get("product", "ETH-USD"), tick_cents, source,
                              cached=True, fetched_t=int(d.get("fetched_t", 0)), file=str(path))


def load_latest(data_dir: Path = DATA_DIR, instrument: str = "ETH", product: str = "ETH-USD", tick_cents: int = 100,
                n: int = SLICE, online: bool | None = None) -> Tape:
    """The current slice: fetched and cached when online, else the newest cached slice (`cached`
    is true and the card says `tape: cached`), else the committed sample."""
    data_dir.mkdir(parents=True, exist_ok=True)
    if online is None:
        online = os.environ.get("LAB_TAPE_OFFLINE") != "1"
    if online:
        try:
            candles = fetch_candles(product, 60)
            t = _tape_from_candles(candles, instrument, product, tick_cents, "coinbase", cached=False,
                                   fetched_t=int(time.time() * 1000), n=n)
            path = data_dir / f"tape-{t.from_t}.json"
            if not path.exists():
                path.write_text(json.dumps({"instrument": instrument, "product": product, "granularity": 60,
                                            "fetched_t": t.fetched_t, "candles": sorted(candles, key=lambda c: c[0])[-n:]}))
                for old in _cache_files(data_dir)[:-KEEP_CACHED]:
                    old.unlink(missing_ok=True)
            t.file = str(path)
            return t
        except (httpx.HTTPError, ValueError, OSError) as e:
            log.warning("tape: coinbase unavailable (%s); using the cache", e)
    files = _cache_files(data_dir)
    if files:
        return _load_file(files[-1], tick_cents, "cache")
    if SAMPLE.exists():
        return _load_file(SAMPLE, tick_cents, "sample")
    raise RuntimeError("no tape: coinbase unreachable and nothing cached under lab/data/")


# ----------------------------------------------------------------------------- the fill model
def fee_cents(price_ticks: int, qty: int, tick_cents: int, bps: float) -> int:
    return math.ceil(price_ticks * tick_cents * qty * bps / 10000)


@dataclass
class _Order:
    id: str
    kind: str                      # limit | stop_limit | stop_market
    side: str
    qty: int
    price: int | None
    trigger: int | None
    reduce_only: bool
    parent: str | None = None
    oco: str | None = None
    state: str = "live"            # held | live | resting | filled | cancelled
    fills: list = field(default_factory=list)


class TapeBook:
    """A SIMULATION of the §8.1 fill rules over a print sequence, for the two places the real
    engine cannot be used (see the module docstring). Full fill at the print; a stop triggered by
    a print is an aggressor at that print and fills if its limit allows, otherwise it rests at
    its limit; a resting limit fills at its own price when a print trades through it; fees are
    rounded up per fill; reduce-only quantities are capped at the open position."""

    def __init__(self, tick_cents: int = 100, maker_bps: float = 1.0, taker_bps: float = 3.5):
        self.tick, self.maker, self.taker = int(tick_cents), float(maker_bps), float(taker_bps)
        self.reset()

    def reset(self) -> None:
        self.position = self.cash = self.fees = self.trades = 0
        self.orders: list[_Order] = []
        self.events: list[dict] = []
        self.print_ = None

    # ---- state ----------------------------------------------------------------
    def loss(self, p: int | None = None) -> int:
        p = self.print_ if p is None else p
        return -(self.cash + self.position * (p or 0) * self.tick) + self.fees

    def _active(self) -> list[_Order]:
        return [o for o in self.orders if o.state in ("live", "resting", "held")]

    def _fill(self, o: _Order, qty: int, price: int, bps: float) -> None:
        sgn = -1 if o.side == "buy" else 1
        self.cash += sgn * price * qty * self.tick
        self.position += -sgn * qty
        f = fee_cents(price, qty, self.tick, bps)
        self.fees += f
        self.trades += 1
        o.state = "filled"
        self.events.append({"event": "filled", "node": o.id, "qty": qty, "price": price, "fee_cents": f})
        for c in self.orders:
            if c.parent == o.id and c.state == "held":
                c.state = "live"
            if o.oco and c is not o and c.oco == o.oco and c.state in ("live", "held", "resting"):
                c.state = "cancelled"
                self.events.append({"event": "cancelled", "node": c.id})

    def _qty(self, o: _Order) -> int:
        return min(o.qty, abs(self.position)) if o.reduce_only else o.qty

    def on_print(self, p: int) -> None:
        self.print_ = p
        changed = True
        while changed:
            changed = False
            for o in list(self.orders):
                if o.state == "live" and o.kind in ("stop_limit", "stop_market"):
                    crossed = p <= o.trigger if o.side == "sell" else p >= o.trigger
                    if not crossed:
                        continue
                    self.events.append({"event": "triggered", "node": o.id, "print": p,
                                        "limit": None if o.kind == "stop_market" else o.price})
                    q = self._qty(o)
                    if q <= 0:
                        o.state = "cancelled"
                        self.events.append({"event": "cancelled", "node": o.id})
                        continue
                    allows = o.kind == "stop_market" or (o.price <= p if o.side == "sell" else o.price >= p)
                    if allows:
                        self._fill(o, q, p, self.taker)
                        changed = True
                    else:
                        o.state = "resting"
                        self.events.append({"event": "rests", "node": o.id,
                                            "reason": "limit above print" if o.side == "sell" else "limit below print"})
                elif (o.state == "live" and o.kind == "limit") or o.state == "resting":
                    through = p <= o.price if o.side == "buy" else p >= o.price
                    if not through:
                        continue
                    q = self._qty(o)
                    if q <= 0:
                        continue
                    self._fill(o, q, o.price, self.maker)
                    changed = True

    def halted(self) -> bool:
        return self.position == 0 and not self._active()

    # ---- a compiled program on a tape (the fake venue's event path) ---------------
    def run_program(self, compiled: dict, path: list[dict]) -> list[dict]:
        self.reset()
        for n in compiled["nodes"]:
            if n["kind"] == "halt":
                continue
            self.orders.append(_Order(n["id"], n["kind"], n["side"], int(n["qty"]), n.get("price"), n.get("trigger"),
                                      bool(n.get("reduce_only")), n.get("parent"), n.get("oco"),
                                      state="live" if n.get("parent") is None else "held"))
        out: list[dict] = []
        self.loss_series: list[int] = []          # realized + mark-to-market loss after each print
        for step in path:
            if "env" not in step:
                continue
            out.append(dict(step))
            p = int(step["at"] if step["env"] == "print" else step["to"])
            if not self.halted():
                self.events = []
                self.on_print(p)
                out.extend(self.events)
            self.loss_series.append(self.loss(p))
        out.append({"event": "mark", "position": self.position, "loss_cents": self.loss()})
        return out

    # ---- raw orders on a tape (the race's simulated side) ---------------------------
    def run_raw(self, orders: list, path: list[dict], capital_cents: int) -> dict:
        """`orders` are evals.raw_key.RawOrder values. Market orders fill at the first print
        (taker); everything else rests or waits for its trigger. The sub-account is liquidated
        (flattened at the print) when its loss reaches `capital_cents`."""
        self.reset()
        prints = prints_of_path(path)
        if not prints:
            return {"loss_cents": 0, "fees_cents": 0, "trades": 0, "liquidated": False, "position": 0, "series": []}
        first = prints[0]
        self.print_ = first
        markets = []
        for i, o in enumerate(orders):
            kind = o.kind
            if kind == "market":
                markets.append((i, o))
                continue
            price = o.price
            trigger = o.trigger
            if kind in ("stop_limit", "stop_market"):
                trigger = int(o.trigger if o.trigger is not None else o.price)
                price = None if kind == "stop_market" else (trigger - int(o.allow or 0) if o.side == "sell" else trigger + int(o.allow or 0))
            self.orders.append(_Order(f"{kind}#{i}", kind, o.side, int(o.qty), price, trigger, bool(o.reduce_only)))
        liquidated = False
        series: list[list[int]] = []
        for i, o in markets:
            q = min(int(o.qty), abs(self.position)) if o.reduce_only else int(o.qty)
            if q > 0:
                self._fill(_Order(f"market#{i}", "market", o.side, q, None, None, bool(o.reduce_only)), q, first, self.taker)
        for i, p in enumerate(prints):
            self.on_print(p)
            if not liquidated and self.loss(p) >= capital_cents:
                if self.position:
                    self._fill(_Order("liquidation", "market", "sell" if self.position > 0 else "buy", abs(self.position),
                                      None, None, True), abs(self.position), p, self.taker)
                for o in self.orders:
                    if o.state in ("live", "resting", "held"):
                        o.state = "cancelled"
                liquidated = True
            series.append([i, min(self.loss(p), capital_cents)])
        return {"loss_cents": int(min(self.loss(), capital_cents)), "fees_cents": int(self.fees), "trades": int(self.trades),
                "liquidated": liquidated, "position": int(self.position), "series": series, "simulated": True}
