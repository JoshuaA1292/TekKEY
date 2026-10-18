"""`ctx`: what a lab test gets (lab/CONTRACT.md §1). The gateway in-process (`ctx.gw`, and its
routes through `ctx.http`), the venue (`ctx.venue`, real or fake), the model and LLM route, the
tape, the sponsor probe, and the three sinks `log` / `metric` / `row` (plus `series`) that
persist to SQLite and stream to the page.

Every helper here goes through the gateway's HTTP routes with the right key class, exactly as
a client would: owner keys create accounts, author keys submit, program keys deploy and cancel.
Nothing bypasses the service.
"""
from __future__ import annotations

import json
import logging
import time
from pathlib import Path
from typing import Any, Callable

from fastapi.testclient import TestClient

from gateway import config
from gateway.db import now_ms

from .store import LabStore
from .tape import Tape

log = logging.getLogger("lab.context")

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "fixtures" if (ROOT / "fixtures" / "envelope_40.json").exists() else ROOT / "gateway" / "fixtures"
OPENING_BALANCE_CENTS = 100_000_00
MODEL_TESTS = ("firstpass", "injection", "race", "unplugged")


def llm_route() -> str:
    """The route `chalk.ai_gateway.get_llm()` would take right now (chalk | anthropic-direct | stub)."""
    try:
        from chalk import ai_gateway
        return getattr(ai_gateway.get_llm(), "route", "stub") or "stub"
    except Exception as e:
        return f"error: {e}"


class Result:
    """What a test builds and returns. `headline.pass` is the test's own verdict (its oracle)."""

    def __init__(self):
        self.status: str | None = None          # passed | failed | skipped | completed (stub)
        self.skipped_reason: str | None = None
        self.headline: dict | None = None
        self.metrics: dict[str, Any] = {}
        self.rows: list[dict] = []
        self.series: dict[str, list] = {}
        self.log: list[dict] = []
        self.evidence: dict[str, Any] = {}
        self.note: str | None = None

    def skip(self, reason: str) -> "Result":
        self.status, self.skipped_reason = "skipped", reason
        self.headline = {"label": "skipped", "value": None, "unit": "", "pass": None}
        return self

    def verdict(self, label: str, value, unit: str, passed: bool | None) -> "Result":
        self.headline = {"label": label, "value": value, "unit": unit, "pass": passed}
        if self.status is None:
            self.status = "passed" if passed else "failed"
        return self

    def as_dict(self) -> dict:
        return {"headline": self.headline, "metrics": self.metrics, "rows": self.rows, "series": self.series,
                "log": self.log, "evidence": self.evidence, "skipped_reason": self.skipped_reason, "note": self.note}


