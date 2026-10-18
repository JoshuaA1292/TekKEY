"""SQLite for the race, in the race's own database (the bounded lane's gateway lives in the same
file, so a round's program ids point at rows next door). Everything the page shows is read back
from here or from the exchange; `race_events` is the stream, replayable from `Last-Event-ID`."""
from __future__ import annotations

import json
import threading
import time
from typing import Any

from gateway.db import Database, now_ms

SCHEMA = """
CREATE TABLE IF NOT EXISTS race_epochs (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  started_t INTEGER NOT NULL,
  ended_t INTEGER,
  config_json TEXT NOT NULL,
  final_json TEXT
);
CREATE TABLE IF NOT EXISTS race_prints (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  epoch INTEGER NOT NULL,
  t INTEGER NOT NULL,
  price REAL NOT NULL,
  ticks INTEGER NOT NULL,
  source TEXT NOT NULL,
  gap INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS race_prints_epoch ON race_prints(epoch, id);
CREATE TABLE IF NOT EXISTS race_rounds (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  epoch INTEGER NOT NULL,
  n INTEGER NOT NULL,
  t INTEGER NOT NULL,
  ended_t INTEGER,
  shared_json TEXT NOT NULL,
  raw_json TEXT,
  bounded_json TEXT
);
CREATE INDEX IF NOT EXISTS race_rounds_epoch ON race_rounds(epoch, n);
CREATE TABLE IF NOT EXISTS race_fills (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  epoch INTEGER NOT NULL,
  lane TEXT NOT NULL,
  t INTEGER NOT NULL,
  ref TEXT,
  side TEXT NOT NULL,
  qty INTEGER NOT NULL,
  price INTEGER NOT NULL,
  fee_cents INTEGER NOT NULL,
  maker INTEGER NOT NULL,
  position_after INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS race_fills_epoch ON race_fills(epoch, id);
CREATE TABLE IF NOT EXISTS race_equity (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  epoch INTEGER NOT NULL,
  t INTEGER NOT NULL,
  price INTEGER NOT NULL,
  raw_equity INTEGER NOT NULL,
  bnd_equity INTEGER NOT NULL,
  raw_pos INTEGER NOT NULL,
  bnd_pos INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS race_equity_epoch ON race_equity(epoch, id);
CREATE TABLE IF NOT EXISTS race_messages (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  epoch INTEGER NOT NULL,
  t INTEGER NOT NULL,
  source TEXT NOT NULL,          -- owner | market | attack | visitor
  cls TEXT,
  text TEXT NOT NULL,
  round_n INTEGER
);
CREATE TABLE IF NOT EXISTS race_raw_orders (   -- the raw lane's order ids, so a restart can cancel what still rests
  order_id INTEGER PRIMARY KEY,
  epoch INTEGER NOT NULL,
  account INTEGER NOT NULL,
  ref TEXT,
  spec_json TEXT,
  live INTEGER NOT NULL DEFAULT 1,
  t INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS race_events (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  type TEXT NOT NULL,
  data_json TEXT NOT NULL,
  t INTEGER NOT NULL
);
"""


