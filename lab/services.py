"""`GET /lab/status` (lab/CONTRACT.md §3): is each sponsor service live, and if not, why.

`probe_all()` returns `{name: {"live": bool, "detail": str, "mode": str}}` for venue, llm, bank,
sandbox, phone and droid. Every probe is time-boxed to PROBE_TIMEOUT_S (2 s) on its own thread and
never raises: a hung socket or a slow SDK import becomes `live: False` with the reason. `mode` names
what the gateway would actually use (`real`/`fake`, `chalk`/`anthropic-direct`/`stub`,
`nessie`/`local`, `daytona`/`local`, `photon`/`log`, `factory-sdk`/`factory-cli`/`chalk`/`stub`).

What is real: each probe makes one cheap call to the real thing (a TCP connect to venued, a GET to
Nessie, a one-page list on Daytona, GET /health on the phone sidecar, a 1-token completion when
LAB_PROBE_LLM=1). What is not: with LAB_PROBE_LLM unset the llm probe only reports the route that
`chalk.ai_gateway.get_llm()` would take, without spending a token.
"""
from __future__ import annotations

import os
import shutil
import socket
import sys
from concurrent.futures import ThreadPoolExecutor, TimeoutError as FutureTimeout
from pathlib import Path
from typing import Callable

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

PROBE_TIMEOUT_S = float(os.environ.get("LAB_PROBE_TIMEOUT_S", "2"))
VENUE_BINARIES = ("policyc", "verify", "replay", "venued")


def _result(live: bool, detail: str, mode: str) -> dict:
    return {"live": bool(live), "detail": detail, "mode": mode}


# ---- the probes -------------------------------------------------------------------------------

def probe_venue() -> dict:
    from gateway import config
    build = Path(os.environ.get("MINSKY_BUILD", str(config.MINSKY_BUILD)))
    missing = [b for b in VENUE_BINARIES if not (build / b).exists()]
    host, port = os.environ.get("VENUED_HOST", config.VENUED_HOST), int(os.environ.get("VENUED_PORT", config.VENUED_PORT))
    if missing:
        return _result(False, f"missing {', '.join(missing)} in {build}; the fake venue serves fixtures", "fake")
    try:
        socket.create_connection((host, port), timeout=min(1.0, PROBE_TIMEOUT_S)).close()
    except OSError as e:
        return _result(False, f"binaries present but venued not reachable on {host}:{port} ({e.__class__.__name__})", "fake")
    return _result(True, f"policyc/verify/replay in {build}; venued on {host}:{port}", "real")


def probe_llm() -> dict:
    from chalk import ai_gateway
    route, reason = ai_gateway.planned_route()
    if os.environ.get("LAB_PROBE_LLM") == "1" and route != "stub":
        try:
            out = ai_gateway.get_llm().complete("Reply with one word.", "ping", max_tokens=1)
            return _result(route == "chalk", f"{reason}; 1-token call answered by {out.model} via {out.route}", out.route)
        except Exception as e:                      # LLMError or anything the SDK raises
            return _result(False, f"{reason}; probe call failed: {str(e)[:120]}", route)
    return _result(route == "chalk", reason + ("" if route == "stub" else " (route only; set LAB_PROBE_LLM=1 to spend a token)"), route)


def probe_bank() -> dict:
    key = os.environ.get("NESSIE_API_KEY", "")
    base = os.environ.get("NESSIE_BASE", "https://v2-prod-api.nessieisreal.com").rstrip("/")
    if not key:
        return _result(False, "NESSIE_API_KEY unset; LocalBank keeps the same ledger in SQLite", "local")
    import httpx
    # Only a write proves the key. Create a throwaway customer (Nessie v2: Bearer auth).
    body = {"first_name": "Probe", "last_name": "Gateway", "address": {"street_number": "1", "street_name": "Oxford St",
                                                                        "city": "Cambridge", "state": "MA", "zip": "02138"}}
    try:
        r = httpx.post(f"{base}/customers", headers={"Authorization": f"Bearer {key}"}, json=body, timeout=PROBE_TIMEOUT_S)
    except httpx.HTTPError as e:
        return _result(False, f"NESSIE_API_KEY set but {base} unreachable ({e.__class__.__name__})", "local")
    if r.status_code in (200, 201):
        return _result(True, f"Nessie accepts writes at {base} (POST /customers {r.status_code})", "nessie")
    if r.status_code == 429:
        return _result(True, f"Nessie v2 at {base}: key accepted, rate limited right now (120 requests/min per key)", "nessie")
    return _result(False, f"Nessie at {base} rejected a write: {r.status_code} {r.text[:40].strip()}; local bank in use", "local")


