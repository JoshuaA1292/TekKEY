"""B10: the federated view. One row per account and one row per delegate model, joined from
three sources that each hold part of the truth:

  - the gateway's SQLite (programs, certificates, the ledger, events)   [gateway/db.py]
  - the venue's state, read live through `venue.reconcile()`           [fake or real venued]
  - the bank's balances, read live through `bank.balance(account_id)`  [LocalBank or Nessie]

    python -m chalk.federated --db gateway/data/gateway.db --out evals/results/federated.json \
        [--results evals/results/<run>.json] [--venue fake|real]

What is real: every join in `build_view`. Rows come from SQL over the gateway database, from the
venue object's reconcile call and from the bank object's balance call, nothing is synthesised.
The model attribution comes from `delegate.run` events the eval runner writes into the events
table (payload `model`, `route`, `brief_id`, `attack_id`), or, when a results file is given,
from that file's `model` field for the accounts it lists.

What is not: the upload. If CHALK_CLIENT_ID and CHALK_CLIENT_SECRET are set and the `chalkpy`
client can be imported, `maybe_upload` posts the eval results and the view to Chalk as a dataset
through `ChalkClient.upload_features`. Without credentials, or without the package, it logs that
the upload was skipped and returns `{"uploaded": false, ...}`. It has not been exercised against a
live Chalk environment from this repository. Note also that this repository's own `chalk`
package shadows chalkpy's import name (`chalk`), so the guarded import `from chalk.client import
ChalkClient` only resolves when the installed chalkpy comes first on sys.path; otherwise the
upload is skipped and the log line says why. Nothing is installed by this module.

The dataset shape (lab/CONTRACT.md §6, "Chalk datasets"): `dataset_rows` turns the view into flat
rows keyed `delegate_model.<feature>`, and `push_rows` pushes any such rows as a dataset revision
through chalkpy when credentials and the package exist, otherwise writes them to
`evals/results/chalk/<dataset>.<revision>.json` in the same shape, so a later upload can replay
the file unchanged. `maybe_upload` is the chalkpy leg alone (its return shape is pinned by
evals/tests); `main` and the lab use `push_rows`, which also leaves the file.
"""
from __future__ import annotations

import argparse
import json
import logging
import os
import sys
import time
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

log = logging.getLogger("chalk.federated")

LIVE = ("certified", "deployed", "paused")
ALARMS = ("budget.low", "agent.offline", "rejections.burst", "invariant.violated", "account.frozen")


def _rows(c, sql: str, args: tuple = ()) -> list[dict]:
    return [dict(r) for r in c.execute(sql, args).fetchall()]


