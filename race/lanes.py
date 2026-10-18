"""The two lanes.

RawLane: an ordinary API key. Its orders are placed on the exchange by a direct session, exactly
as sent, in the exchange's native order types (limit, stop-limit, stop-market, parent/child
holds and OCO groups: the same primitives the MPL compiler targets, so neither lane has a
better toolkit). A market order is placed the way the venue's own flatten is: a limit at the
last print and a stop-market one tick through it, in one OCO group, so it fills at the next
print at the print. Money is read from the exchange's fill journal with the venue's fee
arithmetic (race/ledger.py). The owner's standing instructions are only *observed* here: every
time the account is outside them (too many lots, a position with no live stop, more than the
daily loss) a breach is recorded, because an ordinary key has no way to refuse. At a
mark-to-market loss of the whole capital the account is liquidated: every order cancelled, the
position flattened at the next print.

BoundedLane: a real account on the race's gateway, opened by an owner key with an envelope; the
model holds an author key and can only `POST /program`. A certified program is deployed with
its `bk_` key by the owner's automation (this module), runs natively on the exchange, and ends
by its own stop or take, or by a `cancel` with the same key. Money: the gateway's ledger and the
bank balance, plus the venue's mark-to-market of the live program.
"""
from __future__ import annotations

import json
import logging
import re
import threading
import time
from dataclasses import dataclass, field
from typing import Any


from . import exchange_client as xc
from .ledger import Fees, Ledger, bps100
from .store import RaceStore
from .world import World

log = logging.getLogger("race.lanes")

RAW_ACCOUNT_BASE = 200        # raw accounts rotate 200..249 per epoch; venued allocates programs from 1 up
RAW_ACCOUNTS = 50
MAX_ORDER_LOTS = 1000         # the exchange's max order size for this venue
MAX_OPEN_ORDERS = 64
OCO_BASE = 0x60000000         # the race's own OCO id range (venued 0x00100000+, replay 0x40000000+)


# ----------------------------------------------------------------------------- raw
@dataclass
class RawOrder:
    order_id: int
    ref: str
    kind: str
    side: str
    qty: int
    price: int | None
    trigger: int | None
    reduce_only: bool
    label: str
    state: str = "resting"
    filled: int = 0
    t: int = 0
    group: str | None = None
    leg: str | None = None       # unused for logical orders; kept for the state file
    legs: list = field(default_factory=list)       # a market order: [stop-market leg id, limit leg id], one OCO pair
    children: list = field(default_factory=list)   # specs held until this (market) order fills, then placed

    def public(self) -> dict:
        return {"ref": self.ref, "order_id": self.order_id, "kind": self.kind, "side": self.side, "qty": self.qty,
                "price": self.price, "trigger": self.trigger, "reduce_only": self.reduce_only, "state": self.state,
                "filled": self.filled, "label": self.label, "group": self.group, "legs": list(self.legs),
                "pending_children": len(self.children), "t": self.t}