def probe_sandbox() -> dict:
    if not os.environ.get("DAYTONA_API_KEY"):
        return _result(False, "DAYTONA_API_KEY unset; LocalSandbox (no egress control)", "local")
    try:
        from daytona import Daytona
    except ImportError as e:
        return _result(False, f"DAYTONA_API_KEY set but the daytona SDK is not importable ({e})", "local")
    try:
        d = Daytona()
        it = d.list()
        next(iter(it), None)                         # one page is enough to prove the API answers
    except Exception as e:
        return _result(False, f"Daytona SDK could not list sandboxes: {str(e)[:120]}", "local")
    return _result(True, "Daytona API answers (list sandboxes)", "daytona")


def probe_phone() -> dict:
    project = os.environ.get("PHOTON_PROJECT_ID", "")
    url = os.environ.get("PHONE_SIDECAR_URL", "http://127.0.0.1:8787").rstrip("/")
    import httpx
    secret = os.environ.get("PHOTON_PROJECT_SECRET", "")
    if project.startswith("pho_prj_") and secret:
        # Photon 2.0 Beta: driven directly over REST by gateway/photon_beta.py, no sidecar needed
        try:
            r = httpx.get(f"https://api.photon.codes/v1/projects/{project}", timeout=PROBE_TIMEOUT_S,
                          headers={"Authorization": f"Bearer {secret}"})
        except httpx.HTTPError as e:
            return _result(False, f"Photon Beta API unreachable ({e.__class__.__name__}); alarms go to the log outbox", "log")
        if r.status_code == 200:
            name = r.json().get("name") or project
            owner = os.environ.get("PHOTON_OWNER_NUMBER")
            line = " ; owner number not set (PHOTON_OWNER_NUMBER), so no line is assigned yet" if not owner else ""
            return _result(True, f"Photon 2.0 Beta project {name} (iMessage shared line){line}", "photon-beta")
        return _result(False, f"Photon Beta rejected the project secret: {r.status_code}", "log")
    try:
        r = httpx.get(f"{url}/health", timeout=PROBE_TIMEOUT_S)
        health = r.json() if r.status_code == 200 else {}
    except (httpx.HTTPError, ValueError) as e:
        health = None
        err = e.__class__.__name__
    if health is None:
        why = f"phone sidecar not answering at {url} ({err}); alarms go to the log outbox"
        return _result(False, why if project else "PHOTON_PROJECT_ID unset; " + why, "log")
    if not project:
        return _result(False, f"sidecar up at {url} but PHOTON_PROJECT_ID unset in the gateway; alarms go to the log outbox", "log")
    if health.get("live"):
        proj = health.get("project") or {}
        return _result(True, f"Spectrum connected: project {proj.get('name') or project}, {proj.get('line') or 'iMessage line'}", "photon")
    return _result(False, f"sidecar up at {url} in {health.get('mode', '?')} mode: {health.get('last_error') or 'no Spectrum session'}", "log")


def probe_droid() -> dict:
    from gateway import repair
    ok, why = repair.droid_sdk_available()
    if ok:
        return _result(True, why, "factory-sdk")
    sdk_why = why
    ok, why = repair.droid_cli_available()
    if ok:
        return _result(True, f"{why}; SDK skipped: {sdk_why}", "factory-cli")
    from chalk import ai_gateway
    route, reason = ai_gateway.planned_route()
    return _result(False, f"no Droid ({sdk_why}; {why}); repair falls back to {route}: {reason}", route)


PROBES: dict[str, Callable[[], dict]] = {
    "venue": probe_venue, "llm": probe_llm, "bank": probe_bank,
    "sandbox": probe_sandbox, "phone": probe_phone, "droid": probe_droid,
}


def probe_all(names: list[str] | None = None, timeout_s: float = PROBE_TIMEOUT_S) -> dict[str, dict]:
    """Run every probe on its own thread, each cut off at `timeout_s`. Never raises."""
    names = names or list(PROBES)
    out: dict[str, dict] = {}
    pool = ThreadPoolExecutor(max_workers=len(names), thread_name_prefix="lab-probe")
    futures = {n: pool.submit(PROBES[n]) for n in names if n in PROBES}
    for n, f in futures.items():
        try:
            out[n] = f.result(timeout=timeout_s)
        except FutureTimeout:
            out[n] = _result(False, f"probe timed out after {timeout_s:.0f}s", "unknown")
        except Exception as e:
            out[n] = _result(False, f"probe failed: {e.__class__.__name__}: {str(e)[:120]}", "unknown")
    pool.shutdown(wait=False, cancel_futures=True)
    return out


if __name__ == "__main__":
    import json
    print(json.dumps(probe_all(), indent=1))