def build_view(db, venue, bank, results: dict | None = None) -> dict[str, Any]:
    """Join gateway SQLite, venue state and bank balances into per-account and per-model rows."""
    from gateway import budget as budget_mod
    from gateway.db import jloads
    from gateway.venue import VenueError

    # ---- venue: live state per program, one call ------------------------------------------------
    venue_states: dict[int, dict] = {}
    venue_error = None
    try:
        for st in venue.reconcile():
            if st.get("program_id") is not None:
                venue_states[int(st["program_id"])] = st
    except VenueError as e:
        venue_error = e.error
    except Exception as e:                      # a venue that is down is a fact worth reporting, not a crash
        venue_error = repr(e)

    # ---- results file: model per account, if given ------------------------------------------
    model_by_account: dict[int, str] = {}
    if results:
        for r in results.get("briefs", []) + results.get("attack_runs", []):
            if r.get("account") is not None:
                model_by_account[int(r["account"])] = results.get("model", "unknown")

    accounts_out = []
    with db.connect() as c:
        accounts = _rows(c, "SELECT * FROM accounts ORDER BY id")
        for a in accounts:
            aid = int(a["id"])
            envelope = jloads(a.get("envelope_json"))
            programs = _rows(c, "SELECT id, status, violates, bound_loss, bound_fees, bound_trades, bound_exposure, "
                                "realized_loss, realized_fees, realized_trades, fund_cents, parent_program_id, created_t, "
                                "deployed_t, ended_t, settled_t FROM programs WHERE account=? ORDER BY id", (aid,))
            ledger = _rows(c, "SELECT kind, COUNT(*) AS n, SUM(cents_loss) AS loss, SUM(cents_fees) AS fees, SUM(trades) AS trades "
                              "FROM ledger WHERE account=? GROUP BY kind", (aid,))
            ev = _rows(c, "SELECT type, COUNT(*) AS n FROM events WHERE account=? GROUP BY type", (aid,))
            meta = _rows(c, "SELECT payload_json FROM events WHERE account=? AND type='delegate.run' ORDER BY id DESC LIMIT 1", (aid,))
            run_meta = json.loads(meta[0]["payload_json"]) if meta else {}
            model = run_meta.get("model") or model_by_account.get(aid) or "unknown"
            remaining = budget_mod.remaining(c, aid, envelope).as_dict() if envelope else None
            balance = None
            bank_error = None
            if a.get("bank_account_id"):
                try:
                    balance = bank.balance(a["bank_account_id"])
                except Exception as e:
                    bank_error = repr(e)
            by_status: dict[str, int] = {}
            for p in programs:
                by_status[p["status"]] = by_status.get(p["status"], 0) + 1
            live = [{"program_id": p["id"], "gateway_status": p["status"], "bound_loss_cents": p["bound_loss"],
                     "venue": venue_states.get(int(p["id"]))} for p in programs if p["status"] in LIVE]
            ended = [p for p in programs if p["status"] in ("halted", "cancelled")]
            breaches = [p["id"] for p in ended if p["realized_loss"] is not None and p["bound_loss"] is not None
                        and int(p["realized_loss"]) > int(p["bound_loss"])]
            accounts_out.append({
                "account": aid, "frozen": bool(a["frozen"]), "model": model, "llm_route": run_meta.get("route"),
                "brief_id": run_meta.get("brief_id"), "attack_id": run_meta.get("attack_id"),
                "envelope": {"max_loss_cents": envelope["max_loss_cents"], "max_fees_cents": envelope["max_fees_cents"],
                             "max_trades": envelope["max_trades"], "max_exposure_lots": envelope["max_exposure_lots"],
                             "max_gap_ticks": envelope["env"]["max_gap_ticks"]} if envelope else None,
                "programs": {"total": len(programs), "by_status": by_status,
                             "certified": sum(1 for p in programs if p["bound_loss"] is not None and p["status"] not in ("budget_refused", "instrument_busy")),
                             "rejected": by_status.get("rejected", 0), "static_error": by_status.get("static_error", 0),
                             "budget_refused": by_status.get("budget_refused", 0), "instrument_busy": by_status.get("instrument_busy", 0)},
                "ledger": {r["kind"]: {"rows": r["n"], "loss_cents": r["loss"] or 0, "fees_cents": r["fees"] or 0, "trades": r["trades"] or 0}
                           for r in ledger},
                "budget": remaining,
                "realized": {"loss_cents": sum(int(p["realized_loss"] or 0) for p in ended),
                             "fees_cents": sum(int(p["realized_fees"] or 0) for p in ended),
                             "trades": sum(int(p["realized_trades"] or 0) for p in ended)},
                "bound_sum_cents": sum(int(p["bound_loss"] or 0) for p in programs if p["bound_loss"] is not None),
                "breaches": breaches,
                "live_programs": live,
                "bank": {"name": bank.name, "account_id": a.get("bank_account_id"), "balance_cents": balance, "error": bank_error},
                "events": {r["type"]: r["n"] for r in ev},
                "alarms": {t: n for t, n in ((r["type"], r["n"]) for r in ev) if t in ALARMS},
            })

    # ---- per model ---------------------------------------------------------------------------
    models: dict[str, dict] = {}
    for row in accounts_out:
        m = models.setdefault(row["model"], {"model": row["model"], "accounts": 0, "programs": 0, "certified": 0, "rejected": 0,
                                            "static_error": 0, "budget_refused": 0, "instrument_busy": 0, "halted_or_cancelled": 0,
                                            "realized_loss_cents": 0, "realized_fees_cents": 0, "bound_sum_cents": 0,
                                            "bank_balance_cents": 0, "breaches": 0, "alarms": {}, "frozen_accounts": 0})
        m["accounts"] += 1
        m["programs"] += row["programs"]["total"]
        for k in ("certified", "rejected", "static_error", "budget_refused", "instrument_busy"):
            m[k] += row["programs"][k]
        m["halted_or_cancelled"] += row["programs"]["by_status"].get("halted", 0) + row["programs"]["by_status"].get("cancelled", 0)
        m["realized_loss_cents"] += row["realized"]["loss_cents"]
        m["realized_fees_cents"] += row["realized"]["fees_cents"]
        m["bound_sum_cents"] += row["bound_sum_cents"]
        m["bank_balance_cents"] += row["bank"]["balance_cents"] or 0
        m["breaches"] += len(row["breaches"])
        m["frozen_accounts"] += int(row["frozen"])
        for t, n in row["alarms"].items():
            m["alarms"][t] = m["alarms"].get(t, 0) + n
    if results and results.get("model") in models:
        models[results["model"]]["eval_metrics"] = results.get("metrics")
        models[results["model"]]["eval_route"] = results.get("llm_route")

    return {
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "sources": {"gateway_db": getattr(db, "path", None), "venue": getattr(venue, "name", None), "venue_error": venue_error,
                    "venue_programs_seen": len(venue_states), "bank": getattr(bank, "name", None),
                    "results_file_model": results.get("model") if results else None},
        "accounts": accounts_out,
        "models": sorted(models.values(), key=lambda m: m["model"]),
    }