class RawLane:
    name = "raw"

    def __init__(self, world: World, store: RaceStore, cfg: dict):
        self.w, self.store, self.cfg = world, store, cfg
        self.capital = int(cfg["capital_cents"])
        self.tick = world.tick_cents
        self.book = world.book
        fb = cfg["envelope"]["env"]["fee_bps"]
        self.fees = Fees(bps100(fb["maker"]), bps100(fb["taker"]))
        self.lock = threading.RLock()
        self.epoch = 0
        self.account = RAW_ACCOUNT_BASE
        self.ledger = Ledger(self.tick, self.fees)
        self.journal_pos = 0
        self.orders: dict[int, RawOrder] = {}
        self._leg_of: dict[int, int] = {}            # exchange order id of a leg -> the logical order's id
        self.history: list[RawOrder] = []
        self._oco = 0
        self._ref = 0
        self.liquidating = False
        self.liquidated = False
        self.liquidated_t: int | None = None
        self.breaches: dict[str, dict] = {}          # kind -> {count, since, total_ms}
        self.decisions = 0
        self.rejections = 0
        self.last_decision: dict | None = None

    # ---- epoch ---------------------------------------------------------------------------------
    def start_epoch(self, epoch: int) -> None:
        with self.lock:
            self.epoch = epoch
            self.account = RAW_ACCOUNT_BASE + (epoch % RAW_ACCOUNTS)
            self._cleanup_previous()
            self.ledger = Ledger(self.tick, self.fees)
            self.journal_pos = self.w.ctl.fill_count(self.account)
            self.orders.clear()
            self._leg_of.clear()
            self.history.clear()
            self._oco = 0
            self._ref = 0
            self.liquidating = self.liquidated = False
            self.liquidated_t = None
            self.breaches = {}
            self.decisions = self.rejections = 0
            self.last_decision = None
            pos = self.w.ctl.position(self.account, self.book)
            if pos:
                log.warning("raw account %d starts epoch %d with a leftover position %d; flattening first", self.account, epoch, pos)
                self._flatten(pos)

    def _cleanup_previous(self) -> None:
        """Orders persisted by earlier epochs (and processes) that may still rest: cancel them."""
        with self.w.db.connect() as c:
            rows = c.execute("SELECT order_id, account FROM race_raw_orders WHERE live=1").fetchall()
        for r in rows:
            try:
                self.w.raw.cancel(int(r["order_id"]))
            except xc.ExchangeError:
                pass
        with self.w.db.tx() as c:
            c.execute("UPDATE race_raw_orders SET live=0 WHERE live=1")
        # a leftover position in an older rotating account is flattened so nothing of ours rests there
        for a in range(RAW_ACCOUNT_BASE, RAW_ACCOUNT_BASE + RAW_ACCOUNTS):
            if a == self.account:
                continue
            try:
                pos = self.w.ctl.position(a, self.book)
            except xc.ExchangeError:
                continue
            if pos:
                last = self.w.ctl.last(self.book)
                if last is not None:
                    self._flatten_account(a, pos, last, tracked=False)

    # ---- orders --------------------------------------------------------------------------------
    def _next_oco(self) -> int:
        self._oco += 1
        return OCO_BASE + ((self.epoch & 0xFFF) << 12) + (self._oco & 0xFFF)

    def _next_ref(self) -> str:
        self._ref += 1
        return f"o{self._ref}"

    def _persist(self, o: RawOrder, live: bool = True) -> None:
        with self.w.db.tx() as c:
            for oid in ([o.order_id] + [l for l in o.legs if l != o.order_id]):
                c.execute("INSERT OR REPLACE INTO race_raw_orders(order_id, epoch, account, ref, spec_json, live, t) VALUES (?,?,?,?,?,?,?)",
                          (oid, self.epoch, self.account, o.ref, json.dumps(o.public()), 1 if live else 0, self.w.now_ms()))

    def apply(self, decision: dict) -> dict:
        """Place what the model asked for. Returns the acks, one per order sent, as the model will see them."""
        with self.lock:
            self.decisions += 1
            out: dict[str, Any] = {"acks": [], "cancels": [], "errors": []}
            if self.liquidated or self.liquidating:
                out["errors"].append("account liquidated: the exchange refuses new orders for this account")
                self.last_decision = out
                return out
            for ref in decision.get("cancel") or []:
                oid = self._resolve(ref)
                if oid is None:
                    out["cancels"].append({"ref": str(ref), "status": "not_found"})
                    continue
                o = self.orders.get(oid)
                names = []
                for leg in (o.legs if o and o.legs else [oid]):
                    names.append(self.w.raw.cancel(leg).name)
                ok = "cancelled" in names
                if o and ok:
                    o.state = "cancelled"
                    o.children = []
                    self._retire(o)
                out["cancels"].append({"ref": str(ref), "status": "cancelled" if ok else names[0]})
            last = self.w.ctl.last(self.book)
            specs = decision.get("orders") or []
            if len(specs) > 20:
                out["errors"].append(f"{len(specs)} orders in one message; the first 20 were sent")
                specs = specs[:20]
            placed: list[int | None] = []
            groups: dict[str, int] = {}
            for i, s in enumerate(specs):
                try:
                    acks = self._place(s, i, placed, groups, last)
                except ValueError as e:
                    out["acks"].append({"index": i, "status": "invalid", "reason": str(e)})
                    placed.append(None)
                    continue
                placed.append(acks[0]["order_id"] if acks and acks[0].get("order_id") else None)
                out["acks"].extend(acks)
            live = len([o for o in self.orders.values() if o.state in ("resting", "pending", "held", "queued")])
            out["open_orders"] = live
            self.last_decision = out
            return out

    def _resolve(self, ref) -> int | None:
        if isinstance(ref, int):
            return ref if ref in self.orders else None
        m = re.match(r"^\s*o?(\d+)\s*$", str(ref))
        if not m:
            return None
        n = m.group(1)
        for o in self.orders.values():
            if o.ref == f"o{n}" or str(o.order_id) == n:
                return o.order_id
        return None

    def _place(self, s: dict, index: int, placed: list, groups: dict, last: int | None) -> list[dict]:
        kind = str(s.get("kind", "limit")).lower().replace("-", "_")
        side_s = str(s.get("side", "")).lower()
        if side_s not in ("buy", "sell"):
            raise ValueError("side must be buy or sell")
        side = xc.BUY if side_s == "buy" else xc.SELL
        try:
            qty = int(s.get("qty", 0))
        except (TypeError, ValueError):
            raise ValueError("qty must be an integer number of lots")
        if qty < 1:
            raise ValueError("qty must be >= 1 lot")
        if qty > MAX_ORDER_LOTS:
            raise ValueError(f"qty {qty} exceeds the exchange's max order size of {MAX_ORDER_LOTS} lots")
        if len([o for o in self.orders.values() if o.state in ("resting", "pending", "held", "queued")]) >= MAX_OPEN_ORDERS:
            raise ValueError(f"more than {MAX_OPEN_ORDERS} open orders")
        reduce_only = bool(s.get("reduce_only", False))
        parent = 0
        hold_under: RawOrder | None = None
        if s.get("after") is not None:
            try:
                j = int(s["after"])
            except (TypeError, ValueError):
                raise ValueError("after must be the index of an earlier order in this message")
            if j < 0 or j >= index or placed[j] is None:
                raise ValueError("after must name an earlier order in this message that was accepted")
            parent = placed[j]
            po = self.orders.get(parent)
            if po is not None and po.legs:
                hold_under = po            # a market order has two legs: the child is placed when it fills
        oco = 0
        group = s.get("group")
        if group:
            group = str(group)
            oco = groups.setdefault(group, self._next_oco())
        px = s.get("price")
        trig = s.get("trigger")
        px = int(px) if px is not None else None
        trig = int(trig) if trig is not None else None
        acks = []
        t = self.w.now_ms()
        if hold_under is not None:
            if kind == "market":
                raise ValueError("a market order cannot wait on another market order")
            spec = dict(s, _oco=oco, _group=group)
            spec.pop("after", None)
            hold_under.children.append(spec)
            ref = self._next_ref()
            spec["_ref"] = ref
            return [{"index": index, "ref": ref, "order_id": None, "status": "held", "kind": kind, "side": side_s, "qty": qty,
                     "price": px, "trigger": trig, "reduce_only": reduce_only, "label": f"placed when {hold_under.ref} fills"}]

        def send(otype: int, price: int, trigger: int, label: str, leg: str | None, k: str, oco_id: int = 0) -> dict:
            a = self.w.raw.submit(self.account, side, otype, self.book, qty, price=price, trigger=trigger,
                                  reduce_only=reduce_only, parent=parent, oco=oco_id or oco)
            ref = self._next_ref()
            d = {"index": index, "ref": ref, "order_id": a.order_id, "status": a.name, "kind": k, "side": side_s, "qty": qty,
                 "price": price or None, "trigger": trigger or None, "reduce_only": reduce_only, "label": label}
            if a.name == "rejected":
                self.rejections += 1
                d["reason"] = self._why_rejected(reduce_only, parent, otype)
                return d
            o = RawOrder(a.order_id, ref, k, side_s, qty, price or None, trigger or None, reduce_only, label,
                         state={"rested": "resting", "held": "held", "filled": "filled", "done": "done"}.get(a.name, a.name),
                         t=t, group=group, leg=leg)
            if a.name in ("filled", "done"):
                o.state = "filled" if a.name == "filled" else "done"
            self.orders[a.order_id] = o
            self.history.append(o)
            self._persist(o, live=o.state in ("resting", "held", "pending", "queued"))
            return d

        if kind == "limit":
            if px is None:
                raise ValueError("a limit order needs a price")
            acks.append(send(xc.LIMIT, px, 0, "limit", None, "limit"))
        elif kind == "stop_market":
            if trig is None:
                raise ValueError("a stop_market order needs a trigger")
            acks.append(send(xc.STOP_MARKET, 0, trig, "stop", None, "stop_market"))
        elif kind == "stop_limit":
            if trig is None:
                trig = px
            if trig is None:
                raise ValueError("a stop_limit order needs a trigger")
            if px is None or s.get("allow") is not None:
                allow = int(s.get("allow") or 0)
                px = trig - allow if side == xc.SELL else trig + allow
            acks.append(send(xc.STOP_LIMIT, px, trig, "stop-limit", None, "stop_limit"))
        elif kind == "market":
            if last is None:
                raise ValueError("no print yet: a market order has nothing to fill against")
            pair = self._next_oco()
            through = last - 1 if side == xc.SELL else last + 1
            a1 = self.w.raw.submit(self.account, side, xc.STOP_MARKET, self.book, qty, trigger=through, reduce_only=reduce_only, parent=parent, oco=pair)
            a2 = self.w.raw.submit(self.account, side, xc.LIMIT, self.book, qty, price=last, reduce_only=reduce_only, parent=parent, oco=pair)
            ref = self._next_ref()
            if a1.name == "rejected" and a2.name == "rejected":
                self.rejections += 1
                acks.append({"index": index, "ref": ref, "order_id": None, "status": "rejected", "kind": "market", "side": side_s, "qty": qty,
                             "reduce_only": reduce_only, "label": "market", "reason": self._why_rejected(reduce_only, parent, xc.LIMIT)})
            else:
                legs = [x.order_id for x in (a1, a2) if x.name != "rejected"]
                filled_now = any(x.name == "filled" for x in (a1, a2))
                o = RawOrder(legs[0], ref, "market", side_s, qty, None, None, reduce_only, "market (fills at the next print)",
                             state="filled" if filled_now else ("held" if a1.name == "held" else "working"), t=t, group=group, legs=legs)
                for l in legs:
                    self._leg_of[l] = o.order_id
                self.orders[o.order_id] = o
                self.history.append(o)
                self._persist(o, live=not filled_now)
                acks.append({"index": index, "ref": ref, "order_id": o.order_id, "status": o.state, "kind": "market", "side": side_s, "qty": qty,
                             "price": None, "trigger": None, "reduce_only": reduce_only, "label": "market: fills at the next print"})
        else:
            raise ValueError(f"unknown order kind {kind!r}; use limit, market, stop_limit or stop_market")
        return acks

    def _why_rejected(self, reduce_only: bool, parent: int, otype: int) -> str:
        if reduce_only and not parent and otype in (xc.LIMIT, xc.MARKET, xc.IOC):
            return "reduce-only with no position to reduce (use `after` to hold it until the entry fills)"
        if parent:
            return "its parent order is no longer live"
        return "rejected by the exchange (bad price, size or type)"

    def _retire(self, o: RawOrder) -> None:
        self._persist(o, live=False)

    # ---- sync: the journal, order states, equity, liquidation, the owner's rules ---------------------
    def sync(self, last: int | None, emit) -> dict:
        with self.lock:
            fills = []
            n = self.w.ctl.fill_count(self.account)
            while self.journal_pos < n:
                f = self.w.ctl.fill_at(self.account, self.journal_pos)
                self.journal_pos += 1
                if f.book != self.book:
                    continue
                fee = self.ledger.fill(f.side, f.price, f.qty, f.maker)
                o = self.orders.get(self._leg_of.get(f.order_id, f.order_id))
                ref = o.ref if o else f"#{f.order_id}"
                label = o.label if o else "order"
                if o:
                    o.filled += f.qty
                    if o.children and o.filled >= o.qty:
                        self._release_children(o, emit)
                row = {"lane": "raw", "t": self.w.now_ms(), "ref": ref, "label": label, "side": "buy" if f.side == xc.BUY else "sell",
                       "qty": f.qty, "price": f.price, "fee_cents": fee, "maker": f.maker, "position_after": self.ledger.pos}
                self.store.add_fill(self.epoch, "raw", row["t"], ref, row["side"], f.qty, f.price, fee, f.maker, self.ledger.pos)
                fills.append(row)
                emit("fill", row)
            for o in list(self.orders.values()):
                if o.state in ("resting", "held", "pending", "queued", "working"):
                    if o.legs:
                        states = [self.w.ctl.order_state(l) for l in o.legs]
                        if all(st == "gone" for st in states):
                            o.state = "filled" if o.filled >= o.qty else ("cancelled" if o.filled == 0 else "done")
                            if o.state != "filled" and o.children:
                                o.children = []
                            self._retire(o)
                        elif "held" in states and o.state != "held":
                            o.state = "held"
                        elif o.state == "held" and "held" not in states:
                            o.state = "working"
                        continue
                    st = self.w.ctl.order_state(o.order_id)
                    if st == "gone":
                        o.state = "filled" if o.filled >= o.qty else ("cancelled" if o.filled == 0 and not self._maybe_filled(o) else "done")
                        self._retire(o)
                    elif st != o.state:
                        o.state = st
            equity = self.capital - self.ledger.loss(last)
            if not self.liquidated and not self.liquidating and last is not None and self.ledger.loss(last) >= self.capital:
                self._liquidate(last, emit)
            if self.liquidating and self.ledger.pos == 0 and not self.live_orders():
                self.liquidating, self.liquidated = False, True
                self.liquidated_t = self.w.now_ms()
                emit("liquidated", {"lane": "raw", "equity_cents": equity, "fees_cents": self.ledger.fees_cents, "trades": self.ledger.trades})
            self._observe_rules(last, emit)
            return {"fills": fills, "equity": equity}

    def _maybe_filled(self, o: RawOrder) -> bool:
        return o.filled > 0

    def live_orders(self) -> list[RawOrder]:
        return [o for o in self.orders.values() if o.state in ("resting", "held", "pending", "queued", "working")]

    def _release_children(self, parent: RawOrder, emit) -> None:
        """The orders the model attached to a market order ("after"), placed now that it has filled: the
        exchange's own parent/child hold cannot span the pair's two legs, so the venue does the hold."""
        specs, parent.children = parent.children, []
        side_of = {"buy": xc.BUY, "sell": xc.SELL}
        placed = []
        for sp in specs:
            kind = str(sp.get("kind", "limit")).lower().replace("-", "_")
            side = side_of.get(str(sp.get("side", "")).lower(), xc.SELL)
            qty = int(sp.get("qty", parent.qty) or parent.qty)
            px = int(sp["price"]) if sp.get("price") is not None else None
            trig = int(sp["trigger"]) if sp.get("trigger") is not None else None
            ro = bool(sp.get("reduce_only", False))
            oco = int(sp.get("_oco") or 0)
            try:
                if kind == "limit" and px is not None:
                    a = self.w.raw.submit(self.account, side, xc.LIMIT, self.book, qty, price=px, reduce_only=ro, oco=oco)
                elif kind == "stop_market" and trig is not None:
                    a = self.w.raw.submit(self.account, side, xc.STOP_MARKET, self.book, qty, trigger=trig, reduce_only=ro, oco=oco)
                elif kind == "stop_limit" and (trig is not None or px is not None):
                    trig = trig if trig is not None else px
                    if px is None or sp.get("allow") is not None:
                        allow = int(sp.get("allow") or 0)
                        px = trig - allow if side == xc.SELL else trig + allow
                    a = self.w.raw.submit(self.account, side, xc.STOP_LIMIT, self.book, qty, price=px, trigger=trig, reduce_only=ro, oco=oco)
                else:
                    continue
            except xc.ExchangeError:
                continue
            if a.name == "rejected":
                self.rejections += 1
                continue
            o = RawOrder(a.order_id, sp.get("_ref") or self._next_ref(), kind, "buy" if side == xc.BUY else "sell", qty, px, trig, ro,
                         f"{kind.replace('_', '-')} (placed when {parent.ref} filled)",
                         state={"rested": "resting", "held": "held"}.get(a.name, "pending" if kind.startswith("stop") else a.name),
                         t=self.w.now_ms(), group=sp.get("_group"))
            self.orders[o.order_id] = o
            self.history.append(o)
            self._persist(o)
            placed.append(o.public())
        if placed:
            emit("orders.released", {"lane": "raw", "parent": parent.ref, "orders": placed})

    def _liquidate(self, last: int, emit) -> None:
        self.liquidating = True
        for o in self.live_orders():
            for leg in (o.legs or [o.order_id]):
                try:
                    self.w.raw.cancel(leg)
                except xc.ExchangeError:
                    pass
            o.state = "cancelled"
            o.children = []
            self._retire(o)
        if self.ledger.pos:
            self._flatten(self.ledger.pos)
        emit("liquidation", {"lane": "raw", "position": self.ledger.pos, "loss_cents": self.ledger.loss(last), "capital_cents": self.capital,
                             "detail": "mark-to-market loss reached the account's capital: every order cancelled, the position is flattened at the next print"})

    def _flatten(self, pos: int) -> None:
        last = self.w.ctl.last(self.book)
        if last is None:
            return
        self._flatten_account(self.account, pos, last, tracked=True)

    def _flatten_account(self, account: int, pos: int, last: int, tracked: bool) -> None:
        side = xc.SELL if pos > 0 else xc.BUY
        q = abs(pos)
        oco = self._next_oco()
        a = self.w.raw.submit(account, side, xc.STOP_MARKET, self.book, q, trigger=last - 1 if pos > 0 else last + 1, reduce_only=True, oco=oco)
        b = self.w.raw.submit(account, side, xc.LIMIT, self.book, q, price=last, reduce_only=True, oco=oco)
        if tracked:
            t = self.w.now_ms()
            for ack, kind, leg in ((a, "stop_market", "stop"), (b, "limit", "limit")):
                if ack.ok and ack.order_id:
                    o = RawOrder(ack.order_id, self._next_ref(), kind, "sell" if side == xc.SELL else "buy", q,
                                 last if leg == "limit" else None, (last - 1 if pos > 0 else last + 1) if leg == "stop" else None,
                                 True, "liquidation", state="held" if ack.name == "held" else ("pending" if leg == "stop" else "resting"), t=t, leg=leg)
                    self.orders[o.order_id] = o
                    self.history.append(o)
                    self._persist(o)

    def _observe_rules(self, last: int | None, emit) -> None:
        env = self.cfg["envelope"]
        now = self.w.now_ms()
        pos = self.ledger.pos
        has_stop = any(o.kind in ("stop_market", "stop_limit") and o.reduce_only and o.state in ("pending", "held", "queued", "resting")
                       and ((pos > 0 and o.side == "sell") or (pos < 0 and o.side == "buy")) for o in self.orders.values())
        loss = self.ledger.loss(last) if last is not None else self.ledger.realized_loss()
        checks = {
            "exposure": abs(pos) > int(env["max_exposure_lots"]),
            "no_stop": pos != 0 and not has_stop,
            "daily_loss": loss > int(env["max_loss_cents"]),
            "fees": self.ledger.fees_cents > int(env["max_fees_cents"]),
        }
        for kind, bad in checks.items():
            b = self.breaches.setdefault(kind, {"count": 0, "since": None, "total_ms": 0, "active": False})
            if bad and not b["active"]:
                b.update(active=True, since=now, count=b["count"] + 1)
                emit("breach", {"lane": "raw", "kind": kind, "position": pos, "loss_cents": loss, "fees_cents": self.ledger.fees_cents,
                                "limit": {"exposure": env["max_exposure_lots"], "no_stop": "a live stop", "daily_loss": env["max_loss_cents"],
                                          "fees": env["max_fees_cents"]}[kind]})
            elif not bad and b["active"]:
                b.update(active=False, total_ms=b["total_ms"] + (now - (b["since"] or now)), since=None)

    # ---- views ---------------------------------------------------------------------------------
    def snapshot(self, last: int | None) -> dict:
        with self.lock:
            loss = self.ledger.loss(last) if last is not None else self.ledger.realized_loss()
            now = self.w.now_ms()
            br = {}
            for k, b in self.breaches.items():
                br[k] = {"count": b["count"], "active": b["active"],
                         "total_ms": b["total_ms"] + ((now - b["since"]) if b["active"] and b["since"] else 0)}
            return {
                "lane": "raw", "account": self.account, "capital_cents": self.capital, "equity_cents": self.capital - loss,
                "pnl_cents": -loss, "position": self.ledger.pos, "avg_cost_cents": self.ledger.avg_cost(),
                "cash_cents": self.ledger.cash_cents, "fees_cents": self.ledger.fees_cents, "trades": self.ledger.trades,
                "realized_loss_cents": self.ledger.realized_loss(), "unrealized_cents": -(loss - self.ledger.realized_loss()),
                "max_exposure": self.ledger.max_exposure, "open_orders": [o.public() for o in self.live_orders()],
                "recent_orders": [o.public() for o in self.history[-12:]], "liquidating": self.liquidating,
                "liquidated": self.liquidated, "liquidated_t": self.liquidated_t, "breaches": br,
                "decisions": self.decisions, "exchange_rejections": self.rejections,
                "exchange_position": None, "exchange_cash": None,
            }

    def verify_against_exchange(self) -> dict:
        """The exchange's own counters for the account (position, cash): the cross-check the page shows."""
        with self.lock:
            pos = self.w.ctl.position(self.account, self.book)
            cash = self.w.ctl.cash(self.account)
            return {"exchange_position": pos, "exchange_cash_cents": cash * self.tick, "ledger_position": self.ledger.pos,
                    "ledger_cash_cents": self.ledger.cash_cents, "agree": pos == self.ledger.pos and cash * self.tick == self.ledger.cash_cents}

    def describe_for_prompt(self, last: int | None) -> str:
        s = self.snapshot(last)
        lines = [f"Your account (ordinary key, exchange account {s['account']}): equity ${s['equity_cents'] / 100:,.2f} "
                 f"(start $10,000.00), position {s['position']:+d} lot(s)" + (f" at avg {s['avg_cost_cents'] // 100}" if s['avg_cost_cents'] else "")
                 + f", fees paid ${s['fees_cents'] / 100:,.2f}, {s['trades']} fills."]
        if s["liquidated"] or s["liquidating"]:
            lines.append("THE ACCOUNT HAS BEEN LIQUIDATED: the exchange closed the position; no further orders are accepted.")
        oo = s["open_orders"]
        if oo:
            lines.append("Open orders: " + "; ".join(
                f"{o['ref']} {o['kind']} {o['side']} {o['qty']}" + (f" @ {o['price']}" if o['price'] else "") + (f" trigger {o['trigger']}" if o['trigger'] else "")
                + (" reduce-only" if o['reduce_only'] else "") + f" [{o['state']}]"
                + (f" (+{o['pending_children']} order(s) placed when it fills)" if o.get('pending_children') else "") for o in oo))
        else:
            lines.append("Open orders: none.")
        return "\n".join(lines)


