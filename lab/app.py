"""The lab's routes (lab/CONTRACT.md §3), mounted into the gateway app by `gateway/app.py`:

    from lab.app import router as lab_router, install as install_lab
    app.include_router(lab_router); install_lab(app, schedule=...)

`install` creates `app.state.lab`: the store (in the gateway's SQLite), the runner (one worker
thread), the tape loader and the sponsor probe. The routes read it through `request.app`.
"""
from __future__ import annotations

import inspect
import json
import logging
import os
import shutil
import sqlite3
import threading
import time
from pathlib import Path

from fastapi import APIRouter, Request
from fastapi.responses import JSONResponse, StreamingResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

from gateway import config

from . import tape as tape_mod
from . import tests as registry
from .context import FIXTURES, Context, llm_route
from .runner import LabError, Runner
from .store import LabStore

log = logging.getLogger("lab.app")

router = APIRouter(prefix="/lab", tags=["lab"])
ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MODELS = os.environ.get("LAB_MODELS", "claude-opus-5-5")
TAPE_MAX_AGE_S = 300


# ----------------------------------------------------------------------------- the LLM route
class _Pinned:
    """The configured route with every call pinned to one model name (as evals/run.py does)."""

    def __init__(self, inner, model: str):
        self.inner, self.model, self.route = inner, model, inner.route

    def complete(self, system, user, model=None, max_tokens=4000):
        return self.inner.complete(system, user, model=self.model, max_tokens=max_tokens)


def select_llm(model: str):
    """`chalk.ai_gateway.get_llm()`, passing the model when the gateway accepts one (the Chalk
    Model Gateway form), else pinned here. Returns (llm, route)."""
    from chalk import ai_gateway
    if model == "stub":
        return ai_gateway.StubLLM(), "stub"
    try:
        params = inspect.signature(ai_gateway.get_llm).parameters
    except (TypeError, ValueError):
        params = {}
    try:
        llm = ai_gateway.get_llm(model) if params else ai_gateway.get_llm()
    except Exception as e:
        log.warning("get_llm failed (%s); stub route", e)
        return ai_gateway.StubLLM(), "stub"
    route = getattr(llm, "route", "stub") or "stub"
    route_for = getattr(llm, "route_for", None)
    if callable(route_for):
        # RoutedLLM sends grok-* through Chalk and claude-* to the Anthropic API directly; the run's
        # route must name the one THIS model takes, not the router's primary.
        try:
            route = route_for(model) or route
        except Exception as e:
            log.warning("route_for(%s) failed (%s); reporting the router's primary route", model, e)
    if route != "stub" and not params:
        llm = _Pinned(llm, model)
    return llm, route


# ----------------------------------------------------------------------------- the sponsor probe
def local_probe(app) -> dict:
    """Which services are live, from the gateway's own configuration. The fallback when
    `lab.services.probe_all` is not importable."""
    venue = app.state.venue
    bank = app.state.bank
    route = llm_route()
    daytona = bool(os.environ.get("DAYTONA_API_KEY"))
    photon = bool((config.PHOTON_API_URL and config.PHOTON_API_KEY) or
                  (os.environ.get("PHOTON_PROJECT_ID") and os.environ.get("PHOTON_PROJECT_SECRET")))
    factory_key = bool(os.environ.get("FACTORY_API_KEY"))
    droid_cli = shutil.which(config.DROID_BIN) is not None
    return {
        "venue": {"name": "venue", "value": venue.name, "live": venue.name == "real",
                  "reason": (f"Build A's policyc/verify/replay + venued at {config.VENUED_HOST}:{config.VENUED_PORT}"
                             if venue.name == "real" else "fake venue (VENUE=fake): a Python model, not the verifier")},
        "llm": {"name": "model route", "value": route, "live": route not in ("stub",) and not route.startswith("error"),
                "reason": {"chalk": "Chalk Model Gateway", "stub": "no model credentials: the rule-based stub answers",
                           "anthropic-direct": "Anthropic API directly (Chalk bypassed; development only)"}.get(route, route)},
        "bank": {"name": "bank", "value": bank.name, "live": bank.name == "nessie",
                 "reason": "Capital One Nessie (a simulated bank: the flow is real, the money is not)" if bank.name == "nessie"
                 else "local ledger simulator (NESSIE_API_KEY unset)"},
        "sandbox": {"name": "sandbox", "value": "daytona" if daytona else "local", "live": daytona,
                    "reason": "Daytona sandboxes with the two-host egress policy" if daytona
                    else "local subprocess (DAYTONA_API_KEY unset): egress not enforced"},
        "phone": {"name": "phone", "value": "photon" if photon else "log", "live": photon,
                  "reason": "Photon line: alarms out, freeze/status in" if photon else "alarms logged to the outbox (no Photon credentials)"},
        "droid": {"name": "droid", "value": "factory-sdk" if factory_key else ("factory-cli" if droid_cli else ("llm" if route != "stub" else "stub")),
                  "live": factory_key or droid_cli,
                  "reason": "Factory Droid repairs rejected programs" if (factory_key or droid_cli)
                  else ("the model behind the LLM route repairs" if route != "stub" else "rule-based stub repairer")},
    }


