"""The race's routes, mounted into the gateway app by `gateway/app.py` (`install(app)` builds the
race world, the store and the conductor, and starts them; `router` serves the page's reads, the
event stream and the visitor message box). When the race venue is not reachable the routes
answer 503 with the reason, and the page says so.

    GET  /race/state            everything for first paint (lanes, tape, equity, rounds, fills, history)
    GET  /race/rounds?limit=    rounds, newest last, without prompt texts; &epoch=N reads another epoch, &full=1 adds
                                the prompts and replies (both conductors; {"epoch":N,"rounds":[{id,n,t,ended_t,shared,raw,bounded}]})
    GET  /race/rounds/{id}      one round in full: both lanes' prompts, replies, results, shared hash
    GET  /race/tape?minutes=    the prints of the last N minutes
    GET  /race/fills?limit=     fills of both lanes
    GET  /race/stream           SSE: print, equity, fill, round.started, lane.decision, lane.result, round.finished,
                                program, program.ended, breach, liquidation, liquidated, epoch, route, message.queued, lane.state
    POST /race/message {text}   a message for both agents, appended to the next round's transcript (3 per 10 min per visitor)
    POST /race/epoch {token}    start a new epoch (RACE_ADMIN_TOKEN)
    POST /backtest/start {token,start,end,speed,round_minutes,label,model}   replay a window (RACE_ADMIN_TOKEN); `model` runs it on another id
    POST /backtest/replay {scenario,model}   PUBLIC: replay a named scenario ("crash" = the 10 October 2025 flash crash hours at 120x)
                                on an allow-listed model; 409 busy while a window runs; 1 per 10 min per visitor, 1 per 4 min overall
"""
from __future__ import annotations

import logging
import os
import pathlib
import threading
import time
from collections import deque

from fastapi import APIRouter, Request
from fastapi.responses import JSONResponse, StreamingResponse
from pydantic import BaseModel

log = logging.getLogger("race.app")

RATE_LIMIT_N, RATE_LIMIT_WINDOW_S = 3, 600
_visitors: dict[str, deque] = {}
_vlock = threading.Lock()

# ---- public replays (C2) ---------------------------------------------------------------------------
REPLAY_MODELS = ["grok-4.20-0309-non-reasoning", "grok-4.7", "grok-4.20-0309-reasoning", "claude-sonnet-5-5"]
REPLAY_DEFAULT_MODEL = REPLAY_MODELS[0]
REPLAY_SCENARIOS = {
    # 16:21-21:21 UTC on 10 October 2025: ETH 4140 -> 3510 in the leverage flash crash. One decision every 10 market
    # minutes, the tape at 120x, so the five hours replay in about 150 s of tape plus the paused thinking time.
    "crash": {"start": "2025-10-10T16:21Z", "end": "2025-10-10T21:21Z", "speed": 120.0, "round_minutes": 10.0,
              "label": "flash crash hours on {model} via {route}"},
}
REPLAY_PER_IP_S, REPLAY_GLOBAL_S = 600, 240


class ReplayLimiter:
    """One replay per `per_ip_s` per visitor and one per `global_s` for everyone. `allow` answers how
    long to wait (0 = go); `record` is called only once a replay actually started, so a refusal (busy,
    bad model) never spends a visitor's one slot."""

    def __init__(self, per_ip_s: int = REPLAY_PER_IP_S, global_s: int = REPLAY_GLOBAL_S, clock=time.time):
        self.per_ip_s, self.global_s, self.clock = per_ip_s, global_s, clock
        self._ip: dict[str, float] = {}
        self._last: float | None = None
        self._lock = threading.Lock()

    def reset(self) -> None:
        with self._lock:
            self._ip.clear()
            self._last = None

    def allow(self, ip: str) -> tuple[int, str | None]:
        now = self.clock()
        with self._lock:
            for k in [k for k, t in self._ip.items() if now - t > self.per_ip_s]:
                del self._ip[k]
            t = self._ip.get(ip)
            if t is not None and now - t < self.per_ip_s:
                return int(self.per_ip_s - (now - t)) + 1, "visitor"
            if self._last is not None and now - self._last < self.global_s:
                return int(self.global_s - (now - self._last)) + 1, "global"
        return 0, None

    def record(self, ip: str) -> None:
        now = self.clock()
        with self._lock:
            self._ip[ip] = now
            self._last = now