# ----------------------------------------------------------------------------- bounded
class BoundedLane:
    name = "bounded"

    def __init__(self, world: World, store: RaceStore, cfg: dict):
        self.w, self.store, self.cfg = world, store, cfg
        self.capital = int(cfg["capital_cents"])
        self.lock = threading.RLock()
        self.epoch = 0
        self.account: int | None = None
        self._ok = self._ak = None
        self.envelope: dict | None = None
        self.live: dict | None = None            # {pid, bk, bound, fund_cents, deployed_t, state}
        self.programs: list[dict] = []           # every submission this epoch, newest last
        self.last_view: dict | None = None
        self._events_after = 0
        self.decisions = 0
        self.fills_n = 0

    # ---- helpers -------------------------------------------------------------------------------
    @staticmethod
    def H(key: str) -> dict:
        return {"Authorization": f"Bearer {key}"}

    def start_epoch(self, epoch: int, owner_phone: str | None = None) -> None:
        with self.lock:
            self.epoch = epoch
            body = {"opening_balance_cents": self.capital}
            if owner_phone:
                body["owner_phone"] = owner_phone          # the live race's owner gets the alarms on their phone
            r = self.w.http.post("/account", json=body)
            if r.status_code != 200:
                raise RuntimeError(f"POST /account -> {r.status_code} {r.text[:200]}")
            self.account, self._ok = r.json()["account"], r.json()["owner_key"]
            env = dict(self.cfg["envelope"], account=self.account)
            r = self.w.http.put(f"/account/{self.account}/envelope", json=env, headers=self.H(self._ok))
            if r.status_code != 200:
                raise RuntimeError(f"PUT envelope -> {r.status_code} {r.text[:200]}")
            self.envelope = r.json()
            self._ak = self.w.http.post(f"/account/{self.account}/author-key", headers=self.H(self._ok)).json()["author_key"]
            self.live = None
            self.programs = []
            self.decisions = 0
            self.fills_n = 0
            with self.w.db.connect() as c:
                self._events_after = int(c.execute("SELECT COALESCE(MAX(id),0) FROM events").fetchone()[0])
            self.last_view = self.w.http.get(f"/account/{self.account}", headers=self.H(self._ok)).json()

    # ---- the model's three verbs ---------------------------------------------------------------
    def submit(self, source: str) -> tuple[int, dict]:
        r = self.w.http.post("/program", json={"account": self.account, "backend": "minsky", "source": source}, headers=self.H(self._ak))
        try:
            body = r.json()
        except ValueError:
            body = {"error": "non_json", "text": r.text[:300]}
        return r.status_code, body

    def apply(self, decision: dict, emit) -> dict:
        with self.lock:
            self.decisions += 1
            action = decision.get("action")
            if action == "cancel":
                return self._cancel(emit)
            if action != "program":
                return {"outcome": "hold"}
            source = decision.get("mpl") or ""
            if not source.strip():
                return {"outcome": "invalid", "detail": "no mpl program in the reply"}
            status, body = self.submit(source)
            rec = {"t": self.w.now_ms(), "http": status, "program_id": body.get("program_id"), "source": source,
                   "violates": body.get("violates"), "error": body.get("error"), "status": body.get("status")}
            if status == 200:
                rec.update(bound=body.get("bound"), budget=body.get("budget"), certificate={k: v for k, v in body.items() if k != "key"})
                dep = self.w.http.post(f"/program/{body['program_id']}/deploy", headers=self.H(body["key"]))
                if dep.status_code == 200:
                    d = dep.json()
                    self.live = {"pid": body["program_id"], "bk": body["key"], "bound": body["bound"], "fund_cents": d.get("fund_cents"),
                                 "deployed_t": self.w.now_ms(), "sub_account": d.get("sub_account"), "state": None, "source": source}
                    rec.update(outcome="certified+deployed", deploy=d)
                else:
                    rec.update(outcome="certified, deploy failed", deploy_http=dep.status_code, deploy_error=dep.json())
            elif status == 422:
                rec.update(outcome="rejected", limit=body.get("limit"), value_on_path=body.get("value_on_path"),
                           counterexample=body.get("counterexample") or [], mark=body.get("mark"))
            elif status == 409:
                rec.update(outcome=body.get("error", "refused"), detail=body)
            elif status == 400:
                rec.update(outcome="static_error", line=body.get("line"), message=body.get("message"))
            else:
                rec.update(outcome=f"http {status}", detail=body)
            self.programs.append(rec)
            emit("program", {"lane": "bounded", **{k: v for k, v in rec.items() if k not in ("certificate", "counterexample", "deploy", "detail")},
                             "counterexample_len": len(rec.get("counterexample") or [])})
            return rec

    def _cancel(self, emit) -> dict:
        if not self.live:
            return {"outcome": "nothing to cancel"}
        pid, bk = self.live["pid"], self.live["bk"]
        r = self.w.http.post(f"/program/{pid}/cancel", headers=self.H(bk))
        body = r.json() if r.content else {}
        emit("program.cancel", {"lane": "bounded", "program_id": pid, "http": r.status_code, "flatten_pending": body.get("flatten_pending")})
        return {"outcome": "cancel requested" if r.status_code == 200 else f"cancel refused ({r.status_code} {body.get('error')})",
                "program_id": pid, "http": r.status_code, "detail": body}

    # ---- sync ----------------------------------------------------------------------------------
    def sync(self, last: int | None, emit) -> dict:
        """Reads first, with no lock held (the gateway's state route and the bank can be slow); then the
        state changes under the lock. The page's snapshot only ever waits for the second part."""
        live = self.live
        st = None
        if live:
            r = self.w.http.get(f"/program/{live['pid']}/state", headers=self.H(live["bk"]))
            if r.status_code == 200:
                st = r.json()
        nessie = getattr(self.w.bank, "name", "") == "nessie"
        want_view = (not nessie) or time.monotonic() - getattr(self, "_view_t", 0.0) >= 10.0 or (st is not None and st.get("status") in ("halted", "cancelled"))
        view = None
        if want_view:
            view = self.w.http.get(f"/account/{self.account}", headers=self.H(self._ok)).json()
        with self.lock:
            fills = self._drain_events(emit)
            if self.live is live and live and st is not None:
                self.live["state"] = st
                if st.get("status") in ("halted", "cancelled"):
                    ended_t = self.w.now_ms()
                    signed = None
                    try:
                        signed = int(self.w.venue.state(live["pid"]).get("realized_loss_cents"))   # signed: negative is a profit
                    except Exception:
                        pass
                    if signed is None:
                        signed = int((st.get("realized") or {}).get("loss_cents") or 0)
                    for p in reversed(self.programs):
                        if p.get("program_id") == live["pid"]:
                            p.update(ended=ended_t, final_status=st.get("status"), realized=st.get("realized"), signed_loss_cents=signed)
                            break
                    emit("program.ended", {"lane": "bounded", "program_id": live["pid"], "status": st.get("status"),
                                           "realized": st.get("realized"), "bound": live["bound"]})
                    self.live = None
            if view is not None:
                self.last_view = view
                self._view_t = time.monotonic()
        if view is not None or not nessie or time.monotonic() - getattr(self, "_bank_t", 0.0) >= 5.0:
            self._refresh_bank()
            self._bank_t = time.monotonic()
        return {"fills": fills, "equity": self.equity()}

    def _drain_events(self, emit) -> list[dict]:
        out = []
        with self.w.db.connect() as c:
            rows = c.execute("SELECT id, type, payload_json FROM events WHERE account=? AND id>? ORDER BY id", (self.account, self._events_after)).fetchall()
        for r in rows:
            self._events_after = int(r["id"])
            if r["type"] != "node.filled":
                continue
            p = json.loads(r["payload_json"])
            pos = None
            if self.live and self.live.get("state"):
                pos = self.live["state"].get("position")
            side = self._side_of_node(p.get("program_id"), p.get("node"))
            row = {"lane": "bounded", "t": self.w.now_ms(), "ref": f"p{p.get('program_id')}:{p.get('node')}", "label": p.get("node"),
                   "side": side, "qty": int(p.get("qty") or 0), "price": int(p.get("price") or 0), "fee_cents": int(p.get("fee_cents") or 0),
                   "maker": p.get("node") not in ("sl", "flatten") and not str(p.get("node", "")).startswith("sl"), "position_after": pos}
            self.fills_n += 1
            self.store.add_fill(self.epoch, "bounded", row["t"], row["ref"], side or "?", row["qty"], row["price"], row["fee_cents"], row["maker"], pos or 0)
            out.append(row)
            emit("fill", row)
        return out

    def _side_of_node(self, pid, node) -> str | None:
        src = None
        for p in reversed(self.programs):
            if p.get("program_id") == pid:
                src = p.get("compiled") or None
                break
        if self.live and self.live["pid"] == pid and not src:
            try:
                prog = self.w.http.get(f"/program/{pid}", headers=self.H(self._ak)).json()
                for p in reversed(self.programs):
                    if p.get("program_id") == pid:
                        p["compiled"] = prog.get("compiled")
                        src = prog.get("compiled")
                        break
            except Exception:
                pass
        if src:
            for n in src.get("nodes", []):
                if n.get("id") == node:
                    return n.get("side")
            if node == "flatten":
                ent = next((n for n in src.get("nodes", []) if n.get("id") == "entry"), None)
                if ent:
                    return "sell" if ent.get("side") == "buy" else "buy"
        return None

    # ---- views ---------------------------------------------------------------------------------
    def _escrow_cents(self) -> int:
        """Money of this account parked in program escrows right now (0 once a program has settled)."""
        try:
            with self.w.db.connect() as c:
                rows = c.execute("SELECT escrow_account_id FROM programs WHERE account=? AND escrow_account_id IS NOT NULL AND settled_t IS NULL",
                                 (self.account,)).fetchall()
            return sum(int(self.w.bank.balance(r["escrow_account_id"]) or 0) for r in rows)
        except Exception:
            return int((self.live or {}).get("fund_cents") or 0) if self.live else 0

    def _refresh_bank(self) -> None:
        """(balance, unsettled escrows) read together from the bank. Called by `sync` with no lock held:
        with Nessie each read is a paced HTTP call, and nothing that serves the page may wait on it."""
        acct = (self.last_view or {}).get("bank_account_id")
        try:
            bal = int(self.w.bank.balance(acct)) if acct else int((self.last_view or {}).get("balance_cents") or 0)
        except Exception:
            bal = int((self.last_view or {}).get("balance_cents") or 0)
        esc = self._escrow_cents()
        self._bank_cache = (time.monotonic(), bal, esc)

    def _bank_now(self) -> tuple[int, int]:
        c = getattr(self, "_bank_cache", None)
        if c:
            return c[1], c[2]
        return int((self.last_view or {}).get("balance_cents") or 0), (int(self.live.get("fund_cents") or 0) if self.live else 0)

    def equity(self) -> int:
        """Capital - the venue's signed realized result of every ended program - the live program's
        mark-to-market loss. Exact at every instant and independent of the bank's settlement timing;
        the bank balance is shown next to it as the cross-check (`bank_equity_cents`)."""
        realized = sum(int(p.get("signed_loss_cents") if p.get("signed_loss_cents") is not None else ((p.get("realized") or {}).get("loss_cents") or 0))
                       for p in self.programs if p.get("ended"))
        mtm = 0
        if self.live:
            st = self.live.get("state") or {}
            mtm = int(st.get("loss_cents") or 0) if st else 0
        return self.capital - realized - mtm

    def bank_equity(self) -> int:
        """What the bank says: balance + unsettled escrows (lags the venue by Nessie's settlement sweep)."""
        bal, esc = self._bank_now()
        return bal + esc

    def snapshot(self, last: int | None) -> dict:
        with self.lock:
            v = self.last_view or {}
            budget = v.get("budget") or {}
            live = None
            if self.live:
                st = self.live.get("state") or {}
                live = {"program_id": self.live["pid"], "bound": self.live["bound"], "fund_cents": self.live.get("fund_cents"),
                        "deployed_t": self.live["deployed_t"], "sub_account": self.live.get("sub_account"),
                        "position": st.get("position", 0), "mtm_loss_cents": st.get("loss_cents"), "fees_cents": st.get("fees_cents"),
                        "trades": st.get("trades"), "live_nodes": st.get("live_nodes"), "status": st.get("status"), "source": self.live.get("source")}
            recent = []
            for p in self.programs[-12:]:
                recent.append({k: p.get(k) for k in ("t", "http", "program_id", "outcome", "violates", "limit", "value_on_path", "bound",
                                                     "error", "message", "line", "final_status", "realized", "ended")})
                recent[-1]["source"] = p.get("source")
                recent[-1]["counterexample_len"] = len(p.get("counterexample") or [])
            realized = sum(int((p.get("realized") or {}).get("loss_cents") or 0) for p in self.programs)
            fees = sum(int((p.get("realized") or {}).get("fees_cents") or 0) for p in self.programs) + \
                int(((self.live or {}).get("state") or {}).get("fees_cents") or 0)
            trades = sum(int((p.get("realized") or {}).get("trades") or 0) for p in self.programs) + \
                int(((self.live or {}).get("state") or {}).get("trades") or 0)
            return {
                "lane": "bounded", "account": self.account, "capital_cents": self.capital, "equity_cents": self.equity(),
                "pnl_cents": self.equity() - self.capital, "balance_cents": v.get("balance_cents"), "bank": v.get("bank"),
                "bank_equity_cents": self.bank_equity(),
                "budget": budget, "envelope": self.envelope, "frozen": v.get("frozen"), "live_program": live,
                "position": (live or {}).get("position", 0) or 0, "fees_cents": fees, "trades": trades,
                "realized_loss_cents": realized, "programs": recent, "submitted": len(self.programs),
                "certified": sum(1 for p in self.programs if p.get("http") == 200),
                "rejected": sum(1 for p in self.programs if p.get("http") == 422),
                "refused_409": sum(1 for p in self.programs if p.get("http") == 409),
                "static_errors": sum(1 for p in self.programs if p.get("http") == 400),
                "decisions": self.decisions,
            }

    def bank_ids(self) -> dict:
        """Read-only, from the gateway's own tables (no bank call): which bank backs this account
        ("nessie" = Capital One's Nessie v2 sandbox, "local-sim" = the SQLite simulator), its customer
        and account ids, and one row per program escrow of this epoch's account: the escrow account
        the gateway opened at deploy, the cents parked in it, whether the gateway has settled it, and
        the settlement transfer's id when the bank keeps one (the local simulator's bank_tx row;
        Nessie v2 returns none the gateway stores, so it is null there and `notes` says so)."""
        bank = getattr(getattr(self.w, "bank", None), "name", None)
        out: dict = {"bank": bank, "customer_id": None, "account_id": None, "escrows": []}
        if self.account is None:
            return out
        try:
            with self.w.db.connect() as c:
                a = c.execute("SELECT bank, bank_customer_id, bank_account_id FROM accounts WHERE id=?", (self.account,)).fetchone()
                if a:
                    out.update(bank=a["bank"] or bank, customer_id=a["bank_customer_id"], account_id=a["bank_account_id"])
                rows = c.execute("SELECT id, escrow_account_id, fund_cents, settled_t FROM programs WHERE account=? AND escrow_account_id IS NOT NULL ORDER BY id",
                                 (self.account,)).fetchall()
                esc = []
                for r in rows:
                    tx = None
                    if r["settled_t"]:
                        t = c.execute("SELECT id FROM bank_tx WHERE from_id=? AND description=? ORDER BY id DESC LIMIT 1",
                                      (r["escrow_account_id"], f"settle program {r['id']}")).fetchone()
                        tx = f"bank_tx:{int(t['id'])}" if t else None
                    esc.append({"program_id": int(r["id"]), "escrow_account_id": r["escrow_account_id"], "cents": int(r["fund_cents"] or 0),
                                "settled": bool(r["settled_t"]), "settlement_tx_id": tx})
                out["escrows"] = esc[-25:]
            if out["bank"] == "nessie":
                out["notes"] = ["settlement_tx_id is null on Nessie: its v2 transfer endpoint returns no id the gateway stores; "
                                "`settled` is the gateway's settled_t after escrow -> account and escrow -> clearing transfers"]
        except Exception as e:
            out["error"] = f"{type(e).__name__}: {e}"
        return out

    def describe_for_prompt(self, last: int | None) -> str:
        s = self.snapshot(last)
        b = s["budget"] or {}
        lines = [f"Your account (bounded key, gateway account {s['account']}): equity ${s['equity_cents'] / 100:,.2f} (start $10,000.00); "
                 f"bank balance ${(s['balance_cents'] or 0) / 100:,.2f}; fees paid ${s['fees_cents'] / 100:,.2f}; {s['trades']} fills.",
                 f"Envelope budget left in the 24 h window: loss ${(b.get('remaining_loss_cents') or 0) / 100:,.2f} of "
                 f"${self.envelope['max_loss_cents'] / 100:,.2f}, fees ${(b.get('remaining_fees_cents') or 0) / 100:,.2f}, "
                 f"trades {b.get('remaining_trades')}, exposure {b.get('remaining_exposure_lots')} lot(s)."]
        lp = s["live_program"]
        if lp:
            lines.append(f"Live program #{lp['program_id']}: certified max loss ${lp['bound']['max_loss_cents'] / 100:,.2f}, "
                         f"position {lp['position']:+d}, live nodes {lp['live_nodes']}, mark-to-market P&L ${-(lp['mtm_loss_cents'] or 0) / 100:,.2f}. "
                         "You can `cancel` it (it flattens at the next print) or `hold`; a new program on ETH is refused (409 instrument_busy) while it runs.")
        else:
            lines.append("No live program: you are flat. Submit a program to trade, or hold.")
        return "\n".join(lines)
