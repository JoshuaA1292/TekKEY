"""The race routers parse their request bodies (a 503 'race offline' proves the body was accepted,
a 422 would mean FastAPI could not resolve the model) and answer offline cleanly. With a fake
conductor on app.state they also prove the contract of the epoch-scoped round listing (C1) and
of the public, rate-limited replay (C2) without a venue or a model."""
from fastapi import FastAPI
from fastapi.testclient import TestClient

from race import app as race_app
from race.app import backtest_router, router


def test_bodies_parse_and_offline_is_503():
    app = FastAPI()
    app.include_router(router)
    app.include_router(backtest_router)
    c = TestClient(app)
    assert c.get("/race/state").status_code == 503
    assert c.get("/race/rounds?epoch=3&limit=5&full=1").status_code == 503
    assert c.post("/race/message", json={"text": "hello both"}).status_code == 503
    assert c.post("/backtest/start", json={"token": "x", "start": "2025-10-10", "end": "2025-10-11", "model": "grok-4.7"}).status_code == 503
    assert c.post("/backtest/replay", json={"scenario": "crash"}).status_code == 503
    assert c.post("/backtest/replay", json={}).status_code == 503              # every field has a default
    assert c.post("/backtest/epoch", json={"token": "x"}).status_code == 503
    assert c.post("/race/message", json={}).status_code == 422          # a real validation error still is one


class FakeStore:
    def __init__(self):
        self.calls = []

    def rounds(self, epoch, limit=40, full=False):
        self.calls.append((epoch, limit, full))
        return [{"id": 10 * epoch + i, "round_id": 10 * epoch + i, "epoch": epoch, "n": i, "t": 1000 + i, "ended_t": 1001 + i,
                 "shared": [], "raw": {"full": full}, "bounded": {"full": full}} for i in range(1, 3)]


class FakeConductor:
    """What race.app reads off a conductor: mode, epoch, the window in progress, the store, start_backtest."""
    mode = "backtest"
    model = "grok-4.20-0309-non-reasoning"
    route = "chalk"

    def __init__(self):
        self.epoch = 42
        self.backtest = None
        self.finished = None
        self.store = FakeStore()
        self.started = []

    def start_backtest(self, start=None, end=None, speed=30.0, round_minutes=10.0, label=None, model=None):
        self.started.append({"start": start, "end": end, "speed": speed, "round_minutes": round_minutes, "label": label, "model": model})
        self.model = model or self.model
        self.route = "anthropic-direct" if self.model.startswith("claude") else "chalk"
        self.epoch += 1
        self.backtest = {"label": (label or "").replace("{model}", self.model).replace("{route}", self.route), "speed": speed}
        return {"epoch": self.epoch, "model": self.model, "route": self.route, "eta_s": 255, "label": self.backtest["label"]}


def _app(cond):
    app = FastAPI()
    app.include_router(backtest_router)
    app.include_router(router)
    app.state.backtest = cond
    app.state.race = None
    return TestClient(app)


def test_rounds_epoch_scoped_and_full(monkeypatch):
    cond = FakeConductor()
    c = _app(cond)
    r = c.get("/backtest/rounds?epoch=7&limit=5&full=1")
    assert r.status_code == 200
    body = r.json()
    assert body["epoch"] == 7 and [x["n"] for x in body["rounds"]] == [1, 2]          # oldest first
    assert set(body["rounds"][0]) >= {"id", "n", "t", "ended_t", "shared", "raw", "bounded"}
    assert body["rounds"][0]["raw"]["full"] is True
    assert cond.store.calls[-1] == (7, 5, True)
    r = c.get("/backtest/rounds")
    assert r.json()["epoch"] == 42 and cond.store.calls[-1] == (42, 40, False)         # without epoch: as before
    assert c.get("/backtest/rounds?epoch=7&limit=9999").status_code == 200 and cond.store.calls[-1][1] == 500


