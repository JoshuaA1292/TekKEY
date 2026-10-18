"""B4: `fake_venue`. The same interface as RealVenue, with no C++ behind it.

- `compile` / `verify`: if the MPL matches a fixture by source hash, the pinned fixture file is
  returned verbatim; otherwise the Python model in `mpl.py` / `fake_verify.py` answers.
- `venued`: deploy returns ids; state and events come from the program's worst path, played on a
  timer (FAKE_STEP_MS) or instantly with `drive()`. State is persisted to a JSON file so a
  gateway restart can `reconcile`.

It must be good enough that B-agents never wait on Build A. It is not the verifier.
"""
from __future__ import annotations

import json
import logging
import math
import queue
import threading
import time
from pathlib import Path
from typing import Iterator

from . import config, fake_verify, mpl
from .venue import VenueError

log = logging.getLogger("gateway.fake_venue")

DEFAULT_MARK = 3010


def _now():
    return int(time.time() * 1000)


class FakeVenue:
    name = "fake"

    def __init__(self, fixtures_dir: Path | None = None, state_path: str | None = None,
                 autoplay: bool | None = None, step_ms: int | None = None):
        self.fixtures = Path(fixtures_dir or config.fixtures_dir())
        self.state_path = state_path if state_path is not None else str(Path(config.DB_PATH).parent / "fake_venue.json")
        self.autoplay = config.FAKE_AUTOPLAY if autoplay is None else autoplay
        self.step_ms = config.FAKE_STEP_MS if step_ms is None else step_ms
        self._ins = json.loads((self.fixtures / "instruments.json").read_text()) if (self.fixtures / "instruments.json").exists() \
            else json.loads(Path(config.instruments_path()).read_text())
        self._fixtures_by_hash = self._load_fixtures()
        self._lock = threading.RLock()
        self._events: queue.Queue = queue.Queue()
        self.programs: dict[int, dict] = {}
        self.marks: dict[str, int] = {}
        self._next_sub = 1
        self._load_state()
        self._stop = threading.Event()
        self._player = threading.Thread(target=self._autoplay_loop, name="fake-venue-player", daemon=True)
        if self.autoplay:
            self._player.start()

    # ---- fixtures ---------------------------------------------------------------------
    def _load_fixtures(self) -> dict[str, dict]:
        out = {}
        for mpl_file in self.fixtures.glob("*.mpl"):
            stem = mpl_file.stem
            src = mpl_file.read_text()
            entry = {"stem": stem, "source": src}
            cj = self.fixtures / f"{stem}.compiled.json"
            rj = self.fixtures / f"{stem}.result.json"
            if cj.exists():
                entry["compiled"] = json.loads(cj.read_text())
            if rj.exists():
                entry["result"] = json.loads(rj.read_text())
            out[mpl.source_hash(src)] = entry
            out[mpl.source_hash(_normalize(src))] = entry
        return out

    def _fixture(self, source: str) -> dict | None:
        return self._fixtures_by_hash.get(mpl.source_hash(source)) or \
            self._fixtures_by_hash.get(mpl.source_hash(_normalize(source)))

    def instruments(self) -> dict:
        return self._ins

    # ---- CLIs -------------------------------------------------------------------------
    def compile(self, source: str, env: dict) -> tuple[int, dict]:
        fx = self._fixture(source)
        if fx and "result" in fx and fx["result"].get("status") == "static_error":
            return 2, fx["result"]
        if fx and "compiled" in fx:
            c = dict(fx["compiled"])
            c["source_hash"] = mpl.source_hash(source)
            return 0, c
        try:
            return 0, mpl.compile_program(source, self._ins, env)
        except mpl.StaticError as e:
            return 2, e.body()

    def verify(self, compiled: dict, envelope: dict, mark: int, timeout_ms: int) -> tuple[int, dict]:
        for fx in self._fixtures_by_hash.values():
            if fx.get("compiled") and fx.get("result") and fx["compiled"].get("source_hash") == compiled.get("source_hash") \
                    and fx["result"].get("status") in ("certified", "rejected") \
                    and _same_envelope(fx["result"], envelope):
                r = json.loads(json.dumps(fx["result"]))
                return (0 if r["status"] == "certified" else 3), r
        return fake_verify.verify(compiled, envelope, mark, self._ins)

    def replay(self, compiled: dict, path: list) -> dict:
        """Run a path on a scratch program and report what it cost."""
        nodes = {n["id"]: n for n in compiled["nodes"]}
        tick = int(self._ins[nodes["entry"]["instrument"]]["tick_cents"])
        cash = fees = trades = 0
        position = 0
        last = None
        reported = None
        for step in path:
            if "env" in step:
                last = step.get("at", step.get("to"))
            elif step.get("event") == "filled":
                n = nodes.get(step["node"], {"side": "sell"})
                sgn = -1 if n["side"] == "buy" else 1
                cash += sgn * step["price"] * step["qty"] * tick
                position += -sgn * step["qty"]
                fees += step["fee_cents"]
                trades += 1
            elif step.get("event") == "mark":
                reported = step["loss_cents"]
        mtm = -(cash + position * (last or 0) * tick)
        loss = mtm + fees
        return {"contract_version": 1, "loss_cents": loss, "fees_cents": fees, "trades": trades,
                "matches_verifier": reported is None or reported == loss, "engine": "fake_venue"}

    # ---- venued ---------------------------------------------------------------------
    def mark(self, instrument: str) -> int:
        return self.marks.get(instrument, DEFAULT_MARK)

    def deploy(self, program_id: int, account: int, compiled: dict, fund_cents: int, path: list | None = None) -> dict:
        with self._lock:
            if program_id in self.programs:
                raise VenueError("already_deployed", str(program_id))
            sub = self._next_sub
            self._next_sub = self._next_sub % 254 + 1
            order_ids = {n["id"]: 1000 * program_id + i + 1 for i, n in enumerate(compiled["nodes"]) if n["kind"] != "halt"}
            prog = {
                "program_id": program_id, "account": account, "sub_account": sub, "fund_cents": fund_cents,
                "compiled": compiled, "order_ids": order_ids, "status": "deployed",
                "nodes": {n["id"]: ("live" if n["parent"] is None else "held") for n in compiled["nodes"] if n["kind"] != "halt"},
                "position": 0, "cash": 0, "fees": 0, "trades": 0, "last_print": self.mark(compiled["nodes"][0]["instrument"]),
                "path": path or [], "cursor": 0, "deployed_t": _now(), "ended_t": None,
            }
            self.programs[program_id] = prog
            self._save_state()
        self._emit({"type": "program.deployed", "program_id": program_id, "sub_account": sub, "order_ids": order_ids})
        return {"ok": True, "sub_account": sub, "order_ids": order_ids,
                "locked_collateral_cents": compiled["report"]["locked_collateral_cents"]}

    def set_path(self, program_id: int, path: list) -> None:
        with self._lock:
            self.programs[program_id]["path"] = path

    def _live(self, p: dict) -> list[str]:
        return [k for k, v in p["nodes"].items() if v in ("live", "triggered")]

    def _held(self, p: dict) -> list[str]:
        return [k for k, v in p["nodes"].items() if v == "held"]

    def pause(self, program_id: int) -> dict:
        with self._lock:
            p = self._get_live(program_id)
            cancelled = []
            nodes = {n["id"]: n for n in p["compiled"]["nodes"]}
            for nid, st in list(p["nodes"].items()):
                n = nodes[nid]
                # CONTRACT §9 A2: cancel every node that is not reduce-only (entry, scale, hedge);
                # every reduce-only node stays, stops and takes alike.
                if st in ("live", "held") and not n["reduce_only"]:
                    p["nodes"][nid] = "cancelled"
                    cancelled.append(nid)
            # children of a cancelled, unfilled parent can never be released
            for nid in list(p["nodes"]):
                par = nodes[nid]["parent"]
                if p["nodes"][nid] == "held" and par and p["nodes"].get(par) == "cancelled":
                    p["nodes"][nid] = "cancelled"
                    cancelled.append(nid)
            p["status"] = "paused"
            halted = self._maybe_halt(p)
            self._save_state()
        for nid in cancelled:
            self._emit({"type": "node.cancelled", "program_id": program_id, "node": nid})
        if halted:
            self._emit(halted)
        return {"ok": True, "cancelled": cancelled}

    def cancel(self, program_id: int) -> dict:
        with self._lock:
            p = self._get_live(program_id)
            cancelled = [nid for nid, st in p["nodes"].items() if st in ("live", "held", "triggered")]
            for nid in cancelled:
                p["nodes"][nid] = "cancelled"
            flatten = None
            events = [{"type": "node.cancelled", "program_id": program_id, "node": nid} for nid in cancelled]
            if p["position"] != 0:
                tick = self._tick(p)
                qty = abs(p["position"])
                px = p["last_print"]
                taker = 3.5
                f = fake_verify.fee(px, qty, tick, taker)
                side_sign = 1 if p["position"] > 0 else -1     # long -> sell
                p["cash"] += side_sign * px * qty * tick
                p["position"] = 0
                p["fees"] += f
                p["trades"] += 1
                flatten = {"qty": qty, "price": px, "fee_cents": f}
                events.append({"type": "node.filled", "program_id": program_id, "node": "flatten", "qty": qty,
                               "price": px, "fee_cents": f})
            p["status"] = "cancelled"
            p["ended_t"] = _now()
            events.append(self._halt_event(p))
            self._save_state()
        for ev in events:
            self._emit(ev)
        # CONTRACT §9 A3: the live venue flattens natively on the next print; the fill and the halt
        # arrive on the event stream. The fake does it at the last print, and reports it the same way.
        return {"ok": True, "cancelled": cancelled, "flatten_fill": None, "flatten_pending": flatten is not None}

    def state(self, program_id: int) -> dict:
        with self._lock:
            p = self.programs.get(program_id)
            if p is None:
                raise VenueError("unknown_program", str(program_id))
            return self._state_of(p)

    def _state_of(self, p: dict) -> dict:
        tick = self._tick(p)
        mtm = -(p["cash"] + p["position"] * p["last_print"] * tick)
        return {"ok": True, "program_id": p["program_id"], "status": p["status"], "live_nodes": self._live(p),
                "held_nodes": self._held(p), "position": p["position"],
                "realized_loss_cents": max(0, -(p["cash"]) + p["fees"]) if p["position"] == 0 else 0,
                "mtm_loss_cents": mtm + p["fees"], "fees_cents": p["fees"], "trades": p["trades"],
                "last_print": p["last_print"], "sub_account": p["sub_account"]}

    def reconcile(self) -> list[dict]:
        with self._lock:
            return [self._state_of(p) for p in self.programs.values()]

    def subscribe(self) -> Iterator[dict]:
        while True:
            ev = self._events.get()
            if ev is None:
                return
            yield ev

    def close(self):
        self._stop.set()
        self._events.put(None)

    # ---- playback ---------------------------------------------------------------------
    def drive(self, program_id: int, steps: int | None = None) -> None:
        """Play the program's path now (all of it, or `steps` entries)."""
        n = 0
        while True:
            if steps is not None and n >= steps:
                return
            if not self._step(program_id):
                return
            n += 1

    def _autoplay_loop(self):
        while not self._stop.is_set():
            time.sleep(max(self.step_ms, 50) / 1000)
            with self._lock:
                ids = [pid for pid, p in self.programs.items() if p["status"] in ("deployed", "paused")]
            for pid in ids:
                try:
                    self._step(pid)
                except Exception:   # the player must never die
                    log.exception("fake venue step failed for %s", pid)

    def _step(self, program_id: int) -> bool:
        events = []
        with self._lock:
            p = self.programs.get(program_id)
            if p is None or p["status"] not in ("deployed", "paused"):
                return False
            path = p["path"]
            if p["cursor"] >= len(path):
                halted = self._maybe_halt(p)
                if halted:
                    events.append(halted)
                    self._save_state()
                else:
                    return False
            else:
                step = path[p["cursor"]]
                p["cursor"] += 1
                events.extend(self._apply(p, step))
                if p["cursor"] >= len(path):
                    halted = self._maybe_halt(p)
                    if halted:
                        events.append(halted)
                self._save_state()
        for ev in events:
            self._emit(ev)
        return True

    def _apply(self, p: dict, step: dict) -> list[dict]:
        pid = p["program_id"]
        nodes = {n["id"]: n for n in p["compiled"]["nodes"]}
        ins = nodes["entry"]["instrument"]
        out = []
        if "env" in step:
            px = step.get("at", step.get("to"))
            p["last_print"] = px
            self.marks[ins] = px
            out.append({"type": "print", "instrument": ins, "price": px})
        elif step.get("event") == "triggered":
            nid = step["node"]
            if p["nodes"].get(nid) == "live":
                p["nodes"][nid] = "triggered"
                out.append({"type": "node.triggered", "program_id": pid, "node": nid, "print": p["last_print"]})
        elif step.get("event") == "filled":
            nid = step["node"]
            if p["nodes"].get(nid) in ("live", "triggered"):
                n = nodes[nid]
                tick = self._tick(p)
                sgn = -1 if n["side"] == "buy" else 1
                p["cash"] += sgn * step["price"] * step["qty"] * tick
                p["position"] += -sgn * step["qty"]
                p["fees"] += step["fee_cents"]
                p["trades"] += 1
                p["nodes"][nid] = "filled"
                out.append({"type": "node.filled", "program_id": pid, "node": nid, "qty": step["qty"],
                            "price": step["price"], "fee_cents": step["fee_cents"]})
                # release children; cancel OCO siblings
                for cid, c in nodes.items():
                    if c["parent"] == nid and p["nodes"].get(cid) == "held":
                        p["nodes"][cid] = "live"
                    if n.get("oco") and c.get("oco") == n["oco"] and cid != nid and p["nodes"].get(cid) in ("live", "held", "triggered"):
                        p["nodes"][cid] = "cancelled"
                        out.append({"type": "node.cancelled", "program_id": pid, "node": cid, "reason": "oco"})
        # rests / mark: nothing changes
        return out

    def _maybe_halt(self, p: dict) -> dict | None:
        if p["status"] in ("halted", "cancelled"):
            return None
        if p["position"] == 0 and not self._live(p) and not self._held(p):
            p["status"] = "halted"
            p["ended_t"] = _now()
            return self._halt_event(p)
        return None

    def _halt_event(self, p: dict) -> dict:
        loss = -(p["cash"])
        return {"type": "program.halted", "program_id": p["program_id"], "status": p["status"],
                "realized_loss_cents": max(0, loss + p["fees"]), "fees_cents": p["fees"], "trades": p["trades"]}

    def _get_live(self, program_id: int) -> dict:
        p = self.programs.get(program_id)
        if p is None:
            raise VenueError("unknown_program", str(program_id))
        if p["status"] not in ("deployed", "paused"):
            raise VenueError("not_live", p["status"])
        return p

    def _tick(self, p: dict) -> int:
        return int(self._ins[p["compiled"]["nodes"][0]["instrument"]]["tick_cents"])

    def _emit(self, ev: dict) -> None:
        ev = {"contract_version": 1, **ev}
        ev.setdefault("t", _now())
        self._events.put(ev)

    # ---- persistence --------------------------------------------------------------------
    def _save_state(self):
        if not self.state_path:
            return
        try:
            Path(self.state_path).parent.mkdir(parents=True, exist_ok=True)
            tmp = self.state_path + ".tmp"
            with open(tmp, "w") as f:
                json.dump({"programs": self.programs, "marks": self.marks, "next_sub": self._next_sub}, f)
            Path(tmp).replace(self.state_path)
        except OSError as e:
            log.warning("fake venue state not saved: %s", e)

    def _load_state(self):
        if not self.state_path or not Path(self.state_path).exists():
            return
        try:
            data = json.loads(Path(self.state_path).read_text())
            self.programs = {int(k): v for k, v in data.get("programs", {}).items()}
            self.marks = data.get("marks", {})
            self._next_sub = data.get("next_sub", 1)
        except (OSError, ValueError) as e:
            log.warning("fake venue state not loaded: %s", e)


def _normalize(src: str) -> str:
    return "\n".join(" ".join(line.split()) for line in src.strip().splitlines() if line.strip()) + "\n"


def _same_envelope(result: dict, envelope: dict) -> bool:
    """A pinned result only applies to the envelope it was produced under."""
    chk = result.get("envelope_checked")
    if not chk:
        return True
    return all(int(envelope.get(k, -1)) == int(v) for k, v in chk.items())
