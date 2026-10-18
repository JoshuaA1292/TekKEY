"""The trusted core behind every route: who may ask for what, how much has the account spent,
and who needs to know. `app.py` is a thin HTTP layer over this class; tests call it directly."""
from __future__ import annotations

import json
import logging
import threading
import time
from typing import Callable

from . import budget as budget_mod
from . import config, keys
from .db import Database, jloads, now_ms, row_to_dict
from .errors import ApiError
from .nessie import BankError
from .venue import VenueError, Venue

log = logging.getLogger("gateway.service")

ENVELOPE_REQUIRED = ("max_loss_cents", "max_fees_cents", "max_trades", "max_exposure_lots", "instruments", "env")
DERIVED_EVENTS = ("budget.low", "agent.offline", "rejections.burst", "program.halted", "invariant.violated", "account.frozen")


class Gateway:
    def __init__(self, db: Database, venue: Venue, bank):
        self.db = db
        self.venue = venue
        self.bank = bank
        self.listeners: list[Callable[[dict], None]] = []     # Photon, sandbox hooks, tests
        self._halt_lock = threading.Lock()
        self._offline_alarmed: dict[int, int] = {}

    # ------------------------------------------------------------------ events
    def emit(self, account: int | None, program_id: int | None, type_: str, payload: dict | None = None) -> dict:
        payload = dict(payload or {})
        with self.db.connect() as c:
            eid = self.db.insert_event(c, account, program_id, type_, payload)
            row = c.execute("SELECT payload_json FROM events WHERE id=?", (eid,)).fetchone()
        ev = json.loads(row["payload_json"])
        ev["id"] = eid
        for fn in list(self.listeners):
            try:
                fn(ev)
            except Exception:
                log.exception("event listener failed")
        return ev

    # ------------------------------------------------------------------ accounts (§10.1)
    def create_account(self, opening_balance_cents: int = 100_000_00, owner_phone: str | None = None) -> dict:
        t = now_ms()
        with self.db.tx() as c:
            cur = c.execute("INSERT INTO accounts(bank, frozen, owner_phone, created_t) VALUES (?,?,?,?)",
                            (self.bank.name, 0, owner_phone, t))
            account = int(cur.lastrowid)
            owner_key, _ = keys.issue(c, "ok", account)
        try:
            cust = self.bank.create_customer(account)
            acct = self.bank.create_account(cust, f"gateway-account-{account}", opening_balance_cents)
        except BankError as e:
            raise ApiError(502, "bank_error", detail=str(e))
        with self.db.connect() as c:
            c.execute("UPDATE accounts SET bank_customer_id=?, bank_account_id=? WHERE id=?", (cust, acct, account))
        self.emit(account, None, "account.created", {"bank": self.bank.name, "bank_account_id": acct,
                                                     "opening_balance_cents": opening_balance_cents})
        return {"account": account, "owner_key": owner_key, "bank": self.bank.name,
                "bank_customer_id": cust, "bank_account_id": acct,
                "balance_cents": self.bank.balance(acct),
                "note": "Nessie is a simulated bank: the flow is real, the money is not." if self.bank.name == "nessie"
                else "local bank simulator in use (set NESSIE_API_KEY for Nessie)"}

    def _account(self, c, account: int) -> dict:
        row = c.execute("SELECT * FROM accounts WHERE id=?", (account,)).fetchone()
        if row is None:
            raise ApiError(404, "unknown_account")
        d = row_to_dict(row)
        d["envelope"] = jloads(d.pop("envelope_json"))
        return d

    def set_envelope(self, account: int, envelope: dict) -> dict:
        for k in ENVELOPE_REQUIRED:
            if k not in envelope:
                raise ApiError(400, "bad_envelope", missing=k)
        env = envelope["env"]
        for k in ("max_gap_ticks", "fee_bps"):
            if k not in env:
                raise ApiError(400, "bad_envelope", missing=f"env.{k}")
        for k in ("max_loss_cents", "max_fees_cents", "max_trades", "max_exposure_lots"):
            if int(envelope[k]) < 0:
                raise ApiError(400, "bad_envelope", field=k)
        known = self.venue.instruments()
        for ins in envelope["instruments"]:
            if ins not in known:
                raise ApiError(400, "bad_envelope", unknown_instrument=ins)
        stored = {"contract_version": 1, "account": account, "window": "24h_rolling",
                  "max_loss_cents": int(envelope["max_loss_cents"]), "max_fees_cents": int(envelope["max_fees_cents"]),
                  "max_trades": int(envelope["max_trades"]), "max_exposure_lots": int(envelope["max_exposure_lots"]),
                  "instruments": list(envelope["instruments"]),
                  "require_live_stop": bool(envelope.get("require_live_stop", True)),
                  "env": {"max_gap_ticks": int(env["max_gap_ticks"]), "fill": env.get("fill", "full_at_print"),
                          "fee_bps": {"maker": float(env["fee_bps"]["maker"]), "taker": float(env["fee_bps"]["taker"])},
                          "trigger_ref": env.get("trigger_ref", "last")}}
        with self.db.tx() as c:
            self._account(c, account)
            c.execute("UPDATE accounts SET envelope_json=? WHERE id=?", (json.dumps(stored), account))
        self.emit(account, None, "envelope.set", {"envelope": stored})
        return stored

    def issue_author_key(self, account: int) -> dict:
        with self.db.tx() as c:
            self._account(c, account)
            k, kid = keys.issue(c, "ak", account)
        self.emit(account, None, "author_key.issued", {"key_id": kid})
        return {"author_key": k, "key_id": kid}

    def freeze(self, account: int) -> dict:
        with self.db.tx() as c:
            self._account(c, account)
            c.execute("UPDATE accounts SET frozen=1 WHERE id=?", (account,))
            live = [row_to_dict(r) for r in c.execute(
                "SELECT * FROM programs WHERE account=? AND status='deployed'", (account,))]
        paused = []
        for p in live:
            try:
                self.venue.pause(p["id"])
                with self.db.tx() as c:
                    c.execute("UPDATE programs SET status='paused' WHERE id=? AND status='deployed'", (p["id"],))
                    keys.set_state(c, p["bk_key_id"], "paused")
                paused.append(p["id"])
                self.emit(account, p["id"], "program.paused", {"by": "freeze"})
            except VenueError as e:
                log.error("freeze: pause of %s failed: %s", p["id"], e)
        self.emit(account, None, "account.frozen", {"paused": paused})
        return {"frozen": True, "paused_programs": paused}

    def account_view(self, account: int) -> dict:
        with self.db.connect() as c:
            a = self._account(c, account)
            live = [self._program_summary(row_to_dict(r)) for r in c.execute(
                "SELECT * FROM programs WHERE account=? AND status IN ('certified','deployed','paused') ORDER BY id",
                (account,))]
            alarms = [json.loads(r["payload_json"]) | {"id": r["id"]} for r in c.execute(
                "SELECT id, payload_json FROM events WHERE account=? AND type IN (%s) ORDER BY id DESC LIMIT 20"
                % ",".join("?" * len(DERIVED_EVENTS)), (account, *DERIVED_EVENTS))]
            rem = budget_mod.remaining(c, account, a["envelope"]).as_dict() if a["envelope"] else None
        balance = None
        try:
            balance = self.bank.balance(a["bank_account_id"]) if a["bank_account_id"] else None
        except BankError as e:
            log.warning("bank balance unavailable: %s", e)
        return {"account": account, "bank": a["bank"], "bank_account_id": a["bank_account_id"],
                "balance_cents": balance, "envelope": a["envelope"], "budget": rem, "frozen": bool(a["frozen"]),
                "live_programs": live, "alarms": alarms, "venue": self.venue.name}

    # ------------------------------------------------------------------ programs (§10.2)
    def submit_program(self, key: keys.KeyInfo, account: int, backend: str, source: str,
                       parent_program_id: int | None = None) -> tuple[int, dict]:
        """The one new method. Returns (http status, body) for 200 / 400 / 409 / 422."""
        keys.require_author(key, account)
        if backend != "minsky":
            raise ApiError(400, "unknown_backend", backend=backend)
        if not isinstance(source, str) or not source.strip():
            raise ApiError(400, "empty_source")
        with self.db.connect() as c:
            a = self._account(c, account)
        if a["frozen"]:
            raise ApiError(403, "account_frozen")
        envelope = a["envelope"]
        if not envelope:
            raise ApiError(409, "envelope_missing")
        t = now_ms()

        # 3. policyc
        code, compiled = self.venue.compile(source, envelope["env"])
        if code == 2:
            pid = self._store_program(account, key.id, backend, source, compiled.get("source_hash"), None,
                                      compiled, "static_error", parent_program_id)
            body = {**compiled, "error": "static_error", "program_id": pid}
            self.emit(account, pid, "program.static_error", {"author_key_id": key.id, "line": compiled.get("line"),
                                                              "message": compiled.get("message")})
            return 400, body
        if code != 0:
            raise ApiError(500, "compiler_error", detail=compiled)

        # instruments must be inside the envelope
        outside = [i for i in compiled["instruments"] if i not in envelope["instruments"]]
        if outside:
            rej = {"contract_version": 1, "status": "rejected", "violates": "instruments", "limit": envelope["instruments"],
                   "value_on_path": outside[0], "counterexample": []}
            pid = self._store_program(account, key.id, backend, source, compiled["source_hash"], compiled, rej,
                                      "rejected", parent_program_id, violates="instruments")
            rej["program_id"] = pid
            self._on_rejected(account, pid, key.id, "instruments")
            return 422, rej

        # 4. verify at the current mark. A down venue is a 503, not an unhandled 500:
        # the live demo dies if this request resets the connection.
        try:
            mark = self.venue.mark(compiled["instruments"][0])
        except VenueError as e:
            raise ApiError(503, "venue_error", error=e.error, detail=e.detail)
        code, result = self.venue.verify(compiled, envelope, mark, config.VERIFY_TIMEOUT_MS)
        if code == 4:
            raise ApiError(503, "verify_timeout", timeout_ms=config.VERIFY_TIMEOUT_MS)
        if code == 3:
            pid = self._store_program(account, key.id, backend, source, compiled["source_hash"], compiled, result,
                                      "rejected", parent_program_id, violates=result.get("violates"))
            result = {**result, "program_id": pid, "account": account, "mark": mark}
            self._on_rejected(account, pid, key.id, result.get("violates"))
            return 422, result
        if code != 0:
            raise ApiError(500, "verifier_error", detail=result)
        bound = result["bound"]

        # 5-7 in one transaction: instrument busy, budget, reserve, mint key, store, emit.
        try:
            with self.db.tx() as c:
                a = self._account(c, account)
                if a["frozen"]:
                    raise ApiError(403, "account_frozen")
                busy = c.execute(
                    "SELECT p.id, p.instruments_json FROM programs p WHERE p.account=? AND p.status IN ('certified','deployed','paused')",
                    (account,)).fetchall()
                for b in busy:
                    clash = set(json.loads(b["instruments_json"])) & set(compiled["instruments"])
                    if clash:
                        raise ApiError(409, "instrument_busy", instrument=sorted(clash)[0], live_program_id=int(b["id"]),
                                       certificate=result)
                pid = self._insert_program(c, account, key.id, backend, source, compiled["source_hash"], compiled,
                                           result, "certified", parent_program_id, bound=bound, t=t)
                try:
                    before, after = budget_mod.check_and_reserve(c, account, pid, bound, envelope, t)
                except ApiError as e:
                    e.extra.update(certificate=result)
                    raise
                bk, bk_id = keys.issue(c, "bk", account, pid)
                c.execute("UPDATE programs SET bk_key_id=? WHERE id=?", (bk_id, pid))
                cert = {**result, "program_id": pid, "account": account, "mark": mark,
                        "budget": {"remaining_before_cents": before.loss, "remaining_after_cents": after.loss,
                                   "remaining_fees_after_cents": after.fees, "remaining_trades_after": after.trades},
                        "key": bk}
                c.execute("UPDATE programs SET result_json=? WHERE id=?", (json.dumps({k: v for k, v in cert.items() if k != "key"}), pid))
        except ApiError as e:
            if e.code in ("budget", "instrument_busy"):
                # the transaction rolled back; keep a record of the refused program (no key, no reservation)
                e.extra["program_id"] = self._store_program(account, key.id, backend, source, compiled["source_hash"],
                                                            compiled, result, "budget_refused" if e.code == "budget" else "instrument_busy",
                                                            parent_program_id)
            if e.code == "budget":
                self.emit(account, e.extra.get("program_id"), "program.budget_refused",
                          {"author_key_id": key.id, "remaining_cents": e.extra["remaining_cents"],
                           "requested_cents": e.extra["requested_cents"], "violates": e.extra.get("violates")})
                self._maybe_budget_low(account)
            elif e.code == "instrument_busy":
                self.emit(account, e.extra.get("program_id"), "program.instrument_busy",
                          {"author_key_id": key.id, "instrument": e.extra.get("instrument")})
            raise
        self.emit(account, pid, "program.certified", {"author_key_id": key.id, "bound": bound,
                                                      "remaining_after_cents": after.loss, "bk_key_id": bk_id})
        self._maybe_budget_low(account, before=before, after=after)
        return 200, cert

    def _store_program(self, account, author_key_id, backend, source, source_hash, compiled, result, status,
                       parent_program_id, violates=None) -> int:
        with self.db.tx() as c:
            return self._insert_program(c, account, author_key_id, backend, source, source_hash, compiled, result,
                                        status, parent_program_id, violates=violates)

    def _insert_program(self, c, account, author_key_id, backend, source, source_hash, compiled, result, status,
                        parent_program_id, violates=None, bound=None, t=None) -> int:
        t = t or now_ms()
        instruments = compiled["instruments"] if compiled else []
        cur = c.execute(
            "INSERT INTO programs(account, author_key_id, backend, instruments_json, source, source_hash, compiled_json, "
            "result_json, status, violates, bound_loss, bound_fees, bound_trades, bound_exposure, locked_collateral, "
            "parent_program_id, created_t) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            (account, author_key_id, backend, json.dumps(instruments), source, source_hash or "",
             json.dumps(compiled) if compiled else None, json.dumps(result) if result else None, status, violates,
             bound and bound["max_loss_cents"], bound and bound["max_fees_cents"], bound and bound["halts_within_trades"],
             bound and bound["max_exposure_lots"], compiled and compiled["report"]["locked_collateral_cents"],
             parent_program_id, t))
        return int(cur.lastrowid)

    def _on_rejected(self, account: int, pid: int, author_key_id: int, violates: str | None):
        self.emit(account, pid, "program.rejected", {"author_key_id": author_key_id, "violates": violates})
        # rejections.burst: >= 3 rejections from one author key within 60 s
        since = now_ms() - config.REJECTIONS_BURST_WINDOW_MS
        with self.db.connect() as c:
            n = c.execute("SELECT COUNT(*) FROM events WHERE type='program.rejected' AND t>=? AND "
                          "json_extract(payload_json,'$.author_key_id')=?", (since, author_key_id)).fetchone()[0]
            already = c.execute("SELECT 1 FROM events WHERE type='rejections.burst' AND t>=? AND "
                                "json_extract(payload_json,'$.author_key_id')=?", (since, author_key_id)).fetchone()
        if n >= config.REJECTIONS_BURST_N and not already:
            self.emit(account, pid, "rejections.burst", {"author_key_id": author_key_id, "count": int(n),
                                                         "window_ms": config.REJECTIONS_BURST_WINDOW_MS})

    def _maybe_budget_low(self, account: int, before=None, after=None):
        with self.db.connect() as c:
            a = self._account(c, account)
            if not a["envelope"]:
                return
            after = after or budget_mod.remaining(c, account, a["envelope"])
        threshold = int(a["envelope"]["max_loss_cents"] * config.BUDGET_LOW_FRACTION)
        crossed_now = before is None or before.loss >= threshold
        if after.loss < threshold and crossed_now:
            with self.db.connect() as c:
                recent = c.execute("SELECT 1 FROM events WHERE account=? AND type='budget.low' AND t>=?",
                                   (account, now_ms() - 60_000)).fetchone()
            if not recent or before is not None:
                self.emit(account, None, "budget.low", {"remaining_cents": after.loss,
                                                        "max_loss_cents": a["envelope"]["max_loss_cents"]})

    def _program(self, c, pid: int) -> dict:
        row = c.execute("SELECT * FROM programs WHERE id=?", (pid,)).fetchone()
        if row is None:
            raise ApiError(404, "unknown_program")
        p = row_to_dict(row)
        p["compiled"] = jloads(p.pop("compiled_json"))
        p["result"] = jloads(p.pop("result_json"))
        p["instruments"] = jloads(p.pop("instruments_json"), [])
        p["order_ids"] = jloads(p.pop("order_ids_json"))
        p["incidents"] = jloads(p.pop("incidents_json"), [])
        return p

    def _program_summary(self, p: dict) -> dict:
        return {"program_id": p["id"], "status": p["status"], "instruments": jloads(p.get("instruments_json"), p.get("instruments")),
                "bound": {"max_loss_cents": p["bound_loss"], "max_fees_cents": p["bound_fees"],
                          "halts_within_trades": p["bound_trades"], "max_exposure_lots": p["bound_exposure"]},
                "created_t": p["created_t"], "deployed_t": p["deployed_t"]}

    def _authorize_read(self, key: keys.KeyInfo, p: dict) -> None:
        if key.cls == "ok" and key.account == p["account"]:
            return
        if key.cls == "ak" and key.account == p["account"]:
            return
        if key.cls == "bk":
            if key.program_id != p["id"]:
                raise ApiError(403, "key_bound_to_program", program_id=key.program_id)
            return
        raise ApiError(403, "key_bound_to_account", account=key.account)

    def get_program(self, key: keys.KeyInfo, pid: int) -> dict:
        with self.db.connect() as c:
            p = self._program(c, pid)
        self._authorize_read(key, p)
        return {"program_id": pid, "account": p["account"], "status": p["status"], "source": p["source"],
                "source_hash": p["source_hash"], "instruments": p["instruments"], "result": p["result"],
                "compiled": p["compiled"], "incidents": p["incidents"], "parent_program_id": p["parent_program_id"],
                "created_t": p["created_t"], "deployed_t": p["deployed_t"], "ended_t": p["ended_t"],
                "realized": {"loss_cents": p["realized_loss"], "fees_cents": p["realized_fees"], "trades": p["realized_trades"]}}

    def get_counterexample(self, key: keys.KeyInfo, pid: int) -> dict:
        with self.db.connect() as c:
            p = self._program(c, pid)
        self._authorize_read(key, p)
        if not p["result"] or p["result"].get("status") != "rejected":
            raise ApiError(404, "no_counterexample", program_status=p["status"])
        return {"program_id": pid, "violates": p["result"].get("violates"), "limit": p["result"].get("limit"),
                "value_on_path": p["result"].get("value_on_path"), "path": p["result"].get("counterexample", []),
                "replay": {"cli": "build/replay --program compiled.json --path path.json --exchange " + config.EXCHANGE_URL},
                "compiled": p["compiled"]}

    # ------------------------------------------------------------------ program key (§10.3)
    def deploy(self, key: keys.KeyInfo, pid: int) -> dict:
        keys.require_program_key(key, pid, ("issued",))
        with self.db.connect() as c:
            p = self._program(c, pid)
            a = self._account(c, p["account"])
        if a["frozen"]:
            raise ApiError(403, "account_frozen")
        if p["status"] != "certified":
            raise ApiError(409, "not_certified", program_status=p["status"])
        fund = int(p["locked_collateral"] or 0) + int(p["bound_loss"] or 0)
        try:
            escrow = self.bank.create_account(a["bank_customer_id"], f"escrow-program-{pid}")
            self.bank.transfer(a["bank_account_id"], escrow, fund, f"escrow for program {pid}")
        except BankError as e:
            raise ApiError(402, "insufficient_funds" if e.code == "insufficient_funds" else "bank_error", detail=str(e))
        try:
            resp = self.venue.deploy(pid, p["account"], p["compiled"], fund)
        except VenueError as e:
            try:
                self.bank.transfer(escrow, a["bank_account_id"], fund, f"refund: deploy of {pid} failed")
            except BankError:
                log.exception("refund failed after venue error")
            raise ApiError(503, "venue_error", error=e.error, detail=e.detail)
        if hasattr(self.venue, "set_path"):
            self.venue.set_path(pid, (p["result"] or {}).get("worst_path", []))
        with self.db.tx() as c:
            c.execute("UPDATE programs SET status='deployed', sub_account=?, order_ids_json=?, fund_cents=?, "
                      "escrow_account_id=?, deployed_t=? WHERE id=?",
                      (resp.get("sub_account"), json.dumps(resp.get("order_ids")), fund, escrow, now_ms(), pid))
            keys.set_state(c, key.id, "deployed")
        self.emit(p["account"], pid, "program.deployed", {"sub_account": resp.get("sub_account"),
                                                          "order_ids": resp.get("order_ids"), "fund_cents": fund})
        return {"program_id": pid, "order_ids": resp.get("order_ids"), "sub_account": resp.get("sub_account"),
                "locked_collateral_cents": resp.get("locked_collateral_cents", p["locked_collateral"]),
                "fund_cents": fund, "escrow_account_id": escrow}

    def pause(self, key: keys.KeyInfo, pid: int) -> dict:
        keys.require_program_key(key, pid, ("deployed",))
        try:
            resp = self.venue.pause(pid)
        except VenueError as e:
            if e.error == "not_live":
                raise ApiError(403, "key_spent", state="ended")
            raise ApiError(503, "venue_error", error=e.error, detail=e.detail)
        with self.db.tx() as c:
            p = self._program(c, pid)
            c.execute("UPDATE programs SET status='paused' WHERE id=? AND status='deployed'", (pid,))
            keys.set_state(c, key.id, "paused")
        self.emit(p["account"], pid, "program.paused", {"cancelled": resp.get("cancelled", [])})
        return {"program_id": pid, "cancelled": resp.get("cancelled", []), "status": "paused"}

    def cancel(self, key: keys.KeyInfo, pid: int) -> dict:
        keys.require_program_key(key, pid, ("issued", "deployed", "paused"))
        with self.db.connect() as c:
            p = self._program(c, pid)
        if key.state == "issued":
            # never deployed: release the reservation, nothing to flatten
            self._finalize(pid, realized_loss=0, fees=0, trades=0, final_status="cancelled")
            return {"program_id": pid, "cancelled": [], "flatten_fill": None, "status": "cancelled"}
        try:
            resp = self.venue.cancel(pid)
        except VenueError as e:
            if e.error == "not_live":
                raise ApiError(403, "key_spent", state="ended")
            raise ApiError(503, "venue_error", error=e.error, detail=e.detail)
        with self.db.tx() as c:
            keys.set_state(c, key.id, "spent")
            c.execute("UPDATE programs SET cancel_requested_t=? WHERE id=?", (now_ms(), pid))
        self.emit(p["account"], pid, "program.cancel_requested", {"cancelled": resp.get("cancelled", []),
                                                                  "flatten_pending": resp.get("flatten_pending", False)})
        # CONTRACT §9 A3: the venue flattens natively on the next print. If it has already ended, settle
        # now; otherwise the ledger waits for program.halted (or reconcile), through the same path.
        st = None
        try:
            st = self.venue.state(pid)
        except VenueError:
            pass
        ended = st is not None and st.get("status") in ("halted", "cancelled") and not st.get("position")
        if ended:
            self._finalize(pid, st.get("realized_loss_cents", 0), st.get("fees_cents", 0), st.get("trades", 0), "cancelled")
        return {"program_id": pid, "cancelled": resp.get("cancelled", []), "flatten_fill": resp.get("flatten_fill"),
                "flatten_pending": resp.get("flatten_pending", False), "status": "cancelled" if ended else "cancelling"}

    def state(self, key: keys.KeyInfo, pid: int) -> dict:
        with self.db.connect() as c:
            p = self._program(c, pid)
        if key.cls == "bk":
            keys.require_program_key(key, pid, ("issued", "deployed", "paused", "spent"))
        else:
            self._authorize_read(key, p)
        bound = {"max_loss_cents": p["bound_loss"], "max_fees_cents": p["bound_fees"],
                 "halts_within_trades": p["bound_trades"], "max_exposure_lots": p["bound_exposure"]}
        venue_state = None
        if p["status"] in ("deployed", "paused", "halted", "cancelled") and p["deployed_t"]:
            try:
                venue_state = self.venue.state(pid)
            except VenueError as e:
                venue_state = {"ok": False, "error": e.error}
        return {"program_id": pid, "status": p["status"], "bound": bound, "venue": venue_state,
                "loss_cents": (venue_state or {}).get("mtm_loss_cents", p["realized_loss"]),
                "fees_cents": (venue_state or {}).get("fees_cents", p["realized_fees"]),
                "trades": (venue_state or {}).get("trades", p["realized_trades"]),
                "position": (venue_state or {}).get("position", 0),
                "live_nodes": (venue_state or {}).get("live_nodes", []),
                "realized": {"loss_cents": p["realized_loss"], "fees_cents": p["realized_fees"], "trades": p["realized_trades"]}}

    # ------------------------------------------------------------------ halts, settlement, reconcile
    def on_venue_event(self, ev: dict) -> None:
        """B6 hands every venued event here. Stored, then applied."""
        typ = ev.get("type")
        pid = ev.get("program_id")
        account = None
        if pid is not None:
            with self.db.connect() as c:
                row = c.execute("SELECT account FROM programs WHERE id=?", (pid,)).fetchone()
            account = int(row["account"]) if row else None
        if typ == "program.halted":
            if account is None:
                log.warning("program.halted for a program this gateway does not know: %s", pid)
                return
            self._finalize(pid, ev.get("realized_loss_cents", 0), ev.get("fees_cents", 0), ev.get("trades", 0),
                           ev.get("status") or None, event=ev)
            return
        if typ == "print":
            return      # not persisted: too chatty, and the mark is read from the venue when needed
        self.emit(account, pid, typ, {k: v for k, v in ev.items() if k not in ("type", "contract_version")})

    def _finalize(self, pid: int, realized_loss: int, fees: int, trades: int, final_status: str | None,
                  event: dict | None = None) -> None:
        """One transaction: release the reservation, record the realized loss, spend the key.
        Then settle with the bank. Idempotent: a second halt for the same program is a no-op."""
        with self._halt_lock:
            with self.db.tx() as c:
                p = self._program(c, pid)
                if p["status"] in ("halted", "cancelled"):
                    if p["settled_t"]:
                        return
                else:
                    a = self._account(c, p["account"])
                    envelope = a["envelope"]
                    status = final_status or ("cancelled" if (p["status"] == "cancelled" or p.get("cancel_requested_t")) else "halted")
                    if p["status"] == "certified" and status == "halted":
                        status = "cancelled"
                    budget_mod.release_and_realize(c, p["account"], pid, envelope, realized_loss, fees, trades)
                    c.execute("UPDATE programs SET status=?, realized_loss=?, realized_fees=?, realized_trades=?, ended_t=? WHERE id=?",
                              (status, max(0, int(realized_loss)), int(fees), int(trades), now_ms(), pid))
                    if p["bk_key_id"]:
                        keys.set_state(c, p["bk_key_id"], "spent")
                    p["status"] = status
                    p["realized_loss"], p["realized_fees"], p["realized_trades"] = max(0, int(realized_loss)), int(fees), int(trades)
            # settle: escrow -> account minus realized loss (which includes fees, CONTRACT §6.3); the rest -> venue clearing.
            # A program that ended in profit is paid by the venue: the halt event floors the loss at 0 (the budget
            # only counts losses), so the signed result comes from the venue's state.
            payload_profit = 0
            if p.get("escrow_account_id") and not p.get("settled_t"):
                with self.db.connect() as c:
                    a_row = self._account(c, p["account"])
                keep = min(int(p["realized_loss"]), int(p["fund_cents"] or 0))
                back = max(0, int(p["fund_cents"] or 0) - keep)
                signed = None
                try:
                    signed = int(self.venue.state(pid).get("realized_loss_cents"))
                except Exception:
                    pass
                profit = -signed if signed is not None and signed < 0 else 0
                payload_profit = profit
                if not self._settle_bank(pid, p["escrow_account_id"], a_row["bank_account_id"], back, keep, profit):
                    # the escrow's funds have not landed yet (Nessie completes transfers on a sweep): keep trying off-thread
                    threading.Thread(target=self._settle_bank_later, name=f"settle-{pid}",
                                     args=(pid, p["escrow_account_id"], a_row["bank_account_id"], back, keep, profit), daemon=True).start()
                else:
                    with self.db.tx() as c:
                        c.execute("UPDATE programs SET settled_t=? WHERE id=? AND settled_t IS NULL", (now_ms(), pid))
            else:
                with self.db.tx() as c:
                    c.execute("UPDATE programs SET settled_t=? WHERE id=? AND settled_t IS NULL", (now_ms(), pid))
        payload = {"realized_loss_cents": max(0, int(realized_loss)), "fees_cents": int(fees), "trades": int(trades),
                   "status": p["status"], "bound_max_loss_cents": p["bound_loss"], "profit_cents": locals().get("payload_profit", 0)}
        if event:
            payload["venue_t"] = event.get("t")
        self.emit(p["account"], pid, "program.halted", payload)
        self._maybe_budget_low(p["account"])

    def _settle_bank(self, pid: int, escrow: str, account: str, back: int, keep: int, profit: int) -> bool:
        """One attempt at the bank side of a settlement. False when the escrow cannot pay yet."""
        try:
            self.bank.transfer(escrow, account, back, f"settle program {pid}")
            self.bank.transfer(escrow, self.bank.clearing_account(), keep, f"realized loss+fees program {pid}")
        except BankError as e:
            if e.code == "insufficient_funds":
                return False
            log.error("settlement failed for %s: %s", pid, e)
            return True                                   # not retryable; the event still carries the numbers
        if profit > 0:
            try:
                self.bank.transfer(self.bank.clearing_account(), account, profit, f"profit program {pid}")
            except BankError:
                try:
                    self.bank.deposit(account, profit, f"profit program {pid} (venue clearing)")
                except Exception as e:                       # a bank without deposits: log, the number is still in the event
                    log.error("profit payout failed for %s: %s", pid, e)
        return True

    SETTLE_MAX_WAIT_S = 1800        # Nessie's sweep is slow under load: keep trying for up to 30 minutes
    SETTLE_FIRST_DELAY_S = 5.0      # 5 s, doubling ...
    SETTLE_MAX_DELAY_S = 60.0       # ... to 60 s between attempts
    SETTLE_WARN_EVERY_S = 300       # a warning line every 5 minutes; error only at the final give-up

    def _settle_bank_later(self, pid: int, escrow: str, account: str, back: int, keep: int, profit: int,
                           max_wait_s: float | None = None, first_delay_s: float | None = None,
                           max_delay_s: float | None = None, warn_every_s: float | None = None,
                           sleep: Callable[[float], None] = time.sleep, clock: Callable[[], float] = time.monotonic) -> bool:
        """Retry the bank side of a settlement whose escrow has not been credited yet (Nessie completes
        transfers on a sweep). Backoff 5 s doubling to 60 s, for up to 30 minutes; a warning every 5
        minutes; a success at any point marks the program settled and emits `program.settled` (late);
        an error line only when giving up. The amounts are the ones `_finalize` computed: nothing here
        touches the ledger arithmetic. Returns True when the settlement went through."""
        max_wait_s = self.SETTLE_MAX_WAIT_S if max_wait_s is None else max_wait_s
        delay = self.SETTLE_FIRST_DELAY_S if first_delay_s is None else first_delay_s
        max_delay_s = self.SETTLE_MAX_DELAY_S if max_delay_s is None else max_delay_s
        warn_every_s = self.SETTLE_WARN_EVERY_S if warn_every_s is None else warn_every_s
        t0 = clock()
        deadline = t0 + max_wait_s
        next_warn = t0 + warn_every_s
        attempts = 0
        while True:
            now = clock()
            if now >= deadline:
                break
            sleep(min(delay, max(0.0, deadline - now)))
            attempts += 1
            if self._settle_bank(pid, escrow, account, back, keep, profit):
                with self.db.tx() as c:
                    c.execute("UPDATE programs SET settled_t=? WHERE id=? AND settled_t IS NULL", (now_ms(), pid))
                waited = round(clock() - t0, 1)
                self.emit(None, pid, "program.settled", {"escrow": escrow, "back_cents": back, "kept_cents": keep,
                                                         "profit_cents": profit, "late": True, "waited_s": waited, "attempts": attempts})
                log.info("settlement of program %s went through after %s s (%d attempt(s))", pid, waited, attempts)
                return True
            now = clock()
            if now >= next_warn and now < deadline:
                log.warning("settlement of program %s still blocked after %d s (%d attempt(s)); escrow %s not yet credited, retrying",
                            pid, int(now - t0), attempts, escrow)
                next_warn += warn_every_s
            delay = min(delay * 2, max_delay_s)
        log.error("settlement of program %s still blocked after %d s (%d attempt(s)); escrow %s keeps the funds",
                  pid, int(max_wait_s), attempts, escrow)
        return False

    def reconcile(self) -> dict:
        """After a restart: ask venued for every program's state and apply any halt we missed."""
        applied = []
        try:
            states = self.venue.reconcile()
        except VenueError as e:
            log.error("reconcile failed: %s", e)
            return {"applied": [], "error": e.error}
        for st in states:
            pid = st.get("program_id")
            if pid is None:
                continue
            with self.db.connect() as c:
                row = c.execute("SELECT status, account, deployed_t FROM programs WHERE id=?", (pid,)).fetchone()
            if row is None:
                log.warning("reconcile: venued knows program %s, this gateway does not (stale venue state?)", pid)
                continue
            if not row["deployed_t"] or (st.get("account") is not None and int(st["account"]) != int(row["account"])):
                # Program ids are assigned by this gateway; a program it never deployed cannot have
                # halted at the venue. A venue state file from another database is the usual cause.
                log.error("reconcile: venued reports program %s (account %s) but this gateway never deployed it "
                          "(status %s, account %s); ignoring. Clear the venue's state file.",
                          pid, st.get("account"), row["status"], row["account"])
                continue
            if st.get("status") in ("halted", "cancelled") and row["status"] in ("deployed", "paused"):
                self._finalize(pid, st.get("realized_loss_cents", 0), st.get("fees_cents", 0), st.get("trades", 0), st["status"])
                applied.append(pid)
            elif st.get("status") == "paused" and row["status"] == "deployed":
                with self.db.tx() as c:
                    c.execute("UPDATE programs SET status='paused' WHERE id=?", (pid,))
        return {"applied": applied, "programs": len(states)}

    def check_agent_offline(self, now: int | None = None) -> list[int]:
        """agent.offline: a live program and no author-key request for T seconds."""
        now = now or now_ms()
        fired = []
        with self.db.connect() as c:
            rows = c.execute("SELECT DISTINCT account FROM programs WHERE status IN ('deployed','paused')").fetchall()
            for r in rows:
                account = int(r["account"])
                last = c.execute("SELECT MAX(COALESCE(last_used_t, created_t)) AS t FROM keys WHERE account=? AND class='ak'",
                                 (account,)).fetchone()["t"] or 0
                if now - last > config.AGENT_OFFLINE_T_MS and self._offline_alarmed.get(account) != last:
                    self._offline_alarmed[account] = last
                    fired.append(account)
        for account in fired:
            self.emit(account, None, "agent.offline", {"silent_ms": config.AGENT_OFFLINE_T_MS})
        return fired
