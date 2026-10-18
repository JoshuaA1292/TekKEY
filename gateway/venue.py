"""B4: the venue client. `policyc` / `verify` / `replay` as subprocesses, `venued` over a socket.

VENUE=real selects RealVenue (Build A's binaries); VENUE=fake selects FakeVenue. The gateway
only ever talks to the `Venue` interface below.
"""
from __future__ import annotations

import json
import logging
import os
import socket
import subprocess
import tempfile
import threading
import time
import uuid
from typing import Iterator, Protocol

from . import config

log = logging.getLogger("gateway.venue")


class VenueError(Exception):
    def __init__(self, error: str, detail: str = ""):
        super().__init__(f"{error}: {detail}")
        self.error, self.detail = error, detail


class Venue(Protocol):
    name: str
    def instruments(self) -> dict: ...
    def compile(self, source: str, env: dict) -> tuple[int, dict]: ...            # 0 compiled, 2 static, 1 internal
    def verify(self, compiled: dict, envelope: dict, mark: int, timeout_ms: int) -> tuple[int, dict]: ...  # 0/3/4/1
    def replay(self, compiled: dict, path: list) -> dict: ...
    def deploy(self, program_id: int, account: int, compiled: dict, fund_cents: int) -> dict: ...
    def pause(self, program_id: int) -> dict: ...
    def cancel(self, program_id: int) -> dict: ...
    def state(self, program_id: int) -> dict: ...
    def mark(self, instrument: str) -> int: ...
    def reconcile(self) -> list[dict]: ...
    def subscribe(self) -> Iterator[dict]: ...


def strip_envelope_for_verifier(envelope: dict) -> dict:
    """CONTRACT.md §2.1: the verifier is stateless; it gets the limits, not the budget state."""
    keep = ("contract_version", "account", "window", "max_loss_cents", "max_fees_cents", "max_trades",
            "max_exposure_lots", "instruments", "require_live_stop", "env")
    return {k: envelope[k] for k in keep if k in envelope}


