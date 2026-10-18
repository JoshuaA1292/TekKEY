"""B9: the repair loop. A rejected program's counterexample goes to the Factory Droid, which
returns patched *source only*; the gateway resubmits, at most 3 rounds.

Where the Droid runs: inside the gateway process, at runtime, when `POST /program/{id}/repair`
is called (or when the delegate hands off a 422). What it can touch: the MPL source text, and
nothing else. It receives the source, the envelope and the counterexample; it returns source
text; the gateway resubmits through the same `POST /program` flow with the same author key.
It cannot change the envelope, the key, the budget, or anything server-side, because nothing
else is passed in and nothing else is accepted back. The Droid session's working directory is an
empty temporary directory, so it cannot read this repository either: the three inputs are the
whole of what it sees.

Backends, in order, and the `backend` name the incident report carries:
  1. "factory-droid-sdk"        the Factory Python SDK `droid-sdk` (0.5.0): `droid_sdk.run(prompt, ...)`
                               with FACTORY_API_KEY. The SDK spawns the local `droid` executable
                               (DROID_BIN) and speaks JSON-RPC to it, so the binary must be present.
  2. "factory-droid-cli"        `droid exec -o json "<prompt>"`; the `result` field is the answer
  3. "droid-via-chalk:<model>"  the LLM behind the Chalk Model Gateway (or "droid-via-anthropic-direct:<model>")
  4. "droid-via-stub"           the rule-based stub in chalk/stub_policy.py

A backend that is configured but fails (bad key, timeout, non-zero exit) is logged and the next
one runs; the report names the one that actually produced the source.
"""
from __future__ import annotations

import asyncio
import json
import logging
import os
import shutil
import subprocess
import tempfile
import threading
import time

from chalk import ai_gateway

from . import config, keys
from .db import now_ms
from .errors import ApiError
from .service import Gateway

log = logging.getLogger("gateway.repair")

DROID_TIMEOUT_S = float(os.environ.get("DROID_TIMEOUT_S", "180"))
DROID_MODEL = os.environ.get("DROID_MODEL", "")                     # empty: Factory's default for the account
DROID_REASONING_EFFORT = os.environ.get("DROID_REASONING_EFFORT", "")

DROID_SYSTEM = """You are a repair droid for MPL trading programs. You receive the program source, the
owner's envelope and the verifier's counterexample: the exact sequence of prints and engine events on which the
program broke a limit. Return ONLY the corrected MPL source in a ```mpl fenced block. Change as little as
possible. Rules you must respect: every `exit ... at` needs `allow >= max_gap_ticks` or the stop-limit will rest
unfilled in a gap; the worst loss on a long is (stop distance - 1 + max_gap_ticks) * qty * tick plus fees and must
stay under max_loss_cents; you may tighten stops, lower quantities, remove scale-ins or rearms, or add a stop.
Every program needs a take-profit (`take all at entry + N`, OCO with the stop): without an upper exit a rally makes
the fee bound unbounded and the verifier rejects with max_fees_cents. You cannot change the envelope. Do not add
commentary."""


def _droid_bin() -> str | None:
    return shutil.which(config.DROID_BIN)


def droid_sdk_available() -> tuple[bool, str]:
    """(usable, reason). The SDK needs the package, FACTORY_API_KEY and the local `droid` binary."""
    try:
        import droid_sdk  # noqa: F401
    except ImportError:
        return False, "droid-sdk not installed"
    if not os.environ.get("FACTORY_API_KEY"):
        return False, "FACTORY_API_KEY unset"
    if not _droid_bin():
        return False, f"`{config.DROID_BIN}` executable not on PATH (the SDK drives it)"
    return True, f"droid-sdk {getattr(droid_sdk, 'PACKAGE_VERSION', '?')} with FACTORY_API_KEY and `{config.DROID_BIN}`"


def droid_cli_available() -> tuple[bool, str]:
    b = _droid_bin()
    if not b:
        return False, f"`{config.DROID_BIN}` not on PATH"
    return True, f"{b}" + ("" if os.environ.get("FACTORY_API_KEY") else " (no FACTORY_API_KEY; relies on a local `droid` login)")


