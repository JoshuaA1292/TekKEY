"""SQLite persistence for the lab: `lab_runs` (one row per run, the CONTRACT §2 JSON) and
`lab_events` (everything streamed to the page, replayable from `Last-Event-ID`).

Both tables live in the gateway's own database (`gateway.db.Database`), so a run's evidence
(program ids) points at rows in the same file. While a run is in progress its log, metrics,
rows and series are assembled from `lab_events`; when it finishes the complete result is
written once to `lab_runs.result_json`.
"""
from __future__ import annotations

import json
import secrets
import time
from typing import Any

from gateway.db import Database, now_ms

SCHEMA = """
CREATE TABLE IF NOT EXISTS lab_runs (
  run_id TEXT PRIMARY KEY,
  test TEXT NOT NULL,
  model TEXT NOT NULL,
  route TEXT,
  venue TEXT,
  status TEXT NOT NULL,            -- queued | running | passed | failed | error | skipped | completed
  queued_t INTEGER NOT NULL,
  started_t INTEGER,
  ended_t INTEGER,
  visitor TEXT,
  source TEXT,                     -- visitor | schedule | test
  params_json TEXT NOT NULL DEFAULT '{}',
  result_json TEXT
);
CREATE INDEX IF NOT EXISTS lab_runs_pair ON lab_runs(test, model, queued_t);
CREATE TABLE IF NOT EXISTS lab_events (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  run_id TEXT,
  type TEXT NOT NULL,
  data_json TEXT NOT NULL,
  t INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS lab_events_run ON lab_events(run_id, id);
"""

FINAL = ("passed", "failed", "error", "skipped", "completed")
PENDING = ("queued", "running")
RESULT_KEYS = ("headline", "metrics", "rows", "series", "log", "evidence", "skipped_reason", "stub", "note")


def is_finished(run: dict) -> bool:
    """A run that ran to the end: passed, failed, completed or skipped, or an error the test itself
    reported (it carries a headline). A row the scheduler marked `error` because the process
    restarted while it was pending has no headline and is NOT a finished run: it must never be
    shown as a card's result (CONTRACT C6)."""
    st = run.get("status")
    if st in ("passed", "failed", "completed", "skipped"):
        return True
    if st == "error":
        return bool(run.get("headline"))
    return False


def new_run_id() -> str:
    """Time-ordered and unguessable: r_<ms since epoch><4 random hex>."""
    return f"r_{int(time.time() * 1000):013d}{secrets.token_hex(2)}"