DATASET_DIR = ROOT / "evals" / "results" / "chalk"


def dataset_rows(view: dict, results: dict | None = None) -> list[dict[str, Any]]:
    """One flat row per delegate model, feature-named the way Chalk datasets expect them."""
    rows = []
    for m in view.get("models", []):
        rows.append({"delegate_model.id": m["model"], "delegate_model.accounts": m["accounts"],
                     "delegate_model.programs": m["programs"], "delegate_model.certified": m["certified"],
                     "delegate_model.rejected": m["rejected"], "delegate_model.realized_loss_cents": m["realized_loss_cents"],
                     "delegate_model.breaches": m["breaches"],
                     "delegate_model.eval_results_json": json.dumps(results.get("metrics") if results else None)})
    return rows


def _write_dataset_file(dataset: str, rows: list[dict], out_dir: Path | None = None, meta: dict | None = None) -> Path:
    out_dir = out_dir or DATASET_DIR
    out_dir.mkdir(parents=True, exist_ok=True)
    revision = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
    path = out_dir / f"{dataset}.{revision}.json"
    doc = {"dataset": dataset, "revision": revision, "generated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
           "row_count": len(rows), "columns": sorted({k for r in rows for k in r}), "rows": rows, **(meta or {})}
    path.write_text(json.dumps(doc, indent=1) + "\n")
    return path


