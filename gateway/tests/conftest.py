import json
import os
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
os.environ.setdefault("VENUE", "fake")
os.environ["GATEWAY_LAZY"] = "1"

from fastapi.testclient import TestClient  # noqa: E402

from gateway import app as app_mod  # noqa: E402
from gateway.db import Database  # noqa: E402
from gateway.fake_venue import FakeVenue  # noqa: E402
from gateway.nessie import LocalBank  # noqa: E402

FIX = ROOT / "gateway" / "fixtures"


@pytest.fixture
def envelope():
    return json.loads((FIX / "envelope_40.json").read_text())


def src(name):
    return (FIX / f"{name}.mpl").read_text()


REAL = os.environ.get("GATEWAY_TEST_VENUE") == "real"


@pytest.fixture
def real_stack(tmp_path_factory):
    """CONTRACT §8 step 2: the same suite against Build A's exchange + venued, on scratch ports.
    A fresh exchange and venued per test, like the fresh FakeVenue per test: the mark starts clean."""
    import socket
    import subprocess
    import time

    from gateway import config as cfg
    from gateway.venue import RealVenue

    d = tmp_path_factory.mktemp("real")
    ex_port, vd_port = 7400, 7410
    build = ROOT / "minsky" / "build"
    ex = subprocess.Popen([str(build / "exchange"), "--tcp", str(ex_port), "--accounts", "255", "--feed", "7402"],
                          stdout=open(d / "exchange.log", "w"), stderr=subprocess.STDOUT)
    time.sleep(0.5)
    vd = subprocess.Popen([str(build / "venued"), "--listen", str(vd_port), "--exchange", f"tcp://127.0.0.1:{ex_port}",
                           "--state", str(d / "venued.state.json"), "--instruments", str(cfg.instruments_path())],
                          stdout=open(d / "venued.log", "w"), stderr=subprocess.STDOUT)
    for _ in range(50):
        try:
            socket.create_connection(("127.0.0.1", vd_port), timeout=0.2).close()
            break
        except OSError:
            time.sleep(0.1)
    cfg.EXCHANGE_URL = f"tcp://127.0.0.1:{ex_port}"
    yield {"port": vd_port}
    vd.terminate()
    ex.terminate()
    vd.wait(timeout=5)
    ex.wait(timeout=5)
    time.sleep(0.2)


if REAL:
    @pytest.fixture
    def venue(real_stack):
        from gateway.venue import RealVenue
        return RealVenue(port=real_stack["port"])
else:
    @pytest.fixture
    def venue(tmp_path):
        v = FakeVenue(fixtures_dir=FIX, state_path=str(tmp_path / "fake.json"), autoplay=False)
        yield v
        v.close()


@pytest.fixture
def db(tmp_path):
    d = Database(str(tmp_path / "g.db"))
    if REAL:
        # program ids are assigned by Build B and must be unique at the venue; every test gets a fresh
        # database but shares one venued, so start each database's sequence at a unique base.
        import time
        with d.connect() as c:
            c.execute("INSERT OR REPLACE INTO sqlite_sequence(name, seq) VALUES ('programs', ?)",
                      (int(time.time() * 1000) % 1_000_000_000 * 100,))
    return d


@pytest.fixture
def app(db, venue):
    return app_mod.build(db=db, venue=venue, bank=LocalBank(db), start_bus=True)


@pytest.fixture
def client(app):
    with TestClient(app) as c:
        yield c


def H(key):
    return {"Authorization": f"Bearer {key}"}


@pytest.fixture
def account(client, envelope):
    """An account with the §5.1 envelope, an owner key and an author key."""
    r = client.post("/account", json={"opening_balance_cents": 1_000_000_00})
    assert r.status_code == 200, r.text
    acct, ok = r.json()["account"], r.json()["owner_key"]
    env = dict(envelope, account=acct)
    assert client.put(f"/account/{acct}/envelope", json=env, headers=H(ok)).status_code == 200
    ak = client.post(f"/account/{acct}/author-key", headers=H(ok)).json()["author_key"]
    return {"account": acct, "ok": ok, "ak": ak, "envelope": env}