class RealVenue:
    name = "real"

    def __init__(self, build_dir=None, host=None, port=None):
        self.build = build_dir or config.MINSKY_BUILD
        self.host = host or config.VENUED_HOST
        self.port = port or config.VENUED_PORT
        self._ins_path = config.instruments_path()
        self._lock = threading.Lock()

    # ---- CLIs -------------------------------------------------------------------------
    def instruments(self) -> dict:
        return json.loads(open(self._ins_path).read())

    def _run(self, argv: list[str], stdin: str | None = None, timeout: float = 60) -> tuple[int, str, str]:
        p = subprocess.run(argv, input=stdin, capture_output=True, text=True, timeout=timeout)
        return p.returncode, p.stdout, p.stderr

    def compile(self, source: str, env: dict) -> tuple[int, dict]:
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
            json.dump(env, f)
            env_path = f.name
        try:
            code, out, err = self._run([str(self.build / "policyc"), "--backend", "minsky",
                                        "--instruments", str(self._ins_path), "--env", env_path], stdin=source)
        finally:
            os.unlink(env_path)
        if code == 1:
            log.error("policyc internal error: %s", err)
            return 1, {"error": "internal", "stderr": err}
        return code, json.loads(out)

    def verify(self, compiled: dict, envelope: dict, mark: int, timeout_ms: int) -> tuple[int, dict]:
        with tempfile.TemporaryDirectory() as d:
            pp, ep = os.path.join(d, "p.json"), os.path.join(d, "e.json")
            json.dump(compiled, open(pp, "w"))
            json.dump(strip_envelope_for_verifier(envelope), open(ep, "w"))
            code, out, err = self._run([str(self.build / "verify"), "--program", pp, "--envelope", ep,
                                        "--mark", str(mark), "--timeout-ms", str(timeout_ms)],
                                       timeout=timeout_ms / 1000 + 30)
        if code == 1:
            log.error("verify internal error: %s", err)
            return 1, {"error": "internal", "stderr": err}
        return code, json.loads(out) if out.strip() else {"status": "timeout"}

    def replay(self, compiled: dict, path: list) -> dict:
        with tempfile.TemporaryDirectory() as d:
            pp, pa = os.path.join(d, "p.json"), os.path.join(d, "path.json")
            json.dump(compiled, open(pp, "w"))
            json.dump(path, open(pa, "w"))
            code, out, err = self._run([str(self.build / "replay"), "--program", pp, "--path", pa,
                                        "--exchange", config.EXCHANGE_URL], timeout=120)
        if code != 0:
            raise VenueError("replay_failed", err)
        return json.loads(out)

    # ---- venued ---------------------------------------------------------------------
    def _request(self, op: str, retries: int = 3, **fields) -> dict:
        req = {"contract_version": 1, "op": op, "request_id": str(uuid.uuid4()), **fields}
        line = json.dumps(req) + "\n"
        last = None
        for attempt in range(retries):
            try:
                with socket.create_connection((self.host, self.port), timeout=10) as s:
                    s.sendall(line.encode())
                    buf = b""
                    while not buf.endswith(b"\n"):
                        chunk = s.recv(65536)
                        if not chunk:
                            break
                        buf += chunk
                resp = json.loads(buf.decode())
                if not resp.get("ok", False):
                    raise VenueError(resp.get("error", "unknown"), resp.get("detail", ""))
                return resp
            except (OSError, json.JSONDecodeError) as e:   # same request_id: venued is idempotent
                last = e
                time.sleep(0.2 * (attempt + 1))
        raise VenueError("exchange_down", str(last))

    def deploy(self, program_id, account, compiled, fund_cents):
        self._compiled = getattr(self, "_compiled", {})
        self._compiled[program_id] = compiled
        return self._request("deploy", program_id=program_id, account=account, compiled=compiled, fund_cents=fund_cents)

    def pause(self, program_id):
        return self._request("pause", program_id=program_id)

    def cancel(self, program_id):
        return self._request("cancel", program_id=program_id)

    def state(self, program_id):
        return self._request("state", program_id=program_id)

    def mark(self, instrument) -> int:
        """venued's last print; before any print has happened it is null, so DEFAULT_MARK applies."""
        lp = self._request("mark", instrument=instrument).get("last_print")
        return int(lp) if lp is not None else config.DEFAULT_MARK

    def reconcile(self) -> list[dict]:
        return self._request("reconcile").get("programs", [])

    # ---- test conveniences (the gateway never calls these; the suite and the eval runner do) ---
    def set_path(self, program_id: int, path: list) -> None:
        """Remember a program's worst path so `drive` can replay it (the fake does the same)."""
        with self._lock:
            self._paths = getattr(self, "_paths", {})
            self._paths[program_id] = path

    def drive(self, program_id: int, steps: int | None = None) -> dict:
        """Play the remembered path on the live venue with `replay --attach` (environment moves only;
        the key holder's moves belong to venued). `steps` truncates the path (used to open a position
        and stop)."""
        path = getattr(self, "_paths", {}).get(program_id)
        if path is None:
            raise VenueError("no_path", f"program {program_id} has no remembered path")
        if steps is not None:
            path = path[:steps]
        sub = self.state(program_id).get("sub_account")
        compiled = self._compiled_for(program_id)
        with tempfile.TemporaryDirectory() as d:
            pp, pa = os.path.join(d, "p.json"), os.path.join(d, "path.json")
            json.dump(compiled, open(pp, "w"))
            json.dump(path, open(pa, "w"))
            argv = [str(self.build / "replay"), "--program", pp, "--path", pa, "--exchange", config.EXCHANGE_URL,
                    "--attach", str(sub), "--instruments", str(self._ins_path)]
            if steps is not None:
                argv.append("--keep")          # leave the position open: the test continues from here
            code, out, err = self._run(argv, timeout=120)
        if code != 0:
            raise VenueError("replay_failed", err[-400:])
        return json.loads(out)

    # ---- CONTRACT §9.2 (proposed): a binary-key control group on the live exchange -------------
    def raw_supported(self) -> bool:
        """True once venued answers raw_state (a bad_request/unknown op means not yet)."""
        try:
            self._request("raw_state", sub_account=0)
            return True
        except VenueError as e:
            return e.error not in ("bad_request", "unknown_op")

    def raw_open(self, account: int, fund_cents: int) -> int:
        return int(self._request("raw_open", account=account, fund_cents=fund_cents)["sub_account"])

    def raw_order(self, sub_account: int, instrument: str, side: str, qty: int, kind: str = "limit",
                  price: int | None = None, trigger: int | None = None, reduce_only: bool = False) -> int:
        return int(self._request("raw_order", sub_account=sub_account, instrument=instrument, side=side, qty=qty,
                                 kind=kind, price=price, trigger=trigger, reduce_only=reduce_only)["order_id"])

    def raw_cancel(self, sub_account: int, order_id: int) -> None:
        self._request("raw_cancel", sub_account=sub_account, order_id=order_id)

    def raw_state(self, sub_account: int) -> dict:
        return self._request("raw_state", sub_account=sub_account)

    def _compiled_for(self, program_id: int) -> dict:
        self._compiled = getattr(self, "_compiled", {})
        return self._compiled[program_id]

    def subscribe(self) -> Iterator[dict]:
        """A long-lived connection; yields one event per line. Reconnects on failure."""
        while True:
            try:
                with socket.create_connection((self.host, self.port), timeout=None) as s:
                    s.sendall((json.dumps({"contract_version": 1, "op": "subscribe",
                                           "request_id": str(uuid.uuid4())}) + "\n").encode())
                    f = s.makefile("r")
                    for line in f:
                        line = line.strip()
                        if not line:
                            continue
                        ev = json.loads(line)
                        if ev.get("ok") is not None and "type" not in ev:
                            continue   # the subscribe ack
                        yield ev
            except OSError as e:
                log.warning("venued subscribe dropped: %s; reconnecting", e)
                time.sleep(1.0)


def make_venue(kind: str | None = None, **kw) -> Venue:
    kind = kind or config.VENUE
    if kind == "real":
        return RealVenue(**kw)
    from .fake_venue import FakeVenue
    return FakeVenue(**kw)