def probe_services(app) -> dict:
    try:
        from lab import services as svc            # written by another agent; optional
        fn = getattr(svc, "probe_all")
        try:
            out = fn()
        except TypeError:
            out = fn(app)
        if isinstance(out, dict) and "services" in out:
            out = out["services"]
        norm = {}
        for k, v in (out or {}).items():
            if isinstance(v, dict):
                d = dict(v)
                d.setdefault("name", k)
                d.setdefault("value", d.get("mode") or d.get("route") or d.get("backend") or ("live" if d.get("live") else "off"))
                d.setdefault("live", False)
                d.setdefault("reason", d.get("detail", ""))
                norm[k] = d
            else:
                norm[k] = {"name": k, "value": str(v), "live": bool(v), "reason": ""}
        for k, v in local_probe(app).items():
            norm.setdefault(k, v)
        # The venue entry must describe the venue THIS process uses, not what the environment would pick.
        vn = getattr(getattr(app, "state", None), "venue", None)
        if vn is not None and "venue" in norm:
            name = getattr(vn, "name", "fake")
            norm["venue"]["value"] = norm["venue"]["mode"] = name
            if name != "real":
                norm["venue"]["live"] = False
                norm["venue"]["reason"] = "fake venue in this process (fixtures + Python model); set VENUE=real"
        return norm
    except Exception as e:                      # ImportError or a probe that raised: the local probe answers
        if not isinstance(e, ImportError):
            log.warning("lab.services.probe_all failed (%s); local probe", e)
        return local_probe(app)


# ----------------------------------------------------------------------------- the lab object
class Lab:
    def __init__(self, app, models: list[str] | None = None, schedule: bool = False, **runner_kw):
        self.app = app
        self.models = models or [m.strip() for m in DEFAULT_MODELS.split(",") if m.strip()]
        self.store = LabStore(app.state.db)
        self.runner = Runner(self.store, self.make_context, self.models, **runner_kw)
        self._tape: tape_mod.Tape | None = None
        self._tape_t = 0.0
        self._status: dict | None = None
        self._lock = threading.Lock()
        self.runner.start(schedule=schedule)

    def tape(self, refresh: bool = False) -> tape_mod.Tape | None:
        with self._lock:
            if self._tape is None or refresh or time.time() - self._tape_t > TAPE_MAX_AGE_S:
                try:
                    tick = int(self.app.state.venue.instruments().get("ETH", {}).get("tick_cents", 100))
                    self._tape = tape_mod.load_latest(tick_cents=tick)
                    self._tape_t = time.time()
                except Exception as e:
                    log.warning("tape unavailable: %s", e)
            return self._tape

    def status(self) -> dict:
        services = probe_services(self.app)
        body = {"services": services, "t": int(time.time() * 1000)}
        with self._lock:
            key = {k: (v.get("value"), v.get("live")) for k, v in services.items()}
            changed = self._status is not None and self._status != key
            self._status = key
        if changed:
            self.store.add_event(None, "status.changed", body)
        return body

    def make_context(self, run: dict) -> Context:
        test_id, model = run["test"], run["model"]
        if registry.SPECS[test_id]["model_test"]:
            llm, route = select_llm(model)
        else:
            llm, route = None, "none"
        tape = self.tape(refresh=test_id in ("tape", "race")) if test_id in ("tape", "race") else self._tape
        services = self.status()["services"]
        return Context(self.app, run["run_id"], test_id, model, llm, route, tape, services, self.store, run.get("params"))

    def default_model(self) -> str:
        return self.models[0] if self.models else "claude-opus-5-5"


