"""B5: banking. Capital One's Nessie (v2 API) when NESSIE_API_KEY is set and accepted; otherwise a local simulator.

Be honest: Nessie is a simulated bank. The flow is real (customer, account, deposit, escrow
per program, settlement at halt); the money is not. The local simulator keeps the same ledger
in SQLite so the demo runs offline and the response says which one is in use (`bank` field).
"""
from __future__ import annotations

import logging
import threading
import time
import uuid
import httpx

from . import config
from .db import Database, now_ms

log = logging.getLogger("gateway.bank")

VENUE_CLEARING = "venue-clearing"      # where realized losses and fees go at settlement


class LocalBank:
    name = "local-sim"

    def __init__(self, db: Database):
        self.db = db
        with db.connect() as c:
            c.execute("INSERT OR IGNORE INTO bank_accounts(id, owner, nickname, balance_cents) VALUES (?,?,?,?)",
                      (VENUE_CLEARING, "venue", "clearing", 0))

    def create_customer(self, account_id: int) -> str:
        return f"cust-{account_id}"

    def create_account(self, customer_id: str, nickname: str, opening_cents: int = 0) -> str:
        acct_id = f"acct-{uuid.uuid4().hex[:12]}"
        with self.db.connect() as c:
            c.execute("INSERT INTO bank_accounts(id, owner, nickname, balance_cents) VALUES (?,?,?,?)",
                      (acct_id, customer_id, nickname, 0))
        if opening_cents:
            self.deposit(acct_id, opening_cents, "opening balance")
        return acct_id

    def balance(self, acct_id: str) -> int:
        with self.db.connect() as c:
            row = c.execute("SELECT balance_cents FROM bank_accounts WHERE id=?", (acct_id,)).fetchone()
        return int(row["balance_cents"]) if row else 0

    def deposit(self, acct_id: str, cents: int, description: str = "") -> None:
        with self.db.tx() as c:
            c.execute("UPDATE bank_accounts SET balance_cents=balance_cents+? WHERE id=?", (cents, acct_id))
            c.execute("INSERT INTO bank_tx(from_id, to_id, cents, description, t) VALUES (?,?,?,?,?)",
                      (None, acct_id, cents, description, now_ms()))

    def transfer(self, from_id: str, to_id: str, cents: int, description: str = "") -> None:
        if cents <= 0:
            return
        with self.db.tx() as c:
            row = c.execute("SELECT balance_cents FROM bank_accounts WHERE id=?", (from_id,)).fetchone()
            if row is None or int(row["balance_cents"]) < cents:
                raise BankError("insufficient_funds", f"{from_id} has {row['balance_cents'] if row else 0}, needs {cents}")
            c.execute("UPDATE bank_accounts SET balance_cents=balance_cents-? WHERE id=?", (cents, from_id))
            c.execute("UPDATE bank_accounts SET balance_cents=balance_cents+? WHERE id=?", (cents, to_id))
            c.execute("INSERT INTO bank_tx(from_id, to_id, cents, description, t) VALUES (?,?,?,?,?)",
                      (from_id, to_id, cents, description, now_ms()))

    def clearing_account(self) -> str:
        return VENUE_CLEARING