REPLAY_LIMITER = ReplayLimiter()


def install(app) -> None:
    """The live race on app.state.race."""
    _install(app, "race", "live")


def install_backtest(app) -> None:
    """The backtest race on app.state.backtest: its own venue pair; replays a window when asked
    (POST /backtest/start) and, unless BACKTEST_AUTOSTART=0, the most volatile day of the last 30 days at boot."""
    _install(app, "backtest", "backtest")


def _install(app, attr: str, mode: str) -> None:
    if getattr(app.state, attr, None) is not None or getattr(app.state, attr + "_error", None):
        return
    setattr(app.state, attr, None)
    setattr(app.state, attr + "_error", None)
    try:
        from .conductor import Conductor, load_config
        from .store import RaceStore
        from .world import World
        if mode == "live":
            world = World()
        else:
            from . import world as world_mod
            world = World(exchange_url=os.environ.get("BACKTEST_EXCHANGE_URL", "tcp://127.0.0.1:7040"),
                          venued_port=int(os.environ.get("BACKTEST_VENUED_PORT", "7050")),
                          db_path=os.environ.get("BACKTEST_DB", str(pathlib.Path(world_mod.DEFAULT_DB).with_name("race-backtest.db"))))
        store = RaceStore(world.db)
        cond = Conductor(world, store, load_config(mode), mode=mode)
        cond.start()
        setattr(app.state, attr, cond)
        log.info("%s race started: model %s via %s", mode, cond.model, cond.route)
        if mode == "backtest" and os.environ.get("BACKTEST_AUTOSTART", "1") != "0":
            def boot():
                try:
                    info = cond.start_backtest(speed=float(os.environ.get("BACKTEST_SPEED", "30")),
                                               round_minutes=float(os.environ.get("BACKTEST_ROUND_MINUTES", "10")))
                    log.info("backtest window started: %s", info.get("label"))
                except Exception:
                    log.exception("backtest autostart failed")
            threading.Thread(target=boot, name="backtest-autostart", daemon=True).start()
    except Exception as e:
        setattr(app.state, attr + "_error", f"{type(e).__name__}: {e}")
        log.warning("%s race not started: %s", mode, getattr(app.state, attr + "_error"))


def _visitor(request: Request) -> str:
    fwd = request.headers.get("x-forwarded-for", "")
    return fwd.split(",")[0].strip() if fwd else (request.client.host if request.client else "unknown")


class Message(BaseModel):
    text: str


class EpochRequest(BaseModel):
    token: str = ""
    reason: str = "restarted by the operator"


class StartBacktest(BaseModel):
    token: str = ""
    start: str | None = None
    end: str | None = None
    speed: float = 30.0
    round_minutes: float = 10.0
    label: str | None = None
    model: str | None = None


class Replay(BaseModel):
    scenario: str = "crash"
    model: str | None = None