def install(app, schedule: bool | None = None, **kw) -> Lab:
    """Idempotent. The scheduler runs only in a served gateway (`schedule=True`), never in the
    in-process builds the test suites make (GATEWAY_LAZY=1)."""
    lab = getattr(app.state, "lab", None)
    if lab is None:
        if schedule is None:
            schedule = os.environ.get("GATEWAY_LAZY") != "1"
        lab = Lab(app, schedule=schedule, **kw)
        app.state.lab = lab
    return lab


def _lab(request: Request) -> Lab:
    return install(request.app)


def _visitor(request: Request) -> str:
    fwd = request.headers.get("x-forwarded-for", "")
    if fwd:
        return fwd.split(",")[0].strip()
    return request.client.host if request.client else "unknown"


# ----------------------------------------------------------------------------- routes
@router.get("/catalogue")
def catalogue(request: Request):
    lab = _lab(request)
    latest: dict[str, dict] = {}
    for (test, model), run in lab.store.latest_per_pair().items():          # newest FINISHED run (C6)
        latest.setdefault(test, {})[model] = run
    pending: dict[str, dict] = {}
    for (test, model), p in lab.store.pending_per_pair().items():          # a queued/running run, separately
        p = dict(p)
        p["position"] = lab.runner.position_of(p["run_id"])
        pending.setdefault(test, {})[model] = p
    env = json.loads((FIXTURES / "envelope_40.json").read_text())
    return {"tests": registry.catalogue(), "models": lab.models, "default_model": lab.default_model(),
            "route": llm_route(), "venue": request.app.state.venue.name, "latest": latest, "pending": pending,
            "envelope": {k: env[k] for k in ("max_loss_cents", "max_fees_cents", "max_trades", "max_exposure_lots", "window", "env")},
            "schedule_min": lab.runner.schedule_min, "queue": lab.runner.position()}


@router.get("/runs")
def runs(request: Request, test: str | None = None, model: str | None = None, limit: int = 50):
    lab = _lab(request)
    return {"runs": lab.store.list_runs(test, model, max(1, min(int(limit), 500)))}


@router.get("/runs/{run_id}")
def run(request: Request, run_id: str):
    r = _lab(request).store.get_run(run_id)
    if r is None:
        return JSONResponse(status_code=404, content={"error": "unknown_run", "run_id": run_id})
    return r


class RunRequest(BaseModel):
    test: str
    model: str | None = None
    params: dict | None = None


@router.post("/run")
def post_run(body: RunRequest, request: Request):
    lab = _lab(request)
    try:
        run_, position = lab.runner.submit(body.test, body.model or lab.default_model(), body.params, _visitor(request))
    except LabError as e:
        return JSONResponse(status_code=e.status, content=e.body())
    return {"run_id": run_["run_id"], "position": position, "test": run_["test"], "model": run_["model"]}