class RaceStore:
    def __init__(self, db: Database):
        self.db = db
        self._lock = threading.Lock()
        with db.connect() as c:
            c.executescript(SCHEMA)

    # ---- epochs ------------------------------------------------------------------------------
    def open_epoch(self, config: dict) -> int:
        with self.db.tx() as c:
            cur = c.execute("INSERT INTO race_epochs(started_t, config_json) VALUES (?,?)", (now_ms(), json.dumps(config)))
            return int(cur.lastrowid)

    def close_epoch(self, epoch: int, final: dict) -> None:
        with self.db.tx() as c:
            c.execute("UPDATE race_epochs SET ended_t=?, final_json=? WHERE id=? AND ended_t IS NULL", (now_ms(), json.dumps(final), epoch))

    def close_dangling(self, reason: str, max_gap: int, capital_cents: int) -> list[int]:
        """Epochs a previous process left open (a restart never closes the epoch it interrupts): closed now
        with their numbers from the tables, so they stay in the comparison as finished, cut short."""
        with self.db.connect() as c:
            ids = [int(r["id"]) for r in c.execute("SELECT id FROM race_epochs WHERE ended_t IS NULL").fetchall()]
        for eid in ids:
            s = self.epoch_summary(eid, max_gap, capital_cents)
            final = {"reason": reason, "rounds": s.get("rounds", 0), "attacks": (s.get("messages") or {}).get("attack", 0), "cut_short": True,
                     "raw": {"equity_cents": s["raw"]["final_equity_cents"], "pnl_cents": s["raw"]["pnl_cents"], "fees_cents": s["raw"]["fees_cents"], "trades": s["raw"]["fills"]},
                     "bounded": {"equity_cents": s["bounded"]["final_equity_cents"], "pnl_cents": s["bounded"]["pnl_cents"], "fees_cents": s["bounded"]["fees_cents"], "trades": s["bounded"]["fills"]}}
            with self.db.tx() as c:
                c.execute("UPDATE race_epochs SET ended_t=COALESCE((SELECT MAX(t) FROM race_equity WHERE epoch=?), started_t), final_json=? WHERE id=? AND ended_t IS NULL",
                          (eid, json.dumps(final), eid))
        return ids

    def epochs(self, limit: int = 10) -> list[dict]:
        with self.db.connect() as c:
            rows = c.execute("SELECT * FROM race_epochs ORDER BY id DESC LIMIT ?", (limit,)).fetchall()
        out = []
        for r in rows:
            d = dict(r)
            d["config"] = json.loads(d.pop("config_json"))
            d["final"] = json.loads(d.pop("final_json")) if d.get("final_json") else None
            out.append(d)
        return out

    # ---- prints / fills / equity ---------------------------------------------------------------
    def add_print(self, epoch: int, t: int, price: float, ticks: int, source: str, gap: int) -> int:
        with self.db.tx() as c:
            return int(c.execute("INSERT INTO race_prints(epoch,t,price,ticks,source,gap) VALUES (?,?,?,?,?,?)",
                                 (epoch, t, price, ticks, source, gap)).lastrowid)

    def prints(self, epoch: int, limit: int = 2000, since_t: int | None = None) -> list[list]:
        with self.db.connect() as c:
            if since_t:
                rows = c.execute("SELECT t, price, ticks, gap FROM race_prints WHERE epoch=? AND t>=? ORDER BY id DESC LIMIT ?",
                                 (epoch, since_t, limit)).fetchall()
            else:
                rows = c.execute("SELECT t, price, ticks, gap FROM race_prints WHERE epoch=? ORDER BY id DESC LIMIT ?", (epoch, limit)).fetchall()
        return [[int(r["t"]), float(r["price"]), int(r["ticks"]), int(r["gap"])] for r in reversed(rows)]

    def print_stats(self, epoch: int, max_gap: int) -> dict:
        with self.db.connect() as c:
            r = c.execute("SELECT COUNT(*) AS n, MAX(gap) AS g, SUM(CASE WHEN gap>? THEN 1 ELSE 0 END) AS v, MIN(t) AS t0, MAX(t) AS t1 "
                          "FROM race_prints WHERE epoch=?", (max_gap, epoch)).fetchone()
        return {"n": int(r["n"] or 0), "max_gap": int(r["g"] or 0), "violations": int(r["v"] or 0), "from_t": r["t0"], "to_t": r["t1"]}

    def add_fill(self, epoch: int, lane: str, t: int, ref: str, side: str, qty: int, price: int, fee: int, maker: bool, pos: int) -> int:
        with self.db.tx() as c:
            return int(c.execute("INSERT INTO race_fills(epoch,lane,t,ref,side,qty,price,fee_cents,maker,position_after) VALUES (?,?,?,?,?,?,?,?,?,?)",
                                 (epoch, lane, t, ref, side, qty, price, fee, 1 if maker else 0, pos)).lastrowid)

    def fills(self, epoch: int, limit: int = 400) -> list[dict]:
        with self.db.connect() as c:
            rows = c.execute("SELECT * FROM race_fills WHERE epoch=? ORDER BY id DESC LIMIT ?", (epoch, limit)).fetchall()
        return [dict(r) | {"maker": bool(r["maker"])} for r in reversed(rows)]

    def add_equity(self, epoch: int, t: int, price: int, raw_eq: int, bnd_eq: int, raw_pos: int, bnd_pos: int) -> None:
        with self.db.tx() as c:
            c.execute("INSERT INTO race_equity(epoch,t,price,raw_equity,bnd_equity,raw_pos,bnd_pos) VALUES (?,?,?,?,?,?,?)",
                      (epoch, t, price, raw_eq, bnd_eq, raw_pos, bnd_pos))

    def equity(self, epoch: int, max_points: int = 1500) -> list[list]:
        with self.db.connect() as c:
            n = c.execute("SELECT COUNT(*) FROM race_equity WHERE epoch=?", (epoch,)).fetchone()[0]
            step = max(1, int(n) // max_points)
            rows = c.execute("SELECT id,t,price,raw_equity,bnd_equity,raw_pos,bnd_pos FROM race_equity WHERE epoch=? AND (id %% ? = 0 OR id=(SELECT MAX(id) FROM race_equity WHERE epoch=?)) ORDER BY id"
                             % (), (epoch, step, epoch)).fetchall()
        return [[int(r["t"]), int(r["price"]), int(r["raw_equity"]), int(r["bnd_equity"]), int(r["raw_pos"]), int(r["bnd_pos"])] for r in rows]

    # ---- rounds / messages ---------------------------------------------------------------------
    def open_round(self, epoch: int, n: int, shared: list[dict], t: int | None = None) -> int:
        with self.db.tx() as c:
            return int(c.execute("INSERT INTO race_rounds(epoch,n,t,shared_json) VALUES (?,?,?,?)", (epoch, n, t or now_ms(), json.dumps(shared))).lastrowid)

    def set_lane_round(self, round_id: int, lane: str, data: dict) -> None:
        col = "raw_json" if lane == "raw" else "bounded_json"
        with self.db.tx() as c:
            c.execute(f"UPDATE race_rounds SET {col}=? WHERE id=?", (json.dumps(data), round_id))

    def close_round(self, round_id: int) -> None:
        with self.db.tx() as c:
            c.execute("UPDATE race_rounds SET ended_t=? WHERE id=?", (now_ms(), round_id))

    def rounds(self, epoch: int, limit: int = 40, full: bool = False) -> list[dict]:
        with self.db.connect() as c:
            rows = c.execute("SELECT * FROM race_rounds WHERE epoch=? ORDER BY n DESC LIMIT ?", (epoch, limit)).fetchall()
        return [self._round(dict(r), full) for r in reversed(rows)]

    def round(self, round_id: int) -> dict | None:
        with self.db.connect() as c:
            r = c.execute("SELECT * FROM race_rounds WHERE id=?", (round_id,)).fetchone()
        return self._round(dict(r), True) if r else None

    @staticmethod
    def _round(d: dict, full: bool) -> dict:
        out = {"id": d["id"], "round_id": d["id"], "epoch": d["epoch"], "n": d["n"], "t": d["t"], "ended_t": d["ended_t"],
               "shared": json.loads(d["shared_json"]), "raw": json.loads(d["raw_json"]) if d.get("raw_json") else None,
               "bounded": json.loads(d["bounded_json"]) if d.get("bounded_json") else None}
        if not full:
            for lane in ("raw", "bounded"):
                if out[lane]:
                    d2 = {k: v for k, v in out[lane].items() if k not in ("prompt_system", "prompt_user", "response", "account_state")}
                    d2["exchanges"] = [{k: v for k, v in x.items() if k not in ("prompt_user", "response")} for x in (d2.get("exchanges") or [])]
                    out[lane] = d2
        return out

    def add_message(self, epoch: int, source: str, cls: str | None, text: str) -> int:
        with self.db.tx() as c:
            return int(c.execute("INSERT INTO race_messages(epoch,t,source,cls,text) VALUES (?,?,?,?,?)",
                                 (epoch, now_ms(), source, cls, text)).lastrowid)

    def take_pending_messages(self, epoch: int, round_n: int, limit: int = 3) -> list[dict]:
        with self.db.tx() as c:
            rows = c.execute("SELECT * FROM race_messages WHERE epoch=? AND round_n IS NULL AND source='visitor' ORDER BY id LIMIT ?",
                             (epoch, limit)).fetchall()
            ids = [int(r["id"]) for r in rows]
            if ids:
                c.execute(f"UPDATE race_messages SET round_n=? WHERE id IN ({','.join('?' * len(ids))})", (round_n, *ids))
        return [dict(r) for r in rows]

    def pending_count(self, epoch: int) -> int:
        with self.db.connect() as c:
            return int(c.execute("SELECT COUNT(*) FROM race_messages WHERE epoch=? AND round_n IS NULL AND source='visitor'", (epoch,)).fetchone()[0])

    # ---- events --------------------------------------------------------------------------------
    def emit(self, type_: str, data: dict | None = None) -> int:
        body = {"t": now_ms(), **(data or {})}
        with self.db.tx() as c:
            return int(c.execute("INSERT INTO race_events(type,data_json,t) VALUES (?,?,?)", (type_, json.dumps(body, default=str), body["t"])).lastrowid)

    def events_after(self, last_id: int, limit: int = 500) -> list[dict]:
        with self.db.connect() as c:
            rows = c.execute("SELECT id,type,data_json FROM race_events WHERE id>? ORDER BY id LIMIT ?", (int(last_id), limit)).fetchall()
        return [{"id": int(r["id"]), "type": r["type"], "data": r["data_json"]} for r in rows]

    def last_event_id(self) -> int:
        with self.db.connect() as c:
            return int(c.execute("SELECT MAX(id) FROM race_events").fetchone()[0] or 0)

    def prune_events(self, keep: int = 20000) -> None:
        with self.db.tx() as c:
            c.execute("DELETE FROM race_events WHERE id < (SELECT COALESCE(MAX(id),0) FROM race_events) - ?", (keep,))

    # ---- comparison -----------------------------------------------------------------------------
    def epoch_summary(self, epoch: int, max_gap: int, capital_cents: int) -> dict:
        """What the comparison table needs, computed from the rows of one epoch: P&L path (max drawdown,
        low), fills and fees per lane, prints and assumption violations, rounds and attacks, and the
        bounded lane's programs with each realized loss against its certificate (a breach is realized >
        certified; the count must be 0)."""
        with self.db.connect() as c:
            ep = c.execute("SELECT * FROM race_epochs WHERE id=?", (epoch,)).fetchone()
            if ep is None:
                return {}
            eq = c.execute("SELECT t, raw_equity, bnd_equity, raw_pos, bnd_pos FROM race_equity WHERE epoch=? ORDER BY id", (epoch,)).fetchall()
            fills = c.execute("SELECT lane, COUNT(*) AS n, SUM(fee_cents) AS fees, SUM(qty) AS lots FROM race_fills WHERE epoch=? GROUP BY lane", (epoch,)).fetchall()
            pr = c.execute("SELECT COUNT(*) AS n, MAX(gap) AS g, SUM(CASE WHEN gap>? THEN 1 ELSE 0 END) AS v, MIN(t) AS t0, MAX(t) AS t1, MIN(ticks) AS lo, MAX(ticks) AS hi FROM race_prints WHERE epoch=?", (max_gap, epoch)).fetchone()
            rounds = c.execute("SELECT n, t, raw_json, bounded_json, shared_json FROM race_rounds WHERE epoch=? ORDER BY n", (epoch,)).fetchall()
            msgs = c.execute("SELECT source, COUNT(*) AS n FROM race_messages WHERE epoch=? GROUP BY source", (epoch,)).fetchall()
            pids = set()
            raw_actions = {"orders": 0, "hold": 0, "failed": 0}
            bnd_actions = {"program": 0, "cancel": 0, "hold": 0, "failed": 0}
            http = {}
            routes_seen: dict[str, int] = {}             # the route each answered model call recorded, per round and lane
            for r in rounds:
                for col, acts in (("raw_json", raw_actions), ("bounded_json", bnd_actions)):
                    if not r[col]:
                        continue
                    d = json.loads(r[col])
                    llm = d.get("llm") or {}
                    if llm.get("route") and not llm.get("error"):
                        routes_seen[str(llm["route"])] = routes_seen.get(str(llm["route"]), 0) + 1
                    a = (d.get("decision") or {}).get("action") or "hold"
                    if "model call failed" in str((d.get("decision") or {}).get("note", "")):
                        acts["failed"] += 1
                    else:
                        acts[a] = acts.get(a, 0) + 1
                    if col == "bounded_json":
                        for x in d.get("exchanges") or []:
                            res = x.get("result") or {}
                            if res.get("http"):
                                http[str(res["http"])] = http.get(str(res["http"]), 0) + 1
                            if res.get("program_id"):
                                pids.add(int(res["program_id"]))
            progs = []
            if pids:
                q = ",".join("?" * len(pids))
                for row in c.execute(f"SELECT id, status, bound_loss, realized_loss, realized_fees, realized_trades FROM programs WHERE id IN ({q}) ORDER BY id", tuple(pids)):
                    progs.append(dict(row))
        def dd(idx):
            peak, worst, low = capital_cents, 0, capital_cents
            for row in eq:
                v = int(row[idx]); peak = max(peak, v); worst = max(worst, peak - v); low = min(low, v)
            return worst, low
        raw_dd, raw_low = dd(1); bnd_dd, bnd_low = dd(2)
        f = {r["lane"]: dict(r) for r in fills}
        final = json.loads(ep["final_json"]) if ep["final_json"] else None
        cfg = json.loads(ep["config_json"])
        ended = [p for p in progs if p["status"] in ("halted", "cancelled")]
        breaches = [p for p in ended if p["bound_loss"] is not None and int(p["realized_loss"] or 0) > int(p["bound_loss"])]
        # The route is what the model calls of this epoch recorded (each Completion carries the backend that
        # answered it); the epoch's configured route is only the fallback for an epoch with no answered call.
        route = "+".join(sorted(routes_seen)) if routes_seen else cfg.get("route")
        model = cfg.get("model")
        mode = cfg.get("mode", "live")
        label = (cfg.get("backtest") or {}).get("label")
        if not label:
            label = ("live" if mode == "live" else "backtest") + (f" on {model} via {route}" if model and route else "")
        return {
            "epoch": epoch, "started_t": ep["started_t"], "ended_t": ep["ended_t"], "finished": final is not None,
            "mode": mode, "backtest": cfg.get("backtest"), "label": label,
            "reason": (final or {}).get("reason"), "model": model, "route": route, "route_configured": cfg.get("route"),
            "routes_seen": routes_seen,
            "market": {"from_t": pr["t0"], "to_t": pr["t1"], "prints": int(pr["n"] or 0), "low": pr["lo"], "high": pr["hi"], "max_gap": int(pr["g"] or 0),
                       "violations": int(pr["v"] or 0), "max_gap_ticks": max_gap},
            "rounds": len(rounds), "messages": {r["source"]: int(r["n"]) for r in msgs},
            "raw": {"final_equity_cents": int(eq[-1][1]) if eq else capital_cents, "pnl_cents": (int(eq[-1][1]) if eq else capital_cents) - capital_cents,
                    "max_drawdown_cents": raw_dd, "low_equity_cents": raw_low, "fills": int(f["raw"]["n"] or 0) if "raw" in f else 0,
                    "fees_cents": int(f["raw"]["fees"] or 0) if "raw" in f else 0, "actions": raw_actions,
                    "liquidated": bool(((final or {}).get("raw") or {}).get("liquidated")), "breaches": ((final or {}).get("raw") or {}).get("breaches"),
                    "max_exposure": ((final or {}).get("raw") or {}).get("max_exposure")},
            "bounded": {"final_equity_cents": int(eq[-1][2]) if eq else capital_cents, "pnl_cents": (int(eq[-1][2]) if eq else capital_cents) - capital_cents,
                        "max_drawdown_cents": bnd_dd, "low_equity_cents": bnd_low, "fills": int(f["bounded"]["n"] or 0) if "bounded" in f else 0,
                        "fees_cents": int(f["bounded"]["fees"] or 0) if "bounded" in f else 0, "actions": bnd_actions, "http": http,
                        "programs": len(progs), "ended": len(ended), "certificate_breaches": len(breaches),
                        "worst_realized_vs_bound": max(((int(p["realized_loss"] or 0), int(p["bound_loss"] or 0)) for p in ended), default=None),
                        "sum_realized_loss_cents": sum(int(p["realized_loss"] or 0) for p in ended),
                        "sum_bounds_cents": sum(int(p["bound_loss"] or 0) for p in ended)},
        }