def _run_coroutine(coro, timeout: float):
    """Run an awaitable to completion from sync code, whether or not this thread already has a
    running loop (FastAPI sync routes run in a worker thread; tests may hold a loop)."""
    try:
        asyncio.get_running_loop()
    except RuntimeError:
        return asyncio.run(asyncio.wait_for(coro, timeout))
    box: dict = {}

    def runner():
        try:
            box["value"] = asyncio.run(asyncio.wait_for(coro, timeout))
        except BaseException as e:               # surfaced below on the calling thread
            box["error"] = e
    t = threading.Thread(target=runner, name="droid-sdk", daemon=True)
    t.start()
    t.join(timeout + 5)
    if "error" in box:
        raise box["error"]
    if "value" not in box:
        raise TimeoutError("droid-sdk run did not finish")
    return box["value"]


def _droid_sdk_repair(prompt: str) -> str | None:
    """One Droid turn through the SDK. Returns the assistant text, or None when the run did not
    succeed (the SDK reports execution failures as values, not exceptions)."""
    import droid_sdk

    kwargs: dict = {}
    if DROID_MODEL:
        kwargs["model"] = DROID_MODEL
    if DROID_REASONING_EFFORT:
        kwargs["reasoning_effort"] = DROID_REASONING_EFFORT
    cfg = droid_sdk.SessionConfig(auto_reject_permission_requests=True)       # read-only: no edits, no shell
    with tempfile.TemporaryDirectory(prefix="droid-repair-") as cwd:      # an empty cwd: the three inputs are all it sees
        result = _run_coroutine(droid_sdk.run(
            prompt, cwd=cwd, timeout=DROID_TIMEOUT_S, config=cfg,
            runtime=droid_sdk.Runtime(executable=_droid_bin()),
            api_key=os.environ.get("FACTORY_API_KEY"), **kwargs), DROID_TIMEOUT_S)
    if getattr(result, "success", False) and "```" in (result.text or ""):
        log.info("droid-sdk: %s turns, %s, session %s", result.turn_count, result.duration, result.session_id)
        return result.text
    log.warning("droid-sdk run did not produce source: subtype=%s error=%s text=%r",
                getattr(result, "subtype", None), getattr(result, "error", None), (getattr(result, "text", "") or "")[:200])
    return None


def _droid_cli_repair(prompt: str) -> str | None:
    """`droid exec -o json "<prompt>"`: the JSON on stdout carries type, subtype, is_error,
    duration_ms, num_turns, result, session_id. Returns `result`, or None on failure."""
    argv = [config.DROID_BIN, "exec", "-o", "json"]
    if DROID_MODEL:
        argv += ["--model", DROID_MODEL]
    if DROID_REASONING_EFFORT:
        argv += ["--reasoning-effort", DROID_REASONING_EFFORT]
    argv.append(prompt)
    with tempfile.TemporaryDirectory(prefix="droid-repair-") as cwd:
        p = subprocess.run(argv, capture_output=True, text=True, timeout=DROID_TIMEOUT_S, cwd=cwd)
    if p.returncode != 0:
        log.warning("droid exec failed (%s): %s", p.returncode, (p.stderr or p.stdout)[:300])
        return None
    text = None
    try:
        doc = json.loads(p.stdout.strip().splitlines()[-1]) if p.stdout.strip() else {}
        if doc.get("is_error"):
            log.warning("droid exec reported is_error: %s", json.dumps(doc)[:300])
            return None
        text = doc.get("result")
        log.info("droid exec: %s turns, %s ms, session %s", doc.get("num_turns"), doc.get("duration_ms"), doc.get("session_id"))
    except (ValueError, IndexError):
        text = p.stdout                                   # an older CLI without JSON output
    if text and "```" in text:
        return text
    log.warning("droid exec returned no fenced source: %r", (text or "")[:200])
    return None


def ask_droid(source: str, envelope: dict, rejection: dict, tick_cents: int) -> tuple[str, str]:
    """Returns (patched source, backend name). The prompt holds exactly the source, the envelope
    and the counterexample; the answer is parsed for a single ```mpl block and nothing else."""
    ctx = {"source": source, "envelope": envelope, "rejection": {k: rejection.get(k) for k in
           ("violates", "limit", "value_on_path", "counterexample")}, "tick_cents": tick_cents}
    user = ("TASK: repair the program so the verifier certifies it.\n<context>\n" + json.dumps(ctx) + "\n</context>\n"
            "Return only the corrected MPL in a ```mpl block.")
    prompt = DROID_SYSTEM + "\n\n" + user

    ok, why = droid_sdk_available()
    if ok:
        try:
            text = _droid_sdk_repair(prompt)
            if text:
                return ai_gateway.extract_mpl(text), "factory-droid-sdk"
        except Exception as e:                            # the SDK raises on setup/connection/timeout
            log.warning("droid-sdk unavailable (%s); falling back", e)
    else:
        log.debug("droid-sdk skipped: %s", why)

    ok, why = droid_cli_available()
    if ok:
        try:
            text = _droid_cli_repair(prompt)
            if text:
                return ai_gateway.extract_mpl(text), "factory-droid-cli"
        except (OSError, subprocess.TimeoutExpired) as e:
            log.warning("droid exec unavailable: %s", e)
    else:
        log.debug("droid CLI skipped: %s", why)

    llm = ai_gateway.get_llm()
    out = llm.complete(DROID_SYSTEM, user, max_tokens=2000)
    backend = "droid-via-stub" if out.route == "stub" else f"droid-via-{out.route}:{out.model}"
    return ai_gateway.extract_mpl(out.text), backend


