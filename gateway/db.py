"""SQLite schema and connections. One connection per thread; writers use BEGIN IMMEDIATE."""
from __future__ import annotations

import json
import os
import sqlite3
import threading
import time
from contextlib import contextmanager
from pathlib import Path

from . import config

SCHEMA = """
CREATE TABLE IF NOT EXISTS accounts (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  bank TEXT NOT NULL,
  bank_customer_id TEXT,
  bank_account_id TEXT,
  envelope_json TEXT,
  frozen INTEGER NOT NULL DEFAULT 0,
  owner_phone TEXT,
  created_t INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS keys (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  class TEXT NOT NULL,                 -- ok | ak | bk
  hash TEXT NOT NULL UNIQUE,
  account INTEGER NOT NULL,
  program_id INTEGER,
  state TEXT NOT NULL,                 -- active | issued | deployed | paused | spent
  created_t INTEGER NOT NULL,
  last_used_t INTEGER
);
CREATE TABLE IF NOT EXISTS programs (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  account INTEGER NOT NULL,
  author_key_id INTEGER,
  bk_key_id INTEGER,
  backend TEXT NOT NULL,
  instruments_json TEXT NOT NULL,
  source TEXT NOT NULL,
  source_hash TEXT NOT NULL,
  compiled_json TEXT,
  result_json TEXT,
  status TEXT NOT NULL,   -- static_error|rejected|budget_refused|instrument_busy|certified|deployed|paused|halted|cancelled
  violates TEXT,
  bound_loss INTEGER, bound_fees INTEGER, bound_trades INTEGER, bound_exposure INTEGER,
  locked_collateral INTEGER, fund_cents INTEGER,
  sub_account INTEGER, order_ids_json TEXT, escrow_account_id TEXT,
  realized_loss INTEGER, realized_fees INTEGER, realized_trades INTEGER,
  incidents_json TEXT NOT NULL DEFAULT '[]',
  settled_t INTEGER,
  cancel_requested_t INTEGER,
  parent_program_id INTEGER,
  created_t INTEGER NOT NULL, deployed_t INTEGER, ended_t INTEGER
);
CREATE INDEX IF NOT EXISTS programs_account ON programs(account, status);
CREATE TABLE IF NOT EXISTS ledger (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  account INTEGER NOT NULL,
  program_id INTEGER NOT NULL,
  kind TEXT NOT NULL,                  -- reserve | realize | release
  cents_loss INTEGER NOT NULL,
  cents_fees INTEGER NOT NULL,
  trades INTEGER NOT NULL,
  exposure INTEGER NOT NULL DEFAULT 0,
  t INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS ledger_account ON ledger(account, kind, t);
CREATE TABLE IF NOT EXISTS events (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  account INTEGER,
  program_id INTEGER,
  type TEXT NOT NULL,
  payload_json TEXT NOT NULL,
  t INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS events_account ON events(account, id);
CREATE TABLE IF NOT EXISTS bank_accounts (   -- the local bank simulator (used when NESSIE_API_KEY is unset)
  id TEXT PRIMARY KEY,
  owner TEXT NOT NULL,
  nickname TEXT NOT NULL,
  balance_cents INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS bank_tx (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  from_id TEXT, to_id TEXT, cents INTEGER NOT NULL, description TEXT, t INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS phone_sessions (
  session_id TEXT PRIMARY KEY,
  account INTEGER NOT NULL,
  pending_envelope_json TEXT,
  updated_t INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS meta (k TEXT PRIMARY KEY, v TEXT NOT NULL);
"""


def now_ms() -> int:
    return int(time.time() * 1000)


class Database:
    def __init__(self, path: str | None = None):
        self.path = path or config.DB_PATH
        if self.path != ":memory:":
            Path(self.path).parent.mkdir(parents=True, exist_ok=True)
        self._local = threading.local()
        self._memory_conn: sqlite3.Connection | None = None
        self._lock = threading.RLock()
        with self.connect() as c:
            c.executescript(SCHEMA)

    def _new_conn(self) -> sqlite3.Connection:
        conn = sqlite3.connect(self.path, timeout=10, isolation_level=None, check_same_thread=False)
        conn.row_factory = sqlite3.Row
        conn.execute("PRAGMA busy_timeout=10000")
        if self.path != ":memory:":
            conn.execute("PRAGMA journal_mode=WAL")
        conn.execute("PRAGMA foreign_keys=ON")
        return conn

    @contextmanager
    def connect(self):
        """A connection for reads and single-statement writes (autocommit)."""
        if self.path == ":memory:":
            # one shared connection, serialised by a lock
            with self._lock:
                if self._memory_conn is None:
                    self._memory_conn = self._new_conn()
                yield self._memory_conn
            return
        conn = getattr(self._local, "conn", None)
        if conn is None:
            conn = self._new_conn()
            self._local.conn = conn
        yield conn

    @contextmanager
    def tx(self):
        """BEGIN IMMEDIATE … COMMIT. Two gateway threads cannot both fit into the last of the budget."""
        with self.connect() as conn:
            conn.execute("BEGIN IMMEDIATE")
            try:
                yield conn
            except BaseException:
                conn.execute("ROLLBACK")
                raise
            else:
                conn.execute("COMMIT")

    # ---- small helpers -------------------------------------------------
    def insert_event(self, conn: sqlite3.Connection, account: int | None, program_id: int | None,
                     type_: str, payload: dict, t: int | None = None) -> int:
        t = t if t is not None else payload.get("t") or now_ms()
        body = {"contract_version": 1, "type": type_, **payload}
        body.setdefault("t", t)
        if account is not None:
            body.setdefault("account", account)
        if program_id is not None:
            body.setdefault("program_id", program_id)
        cur = conn.execute(
            "INSERT INTO events(account, program_id, type, payload_json, t) VALUES (?,?,?,?,?)",
            (account, program_id, type_, json.dumps(body), t),
        )
        return int(cur.lastrowid)


def row_to_dict(row) -> dict:
    return {k: row[k] for k in row.keys()} if row is not None else None


def jloads(s, default=None):
    if s is None:
        return default
    return json.loads(s)
