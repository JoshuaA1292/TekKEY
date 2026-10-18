"""Fast checks on the eval suite: the runner produces a results file with breach rate 0 on the
fake venue, every certified program's deploy and halt went through the ledger, and the dataset
generator is deterministic."""
from __future__ import annotations

import json
import os
import sqlite3
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
os.environ.setdefault("VENUE", "fake")
os.environ["GATEWAY_LAZY"] = "1"

from evals import run as run_mod  # noqa: E402
from evals.data import generate  # noqa: E402

DATA = ROOT / "evals" / "data"


@pytest.fixture(scope="module")
def stub_run(tmp_path_factory):
    tmp = tmp_path_factory.mktemp("evals")
    out = tmp / "stub.json"
    db = tmp / "gateway.db"
    code = run_mod.main(["--model", "stub", "--briefs", "5", "--attacks", "8", "--attacks-per-brief", "2",
                         "--out", str(out), "--db", str(db), "--quiet"])
    return {"code": code, "out": out, "db": db, "results": json.loads(out.read_text())}


def test_runner_writes_results_with_zero_breach(stub_run):
    assert stub_run["code"] == 0
    assert stub_run["out"].exists()
    r = stub_run["results"]
    m = r["metrics"]
    assert r["llm_route"] == "stub" and r["model"] == "stub"
    assert r["dataset"]["briefs"] == 5 and r["dataset"]["attacks"] == 8
    assert m["breach"]["breach_rate"] == 0
    assert m["breach"]["n_programs_checked"] >= 1, "at least one program must certify on the stub"
    assert m["breach"]["n_programs_checked"] == m["n_certified_final"] + sum(
        1 for a in r["attack_runs"] if a["certified"])
    for b in r["breach_checks"]:
        assert b["ok"], b
        assert b["realized_loss_cents"] <= b["bound_max_loss_cents"]
        assert b["realized_in_window_cents"] <= b["envelope_max_loss_cents"]
        assert b["replay_matches_verifier"]
    # the four classes are all represented when --attacks 8 is taken round-robin
    assert {a["class"] for a in run_mod.select_attacks(generate.load_attacks(), 8)} == set(generate.CLASSES)
    assert m["raw_key_drain"]["simulated"] is True


def test_every_certified_program_went_through_the_ledger(stub_run):
    """Deploy reserves, halt releases and realizes, and remaining <= max_loss, read from the DB."""
    r = stub_run["results"]
    conn = sqlite3.connect(stub_run["db"])
    conn.row_factory = sqlite3.Row
    certified = [row for row in r["briefs"] + r["attack_runs"] if row["certified"]]
    assert certified
    for row in certified:
        pid, account = row["program_id"], row["account"]
        kinds = {k["kind"]: k for k in conn.execute(
            "SELECT kind, cents_loss FROM ledger WHERE program_id=? ORDER BY id", (pid,)).fetchall()}
        assert set(kinds) == {"reserve", "release", "realize"}, (pid, set(kinds))
        prog = conn.execute("SELECT status, realized_loss, bound_loss FROM programs WHERE id=?", (pid,)).fetchone()
        assert prog["status"] in ("halted", "cancelled")
        assert prog["realized_loss"] <= prog["bound_loss"]
        assert kinds["realize"]["cents_loss"] == prog["realized_loss"]
        assert kinds["reserve"]["cents_loss"] == prog["bound_loss"]
        env = json.loads(conn.execute("SELECT envelope_json FROM accounts WHERE id=?", (account,)).fetchone()[0])
        realized = conn.execute("SELECT COALESCE(SUM(cents_loss),0) FROM ledger WHERE account=? AND kind='realize'",
                                (account,)).fetchone()[0]
        reserved = conn.execute(
            "SELECT COALESCE(SUM(l.cents_loss),0) FROM ledger l WHERE l.account=? AND l.kind='reserve' AND NOT EXISTS "
            "(SELECT 1 FROM ledger r WHERE r.program_id=l.program_id AND r.kind='release')", (account,)).fetchone()[0]
        remaining = env["max_loss_cents"] - realized - reserved
        assert 0 <= remaining <= env["max_loss_cents"]
        halted = conn.execute("SELECT COUNT(*) FROM events WHERE program_id=? AND type='program.halted'", (pid,)).fetchone()[0]
        assert halted >= 1
        frozen = conn.execute("SELECT frozen FROM accounts WHERE id=?", (account,)).fetchone()[0]
        assert frozen == 0, "the ledger invariant froze an account"
    conn.close()


def test_federated_view_joins_the_three_sources(stub_run):
    from chalk import federated
    from gateway.db import Database
    from gateway.fake_venue import FakeVenue
    from gateway.nessie import LocalBank

    db = Database(str(stub_run["db"]))
    venue = FakeVenue(fixtures_dir=ROOT / "gateway" / "fixtures", state_path=str(stub_run["db"].parent / "fake_venue.json"),
                      autoplay=False)
    view = federated.build_view(db, venue, LocalBank(db), stub_run["results"])
    venue.close()
    assert view["accounts"] and view["models"]
    models = {m["model"]: m for m in view["models"]}
    assert "stub" in models and models["stub"]["breaches"] == 0
    assert models["stub"]["eval_metrics"]["breach"]["breach_rate"] == 0
    acct = next(a for a in view["accounts"] if a["programs"]["by_status"].get("halted"))
    assert acct["bank"]["balance_cents"] is not None
    assert acct["ledger"]["realize"]["loss_cents"] == acct["realized"]["loss_cents"]
    assert view["sources"]["venue_programs_seen"] >= 1
    # no credentials in the test environment: the upload is skipped and says so
    os.environ.pop("CHALK_CLIENT_ID", None)
    assert federated.maybe_upload(stub_run["results"], view) == {"uploaded": False, "reason": "no_credentials"}


def test_generate_is_deterministic(tmp_path):
    a, b = tmp_path / "a", tmp_path / "b"
    generate.generate(a)
    generate.generate(b)
    for name in ("briefs.json", "attacks.json"):
        assert (a / name).read_bytes() == (b / name).read_bytes()
        assert (a / name).read_bytes() == (DATA / name).read_bytes(), f"committed {name} is stale: run python -m evals.data.generate"
    briefs = json.loads((a / "briefs.json").read_text())["briefs"]
    attacks = json.loads((a / "attacks.json").read_text())["attacks"]
    assert len(briefs) == 50 and len(attacks) == 200
    assert len({x["text"] for x in attacks}) == 200
    for cls in generate.CLASSES:
        assert sum(1 for x in attacks if x["class"] == cls) == 50
    for b_ in briefs:
        env = b_["envelope"]
        assert env["instruments"] == ["ETH"] and env["window"] == "24h_rolling" and env["require_live_stop"] is True
        assert 2000 <= env["max_loss_cents"] <= 8000 and 10 <= env["env"]["max_gap_ticks"] <= 30
        assert env["env"]["fee_bps"] == {"maker": 1.0, "taker": 3.5} and env["env"]["fill"] == "full_at_print"
    assert all(a_["intent"] in generate.INTENTS for a_ in attacks)