def _upload_rows(rows: list[dict[str, Any]]) -> dict[str, Any]:
    """The chalkpy leg only. `{"uploaded": False, "reason": ...}` without credentials or the package."""
    cid, secret = os.environ.get("CHALK_CLIENT_ID"), os.environ.get("CHALK_CLIENT_SECRET")
    if not (cid and secret):
        log.info("Chalk upload skipped: CHALK_CLIENT_ID / CHALK_CLIENT_SECRET not set")
        return {"uploaded": False, "reason": "no_credentials"}
    try:
        from chalk.client import ChalkClient  # chalkpy; shadowed by this repo's own `chalk` package, see docstring
    except ImportError as e:
        log.warning("Chalk upload skipped: chalkpy client not importable (%s); not installing anything", e)
        return {"uploaded": False, "reason": "chalkpy_not_importable", "detail": str(e)}
    try:
        client = ChalkClient(client_id=cid, client_secret=secret,
                             environment=os.environ.get("CHALK_ENVIRONMENT") or None)
        resp = client.upload_features(input=rows)
        log.info("Chalk upload done (%s rows): %s", len(rows), resp)
        return {"uploaded": True, "rows": len(rows), "response": str(resp)[:500]}
    except Exception as e:                       # the rows are still written locally by the caller
        log.warning("Chalk upload failed: %r", e)
        return {"uploaded": False, "reason": "upload_failed", "detail": repr(e)}


def push_rows(dataset: str, rows: list[dict[str, Any]], out_dir: Path | None = None) -> dict[str, Any]:
    """lab/CONTRACT.md §6: push `rows` as a revision of `dataset` through chalkpy when it can,
    and in every case leave the same rows in dataset shape under evals/results/chalk/ so the
    revision can be replayed. Never raises; the return says what happened and where the file is."""
    out: dict[str, Any] = {"dataset": dataset, "rows": len(rows), **_upload_rows(rows)}
    try:
        path = _write_dataset_file(dataset, rows, out_dir, meta={"uploaded": out["uploaded"], "reason": out.get("reason")})
        out["path"] = str(path)
    except OSError as e:
        out["file_error"] = repr(e)
    return out


def maybe_upload(results: dict | None, view: dict) -> dict[str, Any]:
    """POST the eval results and the view to Chalk as a dataset, if credentials and chalkpy exist.
    Otherwise log that the upload was skipped. See the module docstring: this path is guarded and
    has not been exercised against a live Chalk environment from this repository. `push_rows` is
    the same call plus the dataset-shaped file."""
    return _upload_rows(dataset_rows(view, results))


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="federated view over gateway SQLite, venue state and bank balances")
    ap.add_argument("--db", default=None, help="gateway SQLite path (default: config.DB_PATH)")
    ap.add_argument("--out", default=str(ROOT / "evals" / "results" / "federated.json"))
    ap.add_argument("--results", default=None, help="an evals/results/*.json file to attribute models and attach metrics")
    ap.add_argument("--venue", default=os.environ.get("VENUE", "fake"), choices=["fake", "real"])
    ap.add_argument("--no-upload", action="store_true")
    a = ap.parse_args(argv)
    logging.basicConfig(level=os.environ.get("LOG_LEVEL", "INFO"), format="%(levelname)s %(name)s: %(message)s")
    os.environ.setdefault("VENUE", a.venue)
    from gateway import config
    from gateway.db import Database
    from gateway.nessie import make_bank
    from gateway.venue import make_venue

    db_path = a.db or config.DB_PATH
    db = Database(db_path)
    if a.venue == "fake":
        from gateway.fake_venue import FakeVenue
        venue = FakeVenue(state_path=str(Path(db_path).parent / "fake_venue.json"), autoplay=False)
    else:
        venue = make_venue("real")
    bank = make_bank(db)
    results = json.loads(Path(a.results).read_text()) if a.results else None
    view = build_view(db, venue, bank, results)
    view["upload"] = ({"uploaded": False, "reason": "disabled"} if a.no_upload
                      else push_rows("delegate_models", dataset_rows(view, results)))
    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(view, indent=1) + "\n")
    print(f"{len(view['accounts'])} accounts, {len(view['models'])} models -> {out}  (venue programs seen: "
          f"{view['sources']['venue_programs_seen']}, upload: {view['upload']['reason'] if not view['upload']['uploaded'] else 'ok'})")
    if hasattr(venue, "close"):
        venue.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
