"""B1: the API server. Every route in BOUNDED_KEYS.md §10 and no others, plus:

  POST /program/{id}/repair   the repair loop (B9: "the gateway resubmits"), author key
  /photon/*                   the phone's inbound webhook (B7), not part of the public API
  GET  /healthz

There is no /order route at all: FastAPI's own 404 is the refusal.
"""

import logging
import os
import threading
from contextlib import asynccontextmanager

from fastapi import FastAPI, Header, Request
from fastapi.responses import JSONResponse, StreamingResponse
from pydantic import BaseModel

from . import config, keys
from .db import Database
from .errors import ApiError
from .events import EventBus, sse_stream
from .nessie import make_bank
from .photon import Photon
from .repair import repair as run_repair
from .service import Gateway
from .venue import make_venue

logging.basicConfig(level=os.environ.get("LOG_LEVEL", "INFO"),
                    format="%(asctime)s %(levelname)s %(name)s: %(message)s")
log = logging.getLogger("gateway.app")
for _n in ("httpx", "httpx2", "httpcore"):          # these log full URLs, and Nessie's URL carries the key
    logging.getLogger(_n).setLevel(logging.WARNING)


def build(db: Database | None = None, venue=None, bank=None, start_bus: bool = True, lab: bool = True, race: bool = True) -> FastAPI:
    db = db or Database()
    venue = venue or make_venue()
    bank = bank or make_bank(db)
    gw = Gateway(db, venue, bank)
    photon = Photon(gw)
    bus = EventBus(gw)

    @asynccontextmanager
    async def lifespan(app: FastAPI):
        if start_bus:
            bus.start()
        if photon.beta:
            info = photon.setup(os.environ.get("LAB_PUBLIC_URL"), os.environ.get("PHOTON_OWNER_NUMBER"))
            log.info("photon beta setup: %s", info)
        log.info("gateway up: venue=%s bank=%s db=%s", venue.name, bank.name, db.path)
        yield
        bus.stop()
        if hasattr(venue, "close"):
            venue.close()

    app = FastAPI(title="Bounded keys gateway", version="0.1", lifespan=lifespan,
                  description="An API key whose permission is a theorem. POST /program is the one new method.")
    app.state.gw, app.state.photon, app.state.bus, app.state.db, app.state.venue, app.state.bank = gw, photon, bus, db, venue, bank

    @app.exception_handler(ApiError)
    async def _api_error(_: Request, e: ApiError):
        return JSONResponse(status_code=e.status, content=e.body())

    def auth(authorization: str | None) -> keys.KeyInfo:
        return keys.authenticate(db, authorization)

    # ---------------------------------------------------------------- owner (§10.1)
    class CreateAccount(BaseModel):
        opening_balance_cents: int = 100_000_00
        owner_phone: str | None = None

    @app.post("/account")
    def create_account(body: CreateAccount | None = None):
        body = body or CreateAccount()
        return gw.create_account(body.opening_balance_cents, body.owner_phone)

    @app.put("/account/{a}/envelope")
    def put_envelope(a: int, envelope: dict, authorization: str | None = Header(default=None)):
        k = auth(authorization)
        keys.require_owner(k, a)
        return gw.set_envelope(a, envelope)

    @app.post("/account/{a}/author-key")
    def author_key(a: int, authorization: str | None = Header(default=None)):
        k = auth(authorization)
        keys.require_owner(k, a)
        return gw.issue_author_key(a)

    @app.post("/account/{a}/freeze")
    def freeze(a: int, authorization: str | None = Header(default=None)):
        k = auth(authorization)
        keys.require_owner(k, a)
        return gw.freeze(a)

    @app.get("/account/{a}")
    def get_account(a: int, authorization: str | None = Header(default=None)):
        k = auth(authorization)
        keys.require_owner(k, a)
        return gw.account_view(a)

    @app.get("/account/{a}/events")
    def events(a: int, request: Request, authorization: str | None = Header(default=None),
               last_event_id: str | None = Header(default=None)):
        k = auth(authorization)
        keys.require_owner(k, a)
        since = int(request.query_params.get("since", last_event_id or 0) or 0)
        stop = threading.Event()
        once = request.query_params.get("once") == "1"
        return StreamingResponse(sse_stream(db, a, since, stop=stop, once=once), media_type="text/event-stream",
                                 headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"})

    # ---------------------------------------------------------------- author (§10.2)
    class Submit(BaseModel):
        account: int
        backend: str = "minsky"
        source: str

    @app.post("/program")
    def submit(body: Submit, authorization: str | None = Header(default=None)):
        k = auth(authorization)
        status, out = gw.submit_program(k, body.account, body.backend, body.source)
        return JSONResponse(status_code=status, content=out)

    @app.get("/program/{pid}")
    def get_program(pid: int, authorization: str | None = Header(default=None)):
        return gw.get_program(auth(authorization), pid)

    @app.get("/program/{pid}/counterexample")
    def counterexample(pid: int, authorization: str | None = Header(default=None)):
        return gw.get_counterexample(auth(authorization), pid)

    @app.post("/program/{pid}/repair")
    def repair_program(pid: int, authorization: str | None = Header(default=None)):
        status, out = run_repair(gw, auth(authorization), pid)
        return JSONResponse(status_code=status, content=out)

    # ---------------------------------------------------------------- program key (§10.3)
    @app.post("/program/{pid}/deploy")
    def deploy(pid: int, authorization: str | None = Header(default=None)):
        return gw.deploy(auth(authorization), pid)

    @app.post("/program/{pid}/pause")
    def pause(pid: int, authorization: str | None = Header(default=None)):
        return gw.pause(auth(authorization), pid)

    @app.post("/program/{pid}/cancel")
    def cancel(pid: int, authorization: str | None = Header(default=None)):
        return gw.cancel(auth(authorization), pid)

    @app.get("/program/{pid}/state")
    def state(pid: int, authorization: str | None = Header(default=None)):
        return gw.state(auth(authorization), pid)

    # ---------------------------------------------------------------- phone webhook (B7)
    class Inbound(BaseModel):
        from_number: str
        body: str = ""
        transcript: str | None = None
        session_id: str | None = None

    @app.post("/photon/text")
    def photon_text(msg: Inbound):
        return photon.handle_text(msg.from_number, msg.body)

    @app.post("/photon/voice")
    def photon_voice(msg: Inbound):
        return photon.handle_voice(msg.from_number, msg.transcript or msg.body, msg.session_id or msg.from_number)

    @app.post("/photon/webhook")
    async def photon_webhook(request: Request):
        raw = await request.body()
        return photon.handle_webhook(raw, dict(request.headers))

    @app.get("/photon/line")
    def photon_line():
        return {"project": photon.project_id, "beta": bool(photon.beta), "line": photon.line,
                "owner_number_set": bool(os.environ.get("PHOTON_OWNER_NUMBER"))}

    @app.get("/photon/outbox")
    def photon_outbox():
        return list(photon.outbox)

    @app.get("/healthz")
    def healthz():
        return {"ok": True, "venue": venue.name, "bank": bank.name, "llm": _llm_route()}

    # ---------------------------------------------------------------- the race (race/README.md): its own venue, both lanes, the stream
    if race and os.environ.get("GATEWAY_LAZY") != "1" and os.environ.get("RACE", "1") != "0":
        try:
            from race.app import backtest_router, install as install_race, install_backtest, router as race_router
            app.include_router(race_router)
            install_race(app)                           # degrades to 503 /race/* when the race venue is not reachable
            if os.environ.get("BACKTEST", "1") != "0":
                app.include_router(backtest_router)
                install_backtest(app)                   # the same race on a day of history, on its own venue pair
        except Exception:                               # pragma: no cover
            log.exception("race not mounted")

    # ---------------------------------------------------------------- the lab and the page (lab/CONTRACT.md §3)
    if lab:
        try:
            from lab.app import install as install_lab, router as lab_router, site_files
            app.include_router(lab_router)
            if os.environ.get("GATEWAY_LAZY") != "1":
                install_lab(app, schedule=True)             # a served gateway runs the scheduler; tests install lazily
            site = site_files(config.ROOT / "site")
            if site is not None:
                app.mount("/", site, name="site")           # last, so every API route wins; site/ may not exist yet
        except Exception:                                   # pragma: no cover
            log.exception("lab not mounted")

    return app


def _llm_route() -> str:
    from chalk import ai_gateway
    try:
        return ai_gateway.get_llm().route
    except Exception as e:   # pragma: no cover
        return f"error: {e}"


app = build() if os.environ.get("GATEWAY_LAZY") != "1" else None