def test_replay_is_public_allow_listed_busy_aware_and_rate_limited(monkeypatch):
    now = [1_000_000.0]
    race_app.REPLAY_LIMITER.reset()
    monkeypatch.setattr(race_app.REPLAY_LIMITER, "clock", lambda: now[0])
    cond = FakeConductor()
    c = _app(cond)

    assert c.post("/backtest/replay", json={"scenario": "crash", "model": "gpt-9"}).status_code == 400
    assert c.post("/backtest/replay", json={"scenario": "moon"}).status_code == 400
    assert cond.started == []                                                        # refusals start nothing

    r = c.post("/backtest/replay", json={"scenario": "crash"}, headers={"X-Forwarded-For": "1.1.1.1, 10.0.0.1"})
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["ok"] is True and body["epoch"] == 43 and body["model"] == race_app.REPLAY_DEFAULT_MODEL and body["route"] == "chalk"
    assert body["eta_s"] == 255
    s = cond.started[-1]
    assert (s["start"], s["end"], s["speed"], s["round_minutes"]) == ("2025-10-10T16:21Z", "2025-10-10T21:21Z", 120.0, 10.0)
    assert s["label"] == "flash crash hours on {model} via {route}" and s["model"] == race_app.REPLAY_DEFAULT_MODEL
    assert cond.backtest["label"] == "flash crash hours on grok-4.20-0309-non-reasoning via chalk"

    # a window is now in progress: busy, whoever asks, before any rate limit
    r = c.post("/backtest/replay", json={"scenario": "crash"}, headers={"X-Forwarded-For": "2.2.2.2"})
    assert r.status_code == 409 and r.json()["detail"] == "busy"
    cond.finished = {"reason": "done"}

    # the same visitor inside 10 minutes: 429 (visitor); another visitor inside 4 minutes: 429 (global)
    now[0] += 200
    r = c.post("/backtest/replay", json={"scenario": "crash"}, headers={"X-Forwarded-For": "1.1.1.1"})
    assert r.status_code == 429 and "visitor" in r.json()["detail"] and r.headers["retry-after"]
    r = c.post("/backtest/replay", json={"scenario": "crash", "model": "claude-sonnet-5-5"}, headers={"X-Forwarded-For": "2.2.2.2"})
    assert r.status_code == 429 and "everyone" in r.json()["detail"] and int(r.json()["retry_after_s"]) <= 41
    # after 4 minutes another visitor may go, and the model it names is the one that runs
    now[0] += 50
    r = c.post("/backtest/replay", json={"scenario": "crash", "model": "claude-sonnet-5-5"}, headers={"X-Forwarded-For": "2.2.2.2"})
    assert r.status_code == 200 and r.json()["model"] == "claude-sonnet-5-5" and r.json()["route"] == "anthropic-direct"
    assert cond.backtest["label"] == "flash crash hours on claude-sonnet-5-5 via anthropic-direct"
    cond.finished = {"reason": "done"}
    # the first visitor still waits out the 10 minutes
    now[0] += 300
    assert c.post("/backtest/replay", json={"scenario": "crash"}, headers={"X-Forwarded-For": "1.1.1.1"}).status_code == 429
    now[0] += 100
    assert c.post("/backtest/replay", json={"scenario": "crash"}, headers={"X-Forwarded-For": "1.1.1.1"}).status_code == 200
    race_app.REPLAY_LIMITER.reset()


def test_replay_on_the_live_race_is_404_and_start_takes_a_model(monkeypatch):
    race_app.REPLAY_LIMITER.reset()
    cond = FakeConductor()
    cond.mode = "live"
    c = _app(cond)
    assert c.post("/backtest/replay", json={"scenario": "crash"}).status_code == 404
    cond.mode = "backtest"
    monkeypatch.setenv("RACE_ADMIN_TOKEN", "secret")
    assert c.post("/backtest/start", json={"token": "nope", "model": "grok-4.7"}).status_code == 403
    r = c.post("/backtest/start", json={"token": "secret", "start": "2025-10-10", "end": "2025-10-11", "model": "grok-4.7"})
    assert r.status_code == 200 and cond.started[-1]["model"] == "grok-4.7" and r.json()["model"] == "grok-4.7"