class Context:
    def __init__(self, app, run_id: str, test_id: str, model: str, llm, route: str, tape: Tape | None,
                 services: dict, store: LabStore, params: dict | None = None):
        self.app = app
        self.gw = app.state.gw
        self.venue = app.state.venue
        self.bank = app.state.bank
        self.photon = getattr(app.state, "photon", None)
        self.run_id, self.test_id, self.model, self.llm, self.route = run_id, test_id, model, llm, route
        self.tape, self.services, self.store = tape, services, store
        self.params = dict(params or {})
        self.result = Result()
        # the app's own lifespan already runs under the server (or the test's TestClient); this
        # client only sends requests, so it must not be entered as a context manager.
        self.http = TestClient(app, raise_server_exceptions=False)
        self._ins = self.venue.instruments()

    # ------------------------------------------------------------------ sinks
    def log(self, msg: str, **data) -> None:
        entry = {"t": now_ms(), "msg": msg, "data": data}
        self.result.log.append(entry)
        self.store.add_event(self.run_id, "run.log", {"msg": msg, "data": data})
        log.info("[%s %s] %s %s", self.test_id, self.run_id, msg, json.dumps(data, default=str) if data else "")

    def metric(self, name: str, value) -> None:
        self.result.metrics[name] = value
        self.store.add_event(self.run_id, "run.metric", {"name": name, "value": value})

    def row(self, d: dict) -> None:
        self.result.rows.append(d)
        self.store.add_event(self.run_id, "run.row", {"row": d})

    def series(self, name: str, points: list) -> None:
        self.result.series.setdefault(name, []).extend(points)
        self.store.add_event(self.run_id, "run.series", {"name": name, "points": points})

    # ------------------------------------------------------------------ facts
    @property
    def stub(self) -> bool:
        return self.route == "stub"

    @property
    def model_live(self) -> bool:
        """Is a real model answering? For a model test, its own route; for a venue-only test (route
        `none`), the route the gateway would take, so the test can choose briefs over a fixed set."""
        if self.route == "none":
            return llm_route() not in ("stub",) and not llm_route().startswith("error")
        return self.route not in ("stub", "none")

    def param(self, name: str, default):
        v = self.params.get(name, default)
        try:
            return type(default)(v) if default is not None else v
        except (TypeError, ValueError):
            return default

    def instruments(self) -> dict:
        return self._ins

    def tick_cents(self, instrument: str = "ETH") -> int:
        return int(self._ins[instrument]["tick_cents"])

    def mark(self, instrument: str = "ETH") -> int:
        return int(self.venue.mark(instrument))

    def envelope_40(self) -> dict:
        return json.loads((FIXTURES / "envelope_40.json").read_text())

    def fixture(self, name: str) -> str:
        return (FIXTURES / name).read_text()

    @staticmethod
    def H(key: str) -> dict:
        return {"Authorization": f"Bearer {key}"}

    # ------------------------------------------------------------------ the gateway, by key class
    def new_account(self, envelope: dict, opening_balance_cents: int = OPENING_BALANCE_CENTS,
                    owner_phone: str | None = None) -> dict:
        """Owner key: create, set the envelope, mint an author key."""
        body = {"opening_balance_cents": opening_balance_cents}
        if owner_phone:
            body["owner_phone"] = owner_phone
        r = self.http.post("/account", json=body)
        if r.status_code != 200:
            raise RuntimeError(f"POST /account -> {r.status_code} {r.text[:200]}")
        acct, ok = r.json()["account"], r.json()["owner_key"]
        env = dict(envelope, account=acct)
        r = self.http.put(f"/account/{acct}/envelope", json=env, headers=self.H(ok))
        if r.status_code != 200:
            raise RuntimeError(f"PUT envelope -> {r.status_code} {r.text[:200]}")
        ak = self.http.post(f"/account/{acct}/author-key", headers=self.H(ok)).json()["author_key"]
        return {"account": acct, "ok": ok, "ak": ak, "envelope": r.json(), "bank_account_id": r.json().get("bank_account_id")}

    def submit(self, acc: dict, source: str) -> tuple[int, dict]:
        """Author key: POST /program -> (200 certificate + key | 422 | 409 | 400)."""
        r = self.http.post("/program", json={"account": acc["account"], "backend": "minsky", "source": source},
                           headers=self.H(acc["ak"]))
        return r.status_code, r.json()

    def deploy(self, pid: int, bk: str) -> tuple[int, dict]:
        r = self.http.post(f"/program/{pid}/deploy", headers=self.H(bk))
        return r.status_code, r.json()

    def program(self, pid: int, key: str) -> dict:
        return self.http.get(f"/program/{pid}", headers=self.H(key)).json()

    def account_view(self, acc: dict) -> dict:
        return self.http.get(f"/account/{acc['account']}", headers=self.H(acc["ok"])).json()

    def balance(self, acc: dict) -> int | None:
        """The account's bank balance. With Nessie, settlements complete on the bank's own sweep (up to
        ~30 s), so this waits until no ended program of the account is still unsettled."""
        if getattr(self.bank, "name", "") == "nessie":
            deadline = time.time() + 90
            while time.time() < deadline:
                with self.gw.db.connect() as c:
                    n = c.execute("SELECT COUNT(*) FROM programs WHERE account=? AND status IN ('halted','cancelled') AND settled_t IS NULL",
                                  (acc["account"],)).fetchone()[0]
                if not n:
                    break
                time.sleep(1.0)
        return self.account_view(acc).get("balance_cents")

    # ------------------------------------------------------------------ the venue, as the tests drive it
    def wait_for(self, pred: Callable[[], bool], timeout_s: float = 10.0, poll_s: float = 0.02) -> bool:
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            if pred():
                return True
            time.sleep(poll_s)
        return bool(pred())

    def drive(self, pid: int, path: list | None = None, steps: int | None = None) -> dict | None:
        """Play environment moves for a deployed program: the remembered worst path (set by the
        gateway at deploy) or `path`. RealVenue: `replay --attach`; FakeVenue: scripted playback."""
        if path is not None:
            self.venue.set_path(pid, path)
        return self.venue.drive(pid, steps) if steps is not None else self.venue.drive(pid)

    def wait_ended(self, pid: int, key: str, timeout_s: float = 10.0) -> dict:
        self.wait_for(lambda: self.program(pid, key)["status"] in ("halted", "cancelled"), timeout_s)
        return self.program(pid, key)

    def ended_or_settle(self, pid: int, bk: str, ak: str, timeout_s: float = 10.0) -> tuple[dict, str]:
        """After a drive: if the venue says the program ended, wait for the ledger to record it;
        otherwise cancel it (and give the venue the print it flattens on). Returns (program, how)."""
        try:
            ended = self.venue.state(pid).get("status") in ("halted", "cancelled")
        except Exception:
            ended = False
        if ended:
            p = self.wait_ended(pid, ak, timeout_s)
            if p["status"] in ("halted", "cancelled"):
                return p, "halt"
        return self.settle(pid, bk, ak, timeout_s), "cancel"

    def settle(self, pid: int, bk: str, ak: str, timeout_s: float = 10.0) -> dict:
        """End a program that is still live: cancel with its key (CONTRACT §9 A3: the venue
        flattens natively on the next print, so give it one), then wait for the ledger."""
        p = self.program(pid, ak)
        if p["status"] in ("halted", "cancelled"):
            return p
        r = self.http.post(f"/program/{pid}/cancel", headers=self.H(bk))
        if r.status_code == 200 and r.json().get("flatten_pending"):
            try:
                last = int(self.venue.state(pid).get("last_print") or self.mark())
            except Exception:
                last = self.mark()
            self.drive(pid, [{"env": "print", "at": last}, {"env": "print", "at": last}])
        return self.wait_ended(pid, ak, timeout_s)

    def walk_mark_to(self, target: int, instrument: str = "ETH", max_step: int = 25) -> dict:
        """Move the live book's last print to `target` with environment prints only, in steps
        inside the gap model, carried by a throwaway 'walker' program whose entry sits far
        below the walk (so it never fills), then cancelled. On the fake venue the mark is simply
        the last env step; on the real venue these are real prints by the environment accounts.
        Returns {from, to, steps, program_id}."""
        start = self.mark(instrument)
        if start == target:
            return {"from": start, "to": target, "steps": 0, "program_id": None}
        env = dict(self.envelope_40(), instruments=[instrument])
        acc = self.new_account(env)
        lo = max(30, min(start, target) // 10)
        src = (f"instrument {instrument}\nentry buy 1 at {lo}\n"
               f"sl: exit all at entry - 1 allow {env['env']['max_gap_ticks']} when price <= entry - 1  oco tp\n"
               f"tp: take all at entry + 1  oco sl\n")
        status, body = self.submit(acc, src)
        if status != 200:
            raise RuntimeError(f"walker did not certify: {status} {json.dumps(body)[:200]}")
        pid, bk = body["program_id"], body["key"]
        status, body = self.deploy(pid, bk)
        if status != 200:
            raise RuntimeError(f"walker did not deploy: {status} {body}")
        path = [{"env": "print", "at": start}]
        p = start
        while p != target:
            q = p + max(-max_step, min(max_step, target - p))
            path.append({"env": "print", "at": q} if abs(q - p) <= 1 else {"env": "gap", "from": p, "to": q})
            p = q
        self.drive(pid, path)
        self.settle(pid, bk, acc["ak"])
        return {"from": start, "to": self.mark(instrument), "steps": len(path) - 1, "program_id": pid}

    def close(self) -> None:
        try:
            self.http.close()
        except Exception:
            pass