class LabStore:
    def __init__(self, db: Database):
        self.db = db
        with db.connect() as c:
            c.executescript(SCHEMA)

    # ------------------------------------------------------------------ runs
    def create_run(self, test: str, model: str, route: str | None, venue: str | None, params: dict | None = None,
                   visitor: str | None = None, source: str = "visitor", position: int = 0) -> dict:
        run_id = new_run_id()
        t = now_ms()
        with self.db.tx() as c:
            c.execute("INSERT INTO lab_runs(run_id, test, model, route, venue, status, queued_t, visitor, source, params_json) "
                      "VALUES (?,?,?,?,?,?,?,?,?,?)",
                      (run_id, test, model, route, venue, "queued", t, visitor, source, json.dumps(params or {})))
        self.add_event(run_id, "run.queued", {"test": test, "model": model, "route": route, "venue": venue, "position": position})
        return self.get_run(run_id)

    def start_run(self, run_id: str, route: str | None = None, venue: str | None = None) -> None:
        t = now_ms()
        with self.db.tx() as c:
            c.execute("UPDATE lab_runs SET status='running', started_t=?, route=COALESCE(?, route), venue=COALESCE(?, venue) "
                      "WHERE run_id=?", (t, route, venue, run_id))
            row = c.execute("SELECT test, model FROM lab_runs WHERE run_id=?", (run_id,)).fetchone()
        self.add_event(run_id, "run.started", {"test": row["test"], "model": row["model"], "route": route, "venue": venue,
                                                "started_t": t})

    def finish_run(self, run_id: str, status: str, result: dict) -> dict:
        """Writes the complete result once; the run.finished event carries the full §2 object."""
        assert status in FINAL, status
        with self.db.tx() as c:
            c.execute("UPDATE lab_runs SET status=?, ended_t=?, result_json=? WHERE run_id=?",
                      (status, now_ms(), json.dumps(result), run_id))
        run = self.get_run(run_id)
        self.add_event(run_id, "run.finished", run)
        return run

    def get_run(self, run_id: str, full: bool = True) -> dict | None:
        with self.db.connect() as c:
            row = c.execute("SELECT * FROM lab_runs WHERE run_id=?", (run_id,)).fetchone()
        if row is None:
            return None
        return self._result_of(dict(row), full)

    def list_runs(self, test: str | None = None, model: str | None = None, limit: int = 50) -> list[dict]:
        sql, args = "SELECT * FROM lab_runs", []
        conds = []
        if test:
            conds.append("test=?")
            args.append(test)
        if model:
            conds.append("model=?")
            args.append(model)
        if conds:
            sql += " WHERE " + " AND ".join(conds)
        sql += " ORDER BY queued_t DESC, run_id DESC LIMIT ?"
        args.append(int(limit))
        with self.db.connect() as c:
            rows = c.execute(sql, args).fetchall()
        return [self._result_of(dict(r), full=False) for r in rows]

    def latest_per_pair(self, statuses: tuple[str, ...] | None = None, finished: bool = True) -> dict[tuple[str, str], dict]:
        """The newest FINISHED run per (test, model) (CONTRACT C6), optionally restricted to some
        statuses. A queued or running run, or a row killed by a restart, never displaces the last
        real result; `finished=False` gives the old "newest row of any status" behaviour."""
        with self.db.connect() as c:
            rows = c.execute("SELECT * FROM lab_runs ORDER BY queued_t DESC, run_id DESC").fetchall()
        out: dict[tuple[str, str], dict] = {}
        for r in rows:
            d = dict(r)
            if statuses and d["status"] not in statuses:
                continue
            key = (d["test"], d["model"])
            if key in out:
                continue
            shaped = self._result_of(d, full=False)
            if finished and not is_finished(shaped):
                continue
            out[key] = shaped
        return out

    def pending_per_pair(self) -> dict[tuple[str, str], dict]:
        """The newest queued or running run per (test, model), shaped for the catalogue's `pending`."""
        with self.db.connect() as c:
            rows = c.execute("SELECT * FROM lab_runs WHERE status IN ('queued','running') "
                             "ORDER BY queued_t DESC, run_id DESC").fetchall()
        out: dict[tuple[str, str], dict] = {}
        for r in rows:
            d = dict(r)
            key = (d["test"], d["model"])
            if key not in out:
                out[key] = {"run_id": d["run_id"], "status": d["status"], "queued_t": d["queued_t"],
                            "started_t": d["started_t"], "source": d["source"]}
        return out

    def pending(self, test: str, model: str) -> dict | None:
        with self.db.connect() as c:
            row = c.execute("SELECT * FROM lab_runs WHERE test=? AND model=? AND status IN ('queued','running') "
                            "ORDER BY queued_t DESC LIMIT 1", (test, model)).fetchone()
        return self._result_of(dict(row), full=False) if row else None

    def requeue_stale(self) -> list[str]:
        """On start: runs left queued or running by a previous process are marked `error`."""
        with self.db.tx() as c:
            rows = c.execute("SELECT run_id FROM lab_runs WHERE status IN ('queued','running')").fetchall()
            ids = [r["run_id"] for r in rows]
            for rid in ids:
                c.execute("UPDATE lab_runs SET status='error', ended_t=?, result_json=? WHERE run_id=?",
                          (now_ms(), json.dumps({"note": "the gateway process restarted while this run was pending"}), rid))
        return ids

    # ------------------------------------------------------------------ events
    def add_event(self, run_id: str | None, type_: str, data: dict | None = None) -> int:
        body = {"run_id": run_id, "t": now_ms(), **(data or {})}
        with self.db.tx() as c:
            cur = c.execute("INSERT INTO lab_events(run_id, type, data_json, t) VALUES (?,?,?,?)",
                            (run_id, type_, json.dumps(body), body["t"]))
            return int(cur.lastrowid)

    def events_after(self, last_id: int, limit: int = 500) -> list[dict]:
        with self.db.connect() as c:
            rows = c.execute("SELECT id, type, data_json FROM lab_events WHERE id>? ORDER BY id LIMIT ?",
                             (int(last_id), int(limit))).fetchall()
        return [{"id": int(r["id"]), "type": r["type"], "data": r["data_json"]} for r in rows]

    def events_of_run(self, run_id: str) -> list[dict]:
        with self.db.connect() as c:
            rows = c.execute("SELECT id, type, data_json FROM lab_events WHERE run_id=? ORDER BY id", (run_id,)).fetchall()
        return [{"id": int(r["id"]), "type": r["type"], **json.loads(r["data_json"])} for r in rows]

    def last_event_id(self) -> int:
        with self.db.connect() as c:
            row = c.execute("SELECT MAX(id) AS m FROM lab_events").fetchone()
        return int(row["m"] or 0)

    # ------------------------------------------------------------------ shaping
    def _result_of(self, row: dict, full: bool) -> dict:
        base: dict[str, Any] = {
            "run_id": row["run_id"], "test": row["test"], "model": row["model"], "route": row["route"],
            "venue": row["venue"], "status": row["status"], "queued_t": row["queued_t"], "started_t": row["started_t"],
            "ended_t": row["ended_t"], "source": row["source"], "params": json.loads(row["params_json"] or "{}"),
            "skipped_reason": None, "headline": None, "metrics": {}, "rows": [], "series": {}, "log": [], "evidence": {},
        }
        if row.get("result_json"):
            res = json.loads(row["result_json"])
            for k in RESULT_KEYS:
                if k in res:
                    base[k] = res[k]
        elif row["status"] in ("queued", "running"):
            self._fill_from_events(base)
        if not full:
            base = {k: v for k, v in base.items() if k not in ("rows", "log", "series", "evidence")}
        return base

    def _fill_from_events(self, base: dict) -> None:
        for ev in self.events_of_run(base["run_id"]):
            if ev["type"] == "run.log":
                base["log"].append({"t": ev["t"], "msg": ev.get("msg"), "data": ev.get("data", {})})
            elif ev["type"] == "run.metric":
                base["metrics"][ev["name"]] = ev["value"]
            elif ev["type"] == "run.row":
                base["rows"].append(ev["row"])
            elif ev["type"] == "run.series":
                base["series"].setdefault(ev["name"], []).extend(ev["points"])