def repair(gw: Gateway, key: keys.KeyInfo, program_id: int, max_rounds: int = config.REPAIR_MAX_ROUNDS) -> tuple[int, dict]:
    """Run up to `max_rounds` droid rounds starting from a rejected program. Returns the last
    (status, body) from `submit_program`, plus the incident reports."""
    with gw.db.connect() as c:
        p = gw._program(c, program_id)
        a = gw._account(c, p["account"])
    keys.require_author(key, p["account"])
    if p["status"] != "rejected" or not p["result"]:
        raise ApiError(409, "not_rejected", program_status=p["status"])
    envelope = a["envelope"]
    tick = int(gw.venue.instruments()[p["instruments"][0]]["tick_cents"]) if p["instruments"] else 100
    source, rejection = p["source"], p["result"]
    current_pid = program_id
    reports = []
    status, body = 422, rejection
    for round_no in range(1, max_rounds + 1):
        t0 = time.perf_counter()
        try:
            patched, backend = ask_droid(source, envelope, rejection, tick)
        except ai_gateway.LLMError as e:
            raise ApiError(503, "droid_unavailable", detail=str(e))
        status, body = gw.submit_program(key, p["account"], p["backend"], patched, parent_program_id=current_pid)
        report = {
            "round": round_no, "backend": backend, "t": now_ms(), "ms": int((time.perf_counter() - t0) * 1000),
            "failed": {"program_id": current_pid, "violates": rejection.get("violates"), "limit": rejection.get("limit"),
                       "value_on_path": rejection.get("value_on_path"),
                       "path": _path_summary(rejection.get("counterexample", []))},
            "changed": _diff(source, patched),
            "result": {"program_id": body.get("program_id"), "status": body.get("status") or body.get("error"),
                       "http": status},
        }
        reports.append(report)
        _attach_incident(gw, current_pid, report)
        gw.emit(p["account"], body.get("program_id"), "program.repair_round", report)
        if status == 200:
            break
        if status == 409:
            break               # certified but the budget cannot cover it: nothing to repair
        source = patched
        rejection = body if status == 422 else {"violates": body.get("error"), "counterexample": [],
                                                "limit": None, "value_on_path": body.get("message")}
        current_pid = body.get("program_id", current_pid)
    body = dict(body)
    body["repair"] = {"rounds": len(reports), "origin_program_id": program_id, "incidents": reports}
    return status, body


def _attach_incident(gw: Gateway, pid: int, report: dict) -> None:
    with gw.db.tx() as c:
        row = c.execute("SELECT incidents_json FROM programs WHERE id=?", (pid,)).fetchone()
        inc = json.loads(row["incidents_json"]) if row else []
        inc.append(report)
        c.execute("UPDATE programs SET incidents_json=? WHERE id=?", (json.dumps(inc), pid))


def _path_summary(path: list) -> str:
    parts = []
    for s in path:
        if "env" in s:
            parts.append(f"{s['env']} {s.get('at', s.get('to'))}")
        else:
            ev = s.get("event")
            if ev == "filled":
                parts.append(f"{s['node']} filled {s['qty']}@{s['price']}")
            elif ev == "triggered":
                parts.append(f"{s['node']} triggered (limit {s.get('limit')})")
            elif ev == "rests":
                parts.append(f"{s['node']} rests: {s.get('reason')}")
            elif ev == "mark":
                parts.append(f"mark loss {s['loss_cents']}c with position {s['position']}")
    return " -> ".join(parts)


def _diff(a: str, b: str) -> list[str]:
    import difflib
    return [l for l in difflib.unified_diff(a.splitlines(), b.splitlines(), lineterm="", n=0)
            if not l.startswith(("---", "+++", "@@"))]
