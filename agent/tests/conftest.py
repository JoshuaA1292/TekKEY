"""The gateway in-process with the fake venue, exactly as gateway/tests/conftest.py builds it,
plus the delegate's view of it: a `GatewayClient` over `httpx.ASGITransport`."""
import json
import os
import sys
from pathlib import Path

import httpx
import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
os.environ.setdefault("VENUE", "fake")
os.environ["GATEWAY_LAZY"] = "1"
for var in ("CHALK_AI_GATEWAY_URL", "ANTHROPIC_API_KEY", "ANTHROPIC_AUTH_TOKEN", "DAYTONA_API_KEY"):
    os.environ.pop(var, None)                     # no network: the StubLLM and the local sandbox

from fastapi.testclient import TestClient  # noqa: E402

from chalk import ai_gateway  # noqa: E402
from gateway import app as app_mod  # noqa: E402
from gateway.db import Database  # noqa: E402
from gateway.fake_venue import FakeVenue  # noqa: E402
from gateway.nessie import LocalBank  # noqa: E402

from agent.client import GatewayClient  # noqa: E402

FIX = ROOT / "gateway" / "fixtures"


def H(key):
    return {"Authorization": f"Bearer {key}"}


@pytest.fixture
def envelope():
    return json.loads((FIX / "envelope_40.json").read_text())


@pytest.fixture
def venue(tmp_path):
    v = FakeVenue(fixtures_dir=FIX, state_path=str(tmp_path / "fake.json"), autoplay=False)
    yield v
    v.close()


@pytest.fixture
def db(tmp_path):
    return Database(str(tmp_path / "g.db"))


@pytest.fixture
def app(db, venue):
    return app_mod.build(db=db, venue=venue, bank=LocalBank(db), start_bus=False)


@pytest.fixture
def owner(app):
    """The owner's side of the setup. The delegate never sees this client."""
    return TestClient(app)


@pytest.fixture
def account(owner, envelope):
    r = owner.post("/account", json={"opening_balance_cents": 1_000_000_00})
    assert r.status_code == 200, r.text
    acct, ok = r.json()["account"], r.json()["owner_key"]
    env = dict(envelope, account=acct)
    assert owner.put(f"/account/{acct}/envelope", json=env, headers=H(ok)).status_code == 200
    r = owner.post(f"/account/{acct}/author-key", headers=H(ok)).json()
    return {"account": acct, "ok": ok, "ak": r["author_key"], "ak_id": r["key_id"], "envelope": env}


@pytest.fixture
def stub():
    llm = ai_gateway.StubLLM()
    ai_gateway.set_llm(llm)
    yield llm
    ai_gateway.set_llm(None)


@pytest.fixture
def client(app, account):
    """The delegate's client: author key only, in-process over ASGITransport."""
    c = GatewayClient("http://gateway", account["ak"], transport=httpx.ASGITransport(app=app))
    yield c
    c.close()