class NessieBank:
    """Capital One Nessie v2 (https://v2-prod-api.nessieisreal.com): Bearer auth, amounts in cents,
    customers -> accounts -> deposits and transfers between accounts of the same key. Still a simulated
    bank: the flow is real, the money is not."""
    name = "nessie"

    def __init__(self, db: Database, api_key: str | None = None, base: str | None = None):
        self.db = db
        self.key = api_key or config.NESSIE_API_KEY
        self.base = (base or config.NESSIE_BASE).rstrip("/")
        self.http = httpx.Client(timeout=20, headers={"Authorization": f"Bearer {self.key}"}, base_url=self.base)
        self._clearing: str | None = None

    # Nessie v2 allows 120 requests a minute per key. One process-wide pacer spaces every call and backs
    # off on 429 (honouring retry-after), so a burst of lab programs slows down instead of failing.
    _pace_lock = threading.Lock()
    _next_slot = 0.0
    MIN_SPACING_S = 0.55

    def _req(self, method: str, path: str, body: dict | None = None) -> dict:
        for attempt in range(8):
            with NessieBank._pace_lock:
                wait = NessieBank._next_slot - time.monotonic()
                if wait > 0:
                    time.sleep(wait)
                NessieBank._next_slot = time.monotonic() + NessieBank.MIN_SPACING_S
            try:
                r = self.http.request(method, path, json=body)
            except httpx.HTTPError as e:
                raise BankError("nessie_unreachable", str(e)[:200])
            if r.status_code == 429:
                try:
                    delay = float(r.headers.get("retry-after") or 1.0)
                except ValueError:
                    delay = 1.0
                time.sleep(min(10.0, delay + 0.25 * attempt))
                continue
            try:
                if int(r.headers.get("ratelimit-remaining", "99")) <= 2:
                    time.sleep(1.0)
            except ValueError:
                pass
            if r.status_code >= 300:
                code = "insufficient_funds" if r.status_code in (402, 409, 422) and "insufficient" in r.text.lower() else "nessie_error"
                raise BankError(code, f"{r.status_code} {r.text[:200]}")
            return r.json() if r.content else {}
        raise BankError("nessie_error", "429 rate limited after 8 attempts")

    def create_customer(self, account_id: int) -> str:
        body = {"first_name": "Gateway", "last_name": f"Account{account_id}"[:80],
                "address": {"street_number": "1", "street_name": "Oxford St", "city": "Cambridge", "state": "MA", "zip": "02138"}}
        return self._req("POST", "/customers", body)["id"]

    def create_account(self, customer_id: str, nickname: str, opening_cents: int = 0) -> str:
        body = {"type": "Checking", "nickname": nickname[:40], "balance_cents": int(max(0, opening_cents))}
        return self._req("POST", f"/customers/{customer_id}/accounts", body)["id"]

    def balance(self, acct_id: str) -> int:
        """Settled balance plus what is pending in, minus what is pending out: Nessie v2 completes a
        transaction on a sweep (up to ~30 s after the fast-forward), and the gateway's ledger must not
        see money vanish or double for that window."""
        settled = int(self._req("GET", f"/accounts/{acct_id}").get("balance_cents", 0))
        pending_in = pending_out = 0
        try:
            for tx in self._req("GET", f"/accounts/{acct_id}/transactions").get("items", []):
                if tx.get("status") != "pending":
                    continue
                amt = int(tx.get("amount_cents") or 0)
                if tx.get("kind") in ("transfer",) and tx.get("payee_id") == acct_id:
                    pending_in += amt
                elif tx.get("kind") in ("deposit",) :
                    pending_in += amt
                elif tx.get("payer_id") == acct_id:
                    pending_out += amt
        except BankError:
            pass
        return settled + pending_in - pending_out

    def settled(self, acct_id: str) -> bool:
        """True when nothing is pending on the account (its escrow can be paid out)."""
        try:
            return not any(tx.get("status") == "pending" for tx in self._req("GET", f"/accounts/{acct_id}/transactions").get("items", []))
        except BankError:
            return True

    def deposit(self, acct_id: str, cents: int, description: str = "") -> None:
        if cents <= 0:
            return
        self._req("POST", f"/accounts/{acct_id}/deposits", {"amount_cents": int(cents), "medium": "balance", "description": description[:200]})

    def transfer(self, from_id: str, to_id: str, cents: int, description: str = "") -> None:
        if cents <= 0:
            return
        self._req("POST", f"/accounts/{from_id}/transfers", {"payee_id": to_id, "amount_cents": int(cents), "medium": "balance",
                                                             "description": description[:200]})

    def clearing_account(self) -> str:
        if self._clearing is None:
            with self.db.connect() as c:
                row = c.execute("SELECT v FROM meta WHERE k='nessie_clearing_v2'").fetchone()
            if row:
                self._clearing = row["v"]
            else:
                cust = self.create_customer(0)
                self._clearing = self.create_account(cust, "venue-clearing")
                with self.db.connect() as c:
                    c.execute("INSERT OR REPLACE INTO meta(k, v) VALUES ('nessie_clearing_v2', ?)", (self._clearing,))
        return self._clearing


class BankError(Exception):
    def __init__(self, code: str, detail: str = ""):
        super().__init__(f"{code}: {detail}")
        self.code, self.detail = code, detail


def make_bank(db: Database):
    """Nessie when the key is set AND accepted for a write (its reads answer 200 to any key); otherwise
    the local simulator, with the reason kept on the object for /lab/status and /healthz."""
    if config.NESSIE_API_KEY:
        bank = NessieBank(db)
        try:
            bank.probe_customer_id = bank.create_customer(0)
            return bank
        except (BankError, httpx.HTTPError) as e:
            log.error("Nessie key set but rejected on a write (%s); using the local bank simulator", e)
            local = LocalBank(db)
            local.fallback_reason = f"Nessie rejected the key on a write: {str(e)[:80]}"
            return local
    return LocalBank(db)