def make_router(prefix: str, attr: str) -> APIRouter:
    router = APIRouter(prefix=prefix, tags=[attr])

    def _race(request: Request):
        r = getattr(request.app.state, attr, None)
        if r is None:
            return None, JSONResponse(status_code=503, content={"error": "race_offline", "detail": getattr(request.app.state, attr + "_error", None)
                                                                or f"the {attr} race is not running in this process"})
        return r, None

    @router.get("/state")
    def state(request: Request, minutes: int = 30):
        r, err = _race(request)
        if err:
            return err
        return r.state(prints_minutes=max(1, min(int(minutes), 240)))


    @router.get("/compare")
    def compare(request: Request, limit: int = 12):
        r, err = _race(request)
        if err:
            return err
        from .conductor import compare_rows
        return {"mode": r.mode, "capital_cents": r.cfg["capital_cents"], "envelope": r.cfg["envelope"], "rows": compare_rows(r, max(1, min(int(limit), 50)))}

    @router.get("/status")
    def status(request: Request):
        r, err = _race(request)
        if err:
            return err
        s = r.state(prints_minutes=1)
        return {"epoch": s["epoch"], "route": s["route"], "model": s["model"], "feed": s["feed"], "tape": {k: v for k, v in s["tape"].items() if k != "recent"},
                "raw": {k: s["raw"].get(k) for k in ("equity_cents", "pnl_cents", "position", "liquidated", "trades", "fees_cents")},
                "bounded": {k: s["bounded"].get(k) for k in ("equity_cents", "pnl_cents", "position", "trades", "fees_cents", "certified", "rejected")}}


    @router.get("/rounds")
    def rounds(request: Request, limit: int = 40, epoch: int | None = None, full: int = 0):
        """Rounds of the current epoch, or of `epoch`, oldest first; `full=1` keeps every lane's prompts and replies."""
        r, err = _race(request)
        if err:
            return err
        ep = int(epoch) if epoch is not None else r.epoch
        if ep is None:
            return {"epoch": None, "rounds": []}
        return {"epoch": ep, "rounds": r.store.rounds(ep, max(1, min(int(limit), 500)), full=bool(int(full)))}


    @router.get("/rounds/{round_id}")
    def round_(request: Request, round_id: int):
        r, err = _race(request)
        if err:
            return err
        d = r.store.round(round_id)
        if d is None:
            return JSONResponse(status_code=404, content={"error": "unknown_round"})
        return d


    @router.get("/tape")
    def tape(request: Request, minutes: int = 60):
        r, err = _race(request)
        if err:
            return err
        since = int(time.time() * 1000) - max(1, min(int(minutes), 1440)) * 60_000
        return {"epoch": r.epoch, "feed": r.feed.status(), "prints": r.store.prints(r.epoch, limit=20000, since_t=since),
                "columns": ["t_ms", "price", "ticks", "gap"]}


    @router.get("/fills")
    def fills(request: Request, limit: int = 400):
        r, err = _race(request)
        if err:
            return err
        return {"epoch": r.epoch, "fills": r.store.fills(r.epoch, max(1, min(int(limit), 5000)))}


    @router.get("/stream")
    def stream(request: Request):
        r, err = _race(request)
        if err:
            return err
        header = request.headers.get("last-event-id")
        since = request.query_params.get("since") or header
        try:
            last = int(since) if since is not None else r.store.last_event_id()
        except ValueError:
            last = r.store.last_event_id()
        once = request.query_params.get("once") == "1"

        def gen():
            nonlocal last
            idle = 0.0
            while True:
                rows = r.store.events_after(last)
                if rows:
                    idle = 0.0
                    for ev in rows:
                        last = ev["id"]
                        yield f"id: {last}\nevent: {ev['type']}\ndata: {ev['data']}\n\n"
                    continue
                if once:
                    return
                time.sleep(0.25)
                idle += 0.25
                if idle >= 15:
                    idle = 0.0
                    yield ": keepalive\n\n"

        return StreamingResponse(gen(), media_type="text/event-stream", headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"})



    @router.post("/message")
    def message(body: Message, request: Request):
        r, err = _race(request)
        if err:
            return err
        text = " ".join(body.text.split())
        if not text:
            return JSONResponse(status_code=400, content={"error": "empty_message"})
        if len(text) > 400:
            return JSONResponse(status_code=400, content={"error": "too_long", "detail": "400 characters at most"})
        v = _visitor(request)
        now = time.time()
        with _vlock:
            hist = _visitors.setdefault(v, deque())
            while hist and now - hist[0] > RATE_LIMIT_WINDOW_S:
                hist.popleft()
            if len(hist) >= RATE_LIMIT_N:
                return JSONResponse(status_code=429, content={"error": "rate_limited", "detail": f"{RATE_LIMIT_N} messages per {RATE_LIMIT_WINDOW_S // 60} minutes per visitor",
                                                              "retry_after_s": int(RATE_LIMIT_WINDOW_S - (now - hist[0]))})
            hist.append(now)
        try:
            return r.message(text, "visitor")
        except ValueError as e:
            return JSONResponse(status_code=400, content={"error": "bad_message", "detail": str(e)})



    @router.post("/epoch")
    def new_epoch(body: EpochRequest, request: Request):
        r, err = _race(request)
        if err:
            return err
        token = os.environ.get("RACE_ADMIN_TOKEN", "")
        if not token or body.token != token:
            return JSONResponse(status_code=403, content={"error": "forbidden"})
        r.new_epoch(body.reason[:120])
        return {"epoch": r.epoch}


    @router.post("/start")
    def start_backtest(body: StartBacktest, request: Request):
        r, err = _race(request)
        if err:
            return err
        if getattr(r, "mode", "live") != "backtest":
            return JSONResponse(status_code=404, content={"error": "not_a_backtest"})
        token = os.environ.get("RACE_ADMIN_TOKEN", "")
        if not token or body.token != token:
            return JSONResponse(status_code=403, content={"error": "forbidden", "detail": "RACE_ADMIN_TOKEN required"})
        try:
            return r.start_backtest(body.start, body.end, body.speed, body.round_minutes, body.label, model=body.model)
        except Exception as e:
            return JSONResponse(status_code=400, content={"error": "bad_window", "detail": str(e)})

    @router.post("/replay")
    def replay(body: Replay, request: Request):
        """Public: replay one named scenario on an allow-listed model. Refused 409 while a window is in
        progress; one per 10 minutes per visitor and one per 4 minutes for everyone."""
        r, err = _race(request)
        if err:
            return err
        if getattr(r, "mode", "live") != "backtest":
            return JSONResponse(status_code=404, content={"error": "not_a_backtest"})
        sc = REPLAY_SCENARIOS.get((body.scenario or "").strip().lower())
        if sc is None:
            return JSONResponse(status_code=400, content={"error": "unknown_scenario", "detail": f"scenarios: {sorted(REPLAY_SCENARIOS)}"})
        model = (body.model or "").strip() or REPLAY_DEFAULT_MODEL
        if model not in REPLAY_MODELS:
            return JSONResponse(status_code=400, content={"error": "unknown_model", "detail": f"models: {REPLAY_MODELS}"})
        if getattr(r, "backtest", None) is not None and getattr(r, "finished", None) is None:
            return JSONResponse(status_code=409, content={"detail": "busy", "error": "busy", "epoch": getattr(r, "epoch", None),
                                                          "label": (r.backtest or {}).get("label")})
        ip = _visitor(request)
        wait, who = REPLAY_LIMITER.allow(ip)
        if wait:
            return JSONResponse(status_code=429, content={"error": "rate_limited", "detail": ("one replay per 10 minutes per visitor" if who == "visitor"
                                                                                               else "one replay per 4 minutes for everyone"),
                                                          "retry_after_s": wait}, headers={"Retry-After": str(wait)})
        try:
            info = r.start_backtest(sc["start"], sc["end"], sc["speed"], sc["round_minutes"], sc["label"], model=model)
        except Exception as e:
            return JSONResponse(status_code=400, content={"error": "replay_failed", "detail": str(e)})
        REPLAY_LIMITER.record(ip)
        return {"ok": True, "epoch": info.get("epoch"), "model": info.get("model") or model, "route": info.get("route") or getattr(r, "route", None),
                "eta_s": info.get("eta_s"), "label": info.get("label"), "scenario": body.scenario}

    @router.get("/windows")
    def windows(request: Request, days: int = 30):
        from .backtest import pick_volatile_day
        try:
            return {"most_volatile": pick_volatile_day(max(2, min(int(days), 90)))}
        except Exception as e:
            return JSONResponse(status_code=502, content={"error": "coinbase", "detail": str(e)})

    return router


router = make_router("/race", "race")
backtest_router = make_router("/backtest", "backtest")