@router.get("/stream")
def stream(request: Request, last_event_id: str | None = None):
    lab = _lab(request)
    header = request.headers.get("last-event-id")
    since = request.query_params.get("since") or header or last_event_id or "0"
    try:
        since_id = int(since)
    except ValueError:
        since_id = 0
    once = request.query_params.get("once") == "1"

    def gen():
        last = since_id
        idle = 0.0
        while True:
            rows = lab.store.events_after(last)
            if rows:
                idle = 0.0
                for ev in rows:
                    last = ev["id"]
                    yield f"id: {last}\nevent: {ev['type']}\ndata: {ev['data']}\n\n"
                continue
            if once:
                return
            time.sleep(0.3)
            idle += 0.3
            if idle >= 15:
                idle = 0.0
                yield ": keepalive\n\n"

    return StreamingResponse(gen(), media_type="text/event-stream",
                             headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"})


LEADERBOARD_COLUMNS = [
    {"key": "first_pass", "label": "first pass", "unit": "%"},
    {"key": "droid_rounds", "label": "droid rounds", "unit": ""},
    {"key": "tightness", "label": "tightness", "unit": ""},
    {"key": "turned", "label": "turned", "unit": "%"},
    {"key": "breaches", "label": "breaches", "unit": ""},
    {"key": "race_loss_cents", "label": "race loss (bounded)", "unit": "cents"},
    {"key": "race_raw_loss_cents", "label": "race loss (raw)", "unit": "cents"},
]


@router.get("/leaderboard")
def leaderboard(request: Request):
    """Per model, from the latest passed (or stub-completed) run of each model test (CONTRACT C4).
    A stub row (the rule-based stand-in answered, so the numbers say nothing about a model) is
    listed only while no real model has a row; it is always flagged `stub: true`."""
    lab = _lab(request)
    latest = lab.store.latest_per_pair(statuses=("passed", "completed"))
    models = list(dict.fromkeys(lab.models + [m for (_, m) in latest.keys()]))
    rows = []
    for m in models:
        fp = latest.get(("firstpass", m))
        inj = latest.get(("injection", m))
        race = latest.get(("race", m))
        if not (fp or inj or race):
            continue
        route = next((r["route"] for r in (fp, inj, race) if r and r.get("route")), None)
        routes = sorted({r["route"] for r in (fp, inj, race) if r and r.get("route")})
        stub = any(r.get("stub") or r.get("route") == "stub" for r in (fp, inj, race) if r)
        fm, im, rm = (fp or {}).get("metrics", {}), (inj or {}).get("metrics", {}), (race or {}).get("metrics", {})
        by_class = im.get("by_class") if isinstance(im.get("by_class"), dict) else {}
        rows.append({
            "model": m, "route": route, "routes": routes, "stub": stub,
            "first_pass": _pct(fm.get("certification_rate")),
            "droid_rounds": fm.get("droid_rounds_mean"),
            "droid_rounds_median": fm.get("droid_rounds_median"),
            "tightness": fm.get("bound_tightness_mean"),
            "tightness_min": fm.get("bound_tightness_min"),
            "n_briefs": fm.get("n_briefs") if fm.get("n_briefs") is not None else im.get("n_briefs"),
            "n_pairs": im.get("n_pairs"),
            "turned": _pct(im.get("turned_rate")),
            "complied_silently": _pct(im.get("complied_silently")),
            "ignored": _pct(im.get("ignored")),
            "by_class": {k: {kk: v.get(kk) for kk in ("n", "turned", "complied_silently", "ignored", "static_error", "other") if kk in v}
                         for k, v in by_class.items() if isinstance(v, dict)},
            "breaches": im.get("breaches"),
            "loss_beyond_envelope_cents": im.get("loss_beyond_envelope_cents"),
            "race_loss_cents": rm.get("bounded_loss_cents"),
            "race_raw_loss_cents": rm.get("raw_loss_cents"),
            "race_raw_simulated": rm.get("raw_simulated"),
            "runs": {"firstpass": fp and fp["run_id"], "injection": inj and inj["run_id"], "race": race and race["run_id"]},
        })
    real = [r for r in rows if not r["stub"]]
    excluded = [r["model"] for r in rows if r["stub"]] if real else []
    if real:
        rows = real
    return {"columns": LEADERBOARD_COLUMNS, "rows": rows, "stub_rows_excluded": excluded}


def _pct(x):
    return None if x is None else round(100.0 * float(x), 1)


CERT_SOURCES = ("gateway/data/gateway.db", "gateway/data/race.db", "gateway/data/race-backtest.db")
ENDED = ("halted", "cancelled")
CERTIFIED_EVER = ("certified", "deployed", "paused", "halted", "cancelled")


def certificates_summary(sources=CERT_SOURCES, since_ms: int | None = None, root: Path = ROOT) -> dict:
    """CONTRACT C5: every certificate this project issued today against what the venue then
    realized, read-only across the lab's, the live race's and the backtest's SQLite files. The
    arithmetic is race/store.py's: a program ENDED when its status is halted or cancelled; a
    certificate BREACH is realized_loss > bound_loss on an ended program. Nothing is guessed:
    a number that cannot be computed from a file is explained in `notes` instead."""
    notes: list[str] = []
    env = json.loads((FIXTURES / "envelope_40.json").read_text())
    default_gap = int(env["env"]["max_gap_ticks"])
    tot = {"programs_certified": 0, "programs_ended": 0, "certificate_breaches": 0, "sum_realized_cents": 0,
           "sum_certified_cents": 0, "assumption_violations": 0, "programs_in_profit": 0, "sum_fees_cents": 0}
    worst: float | None = None
    worst_pair = None
    first_t = last_t = None
    by_source = []
    used: list[str] = []
    for rel in sources:
        path = (root / rel) if not Path(rel).is_absolute() else Path(rel)
        if not path.is_file():
            notes.append(f"{rel}: file not present; skipped")
            continue
        row = {"source": rel, "programs_certified": 0, "programs_ended": 0, "certificate_breaches": 0,
               "sum_realized_cents": 0, "sum_certified_cents": 0, "assumption_violations": None, "worst_realized_over_certified": None}
        try:
            c = sqlite3.connect(f"file:{path}?mode=ro", uri=True, timeout=5)
            c.row_factory = sqlite3.Row
        except sqlite3.Error as e:
            notes.append(f"{rel}: cannot open read-only ({e})")
            continue
        try:
            tables = {r[0] for r in c.execute("SELECT name FROM sqlite_master WHERE type='table'")}
            if "programs" not in tables:
                notes.append(f"{rel}: no programs table")
                continue
            where, args = "", []
            if since_ms is not None:
                where, args = " AND created_t>=?", [int(since_ms)]
            ph = ",".join("?" * len(CERTIFIED_EVER))
            row["programs_certified"] = int(c.execute(f"SELECT COUNT(*) FROM programs WHERE status IN ({ph}){where}",
                                                      tuple(CERTIFIED_EVER) + tuple(args)).fetchone()[0])
            ph2 = ",".join("?" * len(ENDED))
            ended = [dict(r) for r in c.execute(
                f"SELECT id, status, bound_loss, realized_loss, realized_fees, created_t, ended_t FROM programs "
                f"WHERE status IN ({ph2}) AND realized_loss IS NOT NULL{where}", tuple(ENDED) + tuple(args))]
            row["programs_ended"] = len(ended)
            nobound = [p for p in ended if p["bound_loss"] is None]
            if nobound:
                notes.append(f"{rel}: {len(nobound)} ended program(s) carry no certificate bound; excluded from the breach count")
            scored = [p for p in ended if p["bound_loss"] is not None]
            breaches = [p for p in scored if int(p["realized_loss"] or 0) > int(p["bound_loss"])]
            row["certificate_breaches"] = len(breaches)
            row["sum_realized_cents"] = sum(int(p["realized_loss"] or 0) for p in scored)
            row["sum_certified_cents"] = sum(int(p["bound_loss"] or 0) for p in scored)
            row["sum_fees_cents"] = sum(int(p["realized_fees"] or 0) for p in ended)
            ratios = [(int(p["realized_loss"] or 0) / int(p["bound_loss"]), p) for p in scored if int(p["bound_loss"]) > 0]
            if ratios:
                r_, p_ = max(ratios, key=lambda x: x[0])
                row["worst_realized_over_certified"] = round(r_, 4)
                row["worst_program"] = {"id": p_["id"], "realized_cents": int(p_["realized_loss"] or 0), "certified_cents": int(p_["bound_loss"])}
                if worst is None or r_ > worst:
                    worst, worst_pair = r_, {"source": rel, **row["worst_program"]}
            ts = [p["created_t"] for p in ended if p["created_t"]]
            if ts:
                first_t = min(ts) if first_t is None else min(first_t, min(ts))
                last_t = max(ts) if last_t is None else max(last_t, max(ts))
            # Assumption violations: a print whose gap exceeds the certificate's max_gap_ticks. Only the
            # race databases keep a print log (race_prints); the lab's gateway.db does not.
            if "race_prints" in tables:
                gap = default_gap
                if "race_epochs" in tables:
                    for e in c.execute("SELECT config_json FROM race_epochs ORDER BY id DESC LIMIT 1"):
                        try:
                            gap = int(((json.loads(e[0]).get("envelope") or {}).get("env") or {}).get("max_gap_ticks") or default_gap)
                        except (ValueError, TypeError, AttributeError):
                            pass
                psql, pargs = "SELECT COUNT(*) FROM race_prints WHERE gap>?", [gap]
                if since_ms is not None and "race_epochs" in tables:
                    # a backtest's prints carry the historical market time; the window is the epoch's own start
                    psql += " AND epoch IN (SELECT id FROM race_epochs WHERE started_t>=?)"
                    pargs.append(int(since_ms))
                row["assumption_violations"] = int(c.execute(psql, tuple(pargs)).fetchone()[0])
                row["max_gap_ticks"] = gap
                tot["assumption_violations"] += row["assumption_violations"]
            else:
                notes.append(f"{rel}: no print log (race_prints), so gaps beyond max_gap_ticks during its programs are not counted here; "
                             f"the lab's tape test reports them per run as metrics.assumption_violations")
            if "events" in tables:
                row["programs_in_profit"] = int(c.execute(
                    "SELECT COUNT(*) FROM events WHERE type='program.halted' AND json_extract(payload_json,'$.profit_cents')>0"
                    + (" AND t>=?" if since_ms is not None else ""), tuple(args)).fetchone()[0])
                tot["programs_in_profit"] += row["programs_in_profit"]
            for k in ("programs_certified", "programs_ended", "certificate_breaches", "sum_realized_cents", "sum_certified_cents", "sum_fees_cents"):
                tot[k] += int(row.get(k) or 0)
            used.append(rel)
            by_source.append(row)
        except sqlite3.Error as e:
            notes.append(f"{rel}: query failed ({e})")
        finally:
            c.close()
    if not any(r.get("assumption_violations") is not None for r in by_source):
        tot["assumption_violations"] = None if not by_source else 0
    return {
        "as_of": int(time.time() * 1000),
        **{k: tot[k] for k in ("programs_certified", "programs_ended", "certificate_breaches")},
        "worst_realized_over_certified": round(worst, 4) if worst is not None else None,
        "worst_program": worst_pair,
        "sum_realized_cents": tot["sum_realized_cents"], "sum_certified_cents": tot["sum_certified_cents"],
        "sum_fees_cents": tot["sum_fees_cents"], "programs_in_profit": tot["programs_in_profit"],
        "assumption_violations": tot["assumption_violations"],
        "since_ms": since_ms, "first_program_t": first_t, "last_program_t": last_t,
        "sources": used, "by_source": by_source, "notes": notes,
        "definitions": {"ended": "status halted or cancelled with a realized loss recorded",
                        "breach": "realized_loss_cents > certificate max_loss_cents on an ended program (race/store.py)",
                        "assumption_violation": "a print whose gap in ticks exceeds the certificate's max_gap_ticks (race_prints only)"},
    }


@router.get("/certificates")
def certificates(request: Request, since: int | None = None):
    """CONTRACT C5. `since` (ms since epoch) narrows to programs created from then on; default: every
    program in the three files (all of them are this hackathon's)."""
    return certificates_summary(since_ms=since)


@router.get("/status")
def status(request: Request):
    return _lab(request).status()


@router.get("/tape/latest")
def tape_latest(request: Request):
    lab = _lab(request)
    t = lab.tape()
    if t is None:
        return JSONResponse(status_code=503, content={"error": "no_tape", "detail": "Coinbase unreachable and nothing cached"})
    env = json.loads((FIXTURES / "envelope_40.json").read_text())
    gap = int(env["env"]["max_gap_ticks"])
    d = t.as_dict()
    d.update({"max_gap_seen": t.max_gap, "max_gap_ticks": gap, "assumption_violations": t.violations(gap),
              "path": t.path(), "mark": t.prints[0]})
    return d


# ----------------------------------------------------------------------------- the page
class SiteFiles(StaticFiles):
    """`site/` at "/". Mounted last so every API route wins; a non-GET on an unknown path (for
    example `POST /order`) stays a 404, the refusal BOUNDED_KEYS.md §10.4 promises, instead of
    StaticFiles' 405."""

    async def __call__(self, scope, receive, send):
        if scope["type"] == "http" and scope.get("method") not in ("GET", "HEAD"):
            await JSONResponse(status_code=404, content={"detail": "Not Found"})(scope, receive, send)
            return
        await super().__call__(scope, receive, send)


def site_files(directory: Path | str = ROOT / "site") -> SiteFiles | None:
    d = Path(directory)
    if not d.is_dir():
        return None
    return SiteFiles(directory=str(d), html=True)
