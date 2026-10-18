"""Daytona lifecycle for the delegate sandbox (BUILD_B_GATEWAY.md B8).

What is real: with `DAYTONA_API_KEY` set, `SandboxManager` drives the `daytona` SDK (0.225):
`Daytona.create(CreateSandboxFromImageParams(...))` builds the image from the same recipe as
`agent/image/Dockerfile`, with the egress policy applied as `domain_allow_list` /
`network_allow_list`, and `sandbox.process.exec(...)` runs `python -m agent.delegate` inside it.
`delete` calls `sandbox.delete()`.

What is simulated: with no `DAYTONA_API_KEY`, `LocalSandbox` runs the same command as a local
subprocess with a scrubbed environment containing only the delegate's variables. Local mode
cannot enforce egress (the process can reach whatever the host can) and is for development
only; `SandboxHandle.backend == "local"` says so.

Either way, deleting a sandbox only kills the delegate process. A deployed program lives in the
gateway and on the venue, with its own `bk_` key; nothing in this module can reach it.
`delete_on_burst(gateway, manager)` is a `Gateway.listeners` hook that deletes a delegate's
sandbox when `rejections.burst` arrives for its author key, the signature of a turned agent.
"""
from __future__ import annotations

import json
import logging
import os
import shlex
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Sequence
from urllib.parse import urlsplit

log = logging.getLogger("agent.sandbox")

ROOT = Path(__file__).resolve().parents[1]
IMAGE_DIR = Path(__file__).resolve().parent / "image"
EGRESS_TEMPLATE = IMAGE_DIR / "egress.json"
DELEGATE_ENV = ("AUTHOR_KEY", "GATEWAY_URL", "CHALK_AI_GATEWAY_URL")      # the only variables the delegate sees
OPTIONAL_ENV = ("CHALK_API_KEY", "CHALK_ROUTER_API_KEY", "CHALK_ENV_ID", "CHALK_ROUTER_URL")   # passed through only when set


@dataclass
class SandboxHandle:
    delegate_id: str
    backend: str                      # "daytona" | "local"
    sandbox_id: str
    gateway_url: str
    chalk_url: str
    author_key_id: int | None = None
    created_t: int = field(default_factory=lambda: int(time.time() * 1000))
    author_key: str = field(default="", repr=False)       # never printed, never logged

    def public(self) -> dict:
        return {k: v for k, v in self.__dict__.items() if k != "author_key"}


# ----------------------------------------------------------------------------- egress
def _host_port(url: str) -> tuple[str, int | None]:
    u = urlsplit(url if "//" in url else "//" + url)
    return (u.hostname or ""), u.port


def _is_ip(host: str) -> bool:
    import ipaddress
    try:
        ipaddress.ip_address(host)
        return True
    except ValueError:
        return False


def egress_policy(gateway_url: str, chalk_url: str | None) -> dict:
    """`agent/image/egress.json` with the two hosts filled in. Exactly two destinations."""
    pol = json.loads(EGRESS_TEMPLATE.read_text(encoding="utf-8"))
    gh, gp = _host_port(gateway_url)
    ch, cp = _host_port(chalk_url) if chalk_url else ("", None)
    out = []
    for d in pol["destinations"]:
        if d["name"] == "gateway":
            d["host"], d["ports"] = gh, sorted({p for p in (443, 80, gp) if isinstance(p, int)})
            out.append(d)
        elif d["name"] == "chalk-ai-gateway" and ch:
            d["host"], d["ports"] = ch, sorted({p for p in (443, cp) if isinstance(p, int)})
            out.append(d)
    pol["destinations"] = out
    return pol


def network_params(gateway_url: str, chalk_url: str | None) -> dict[str, Any]:
    """The Daytona create-params that enforce the policy at the network layer: names go to
    `domain_allow_list`, literal IPs to `network_allow_list` (as /32 or /128). With neither host
    the sandbox gets `network_block_all`."""
    domains, cidrs = [], []
    for url in (gateway_url, chalk_url):
        if not url:
            continue
        host, _ = _host_port(url)
        if not host:
            continue
        if _is_ip(host):
            cidrs.append(f"{host}/{'128' if ':' in host else '32'}")
        else:
            domains.append(host)
    if not domains and not cidrs:
        return {"network_block_all": True}
    params: dict[str, Any] = {}
    if domains:
        params["domain_allow_list"] = ",".join(dict.fromkeys(domains))
    if cidrs:
        params["network_allow_list"] = ",".join(dict.fromkeys(cidrs))
    return params


def delegate_env(author_key: str, gateway_url: str, chalk_url: str | None, chalk_api_key: str | None = None) -> dict[str, str]:
    env = {"AUTHOR_KEY": author_key, "GATEWAY_URL": gateway_url, "CHALK_AI_GATEWAY_URL": chalk_url or ""}
    if chalk_api_key:
        env["CHALK_API_KEY"] = chalk_api_key
    # the Chalk Model Gateway needs the router key and the environment id (lab/CONTRACT.md §6)
    for k in ("CHALK_ROUTER_API_KEY", "CHALK_ENV_ID", "CHALK_ROUTER_URL"):
        if os.environ.get(k):
            env[k] = os.environ[k]
    return env


def delegate_command(brief: str, account: int, envelope: dict, extra_messages: Sequence[str] = (),
                     source_file: str | None = None, python: str = "python") -> list[str]:
    cmd = [python, "-m", "agent.delegate", "--brief", brief, "--account", str(int(account)),
           "--envelope", json.dumps(envelope)]
    for m in extra_messages:
        cmd += ["--message", str(m)]
    if source_file:
        cmd += ["--source-file", source_file]
    return cmd


def _parse_result(stdout: str, stderr: str, exit_code: int | None) -> dict:
    """The delegate prints one JSON object as its last line."""
    for line in reversed((stdout or "").strip().splitlines()):
        line = line.strip()
        if line.startswith("{"):
            try:
                out = json.loads(line)
                out.setdefault("exit_code", exit_code)
                return out
            except ValueError:
                continue
    return {"status": "error", "error": "delegate produced no result", "exit_code": exit_code,
            "stdout": (stdout or "")[-2000:], "stderr": (stderr or "")[-2000:]}


# ----------------------------------------------------------------------------- local fallback
class LocalSandbox:
    """Development-only stand-in: a subprocess with a scrubbed environment. It cannot enforce
    egress; the process can reach whatever the host can. It does scrub the environment, so the
    delegate still sees only AUTHOR_KEY, GATEWAY_URL, CHALK_AI_GATEWAY_URL (and CHALK_API_KEY when
    set) plus PYTHONPATH so `agent` and `chalk` import."""

    def __init__(self, python: str | None = None, cwd: Path = ROOT):
        self.python = python or sys.executable
        self.cwd = cwd
        self._procs: dict[str, subprocess.Popen] = {}
        self._lock = threading.Lock()

    def run(self, handle: SandboxHandle, env: dict[str, str], cmd: list[str], timeout: float) -> dict:
        scrubbed = {**env, "PYTHONPATH": str(self.cwd), "PYTHONUNBUFFERED": "1"}
        p = subprocess.Popen(cmd, cwd=str(self.cwd), env=scrubbed, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        with self._lock:
            self._procs[handle.sandbox_id] = p
        try:
            out, err = p.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            p.kill()
            out, err = p.communicate()
            return {"status": "error", "error": f"delegate timed out after {timeout}s", "stderr": err[-2000:]}
        finally:
            with self._lock:
                self._procs.pop(handle.sandbox_id, None)
        return _parse_result(out, err, p.returncode)

    def kill(self, handle: SandboxHandle) -> None:
        with self._lock:
            p = self._procs.pop(handle.sandbox_id, None)
        if p is not None and p.poll() is None:
            p.kill()


# ----------------------------------------------------------------------------- manager
class SandboxManager:
    """Create one sandbox per delegate, run the delegate in it, delete it. Backend is Daytona
    when `DAYTONA_API_KEY` is set (or a `daytona` client is passed), else `LocalSandbox`."""

    def __init__(self, daytona: Any | None = None, *, force_local: bool = False, python: str | None = None,
                 ttl_minutes: int = 120, create_timeout: float = 300.0, chalk_api_key: str | None = None):
        self._daytona = daytona
        self.chalk_api_key = chalk_api_key if chalk_api_key is not None else os.environ.get("CHALK_API_KEY")
        self.ttl_minutes = ttl_minutes
        self.create_timeout = create_timeout
        self.local = LocalSandbox(python=python)
        self._handles: dict[str, SandboxHandle] = {}
        self._lock = threading.Lock()
        if force_local:
            self.backend = "local"
        elif daytona is not None or os.environ.get("DAYTONA_API_KEY"):
            self.backend = "daytona"
        else:
            self.backend = "local"
            log.warning("sandbox backend: local subprocess (DAYTONA_API_KEY unset). Egress is NOT enforced; development only.")

    # ---- daytona ------------------------------------------------------------------
    def _client(self):
        if self._daytona is None:
            from daytona import Daytona                       # reads DAYTONA_API_KEY / DAYTONA_API_URL / DAYTONA_TARGET
            self._daytona = Daytona()
        return self._daytona

    @staticmethod
    def image():
        """The same recipe as agent/image/Dockerfile, declared for Daytona's builder."""
        from daytona import Image
        return (Image.base("python:3.12-slim")
                .pip_install("httpx>=0.27", "anthropic>=0.40")
                .add_local_dir(str(ROOT / "agent"), "/app/agent")
                .add_local_dir(str(ROOT / "chalk"), "/app/chalk")
                .env({"PYTHONPATH": "/app", "PYTHONUNBUFFERED": "1", "PYTHONDONTWRITEBYTECODE": "1"})
                .workdir("/app"))

    def _daytona_params(self, delegate_id: str, env: dict[str, str], gateway_url: str, chalk_url: str | None,
                        author_key_id: int | None):
        from daytona import CreateSandboxFromImageParams
        labels = {"role": "delegate", "delegate_id": delegate_id}
        if author_key_id is not None:
            labels["author_key_id"] = str(author_key_id)
        return CreateSandboxFromImageParams(
            image=self.image(), name=f"delegate-{delegate_id}"[:63], language="python",
            env_vars=env, labels=labels, ephemeral=True, ttl_minutes=self.ttl_minutes,
            **network_params(gateway_url, chalk_url))

    # ---- lifecycle ----------------------------------------------------------------
    def create(self, delegate_id: str, author_key: str, gateway_url: str, chalk_url: str | None,
               *, author_key_id: int | None = None) -> SandboxHandle:
        if not author_key.startswith("ak_"):
            raise ValueError("a delegate sandbox takes an author key (ak_...) and nothing else")
        env = delegate_env(author_key, gateway_url, chalk_url, self.chalk_api_key)
        egress = "allow-list"
        if self.backend == "daytona":
            params = self._daytona_params(delegate_id, env, gateway_url, chalk_url, author_key_id)
            try:
                sb = self._client().create(params, timeout=self.create_timeout, on_snapshot_create_logs=log.debug)
            except Exception as e:                                   # DaytonaBadRequestError on tiers 1 and 2
                msg = str(e)
                if "Network access is restricted" not in msg and "AllowList" not in msg:
                    raise
                # Daytona tiers 1 and 2: the organisation's policy (essential services only) applies and a
                # sandbox-level allow list is refused. Create without one and say so on the handle.
                log.warning("Daytona refused the sandbox-level egress policy (tier policy applies): %s", msg[:160])
                for k in ("domain_allow_list", "network_allow_list", "network_block_all"):
                    if hasattr(params, k):
                        setattr(params, k, None)
                sb = self._client().create(params, timeout=self.create_timeout, on_snapshot_create_logs=log.debug)
                egress = "daytona-tier-policy"
            sandbox_id = sb.id
        else:
            sandbox_id = f"local:{delegate_id}:{os.getpid()}:{int(time.time() * 1000)}"
        h = SandboxHandle(delegate_id=delegate_id, backend=self.backend, sandbox_id=sandbox_id,
                          gateway_url=gateway_url, chalk_url=chalk_url or "", author_key_id=author_key_id,
                          author_key=author_key)
        h.egress = egress
        with self._lock:
            self._handles[delegate_id] = h
        log.info("sandbox created: %s (egress: %s)", h.public(), egress)
        return h

    def probe_reachability(self, handle: SandboxHandle, url: str, timeout: float = 20.0) -> tuple[bool, str]:
        """Can the sandbox reach `url`? (Daytona tiers 1 and 2 allow essential services only.)"""
        if self.backend != "daytona":
            return True, "local subprocess shares this host's network"
        code = ("import urllib.request\n"
                "try:\n    print('OK', urllib.request.urlopen(%r, timeout=8).status)\n"
                "except Exception as e:\n    print('ERR', type(e).__name__, str(e)[:80])" % url)
        try:
            sb = self._client().get(handle.sandbox_id)
            r = sb.process.exec("python -c \"%s\"" % code.replace('"', '\\"'), timeout=int(timeout))
            out = (r.result or "").strip()
        except Exception as e:
            return False, f"probe failed: {type(e).__name__}: {str(e)[:100]}"
        return out.startswith("OK"), out[:160]

    def run_delegate(self, handle: SandboxHandle, brief: str, *, account: int, envelope: dict,
                     extra_messages: Sequence[str] = (), source_file: str | None = None,
                     timeout: float = 600.0) -> dict:
        """Execute `python -m agent.delegate --brief ...` inside the sandbox and return its JSON result.
        `account` and `envelope` travel on the command line, not the environment, so the
        environment holds exactly the delegate's variables."""
        if self.backend == "daytona":
            cmd = delegate_command(brief, account, envelope, extra_messages, source_file, python="python")
            sb = self._client().get(handle.sandbox_id)
            resp = sb.process.exec(shlex.join(cmd), cwd="/app", timeout=int(timeout))
            return _parse_result(resp.result or "", "", resp.exit_code)
        env = delegate_env(handle.author_key, handle.gateway_url, handle.chalk_url, self.chalk_api_key)
        cmd = delegate_command(brief, account, envelope, extra_messages, source_file, python=self.local.python)
        return self.local.run(handle, env, cmd, timeout)

    def delete(self, handle: SandboxHandle) -> None:
        """Kill the delegate. Touches nothing in the gateway: a deployed program keeps running on
        its own bk_ key."""
        with self._lock:
            self._handles.pop(handle.delegate_id, None)
        if handle.backend == "daytona":
            try:
                self._client().get(handle.sandbox_id).delete()
            except Exception as e:                            # already gone is fine; report the rest
                if "not found" not in str(e).lower() and type(e).__name__ != "DaytonaNotFoundError":
                    raise
        else:
            self.local.kill(handle)
        handle.author_key = ""
        log.info("sandbox deleted: %s", handle.public())

    # ---- lookup -------------------------------------------------------------------
    def handles(self) -> list[SandboxHandle]:
        with self._lock:
            return list(self._handles.values())

    def get(self, delegate_id: str) -> SandboxHandle | None:
        with self._lock:
            return self._handles.get(delegate_id)

    def handles_for_author_key(self, author_key_id: int, resolve: Callable[[str], int | None] | None = None) -> list[SandboxHandle]:
        """Sandboxes created for this author key. Handles created without `author_key_id` are
        resolved through `resolve(plaintext) -> key id` (the gateway can hash and look it up)."""
        out = []
        for h in self.handles():
            if h.author_key_id is None and resolve is not None and h.author_key:
                try:
                    h.author_key_id = resolve(h.author_key)
                except Exception:
                    log.exception("could not resolve author key id for %s", h.delegate_id)
            if h.author_key_id == author_key_id:
                out.append(h)
        return out


# ----------------------------------------------------------------------------- the burst hook
def delete_on_burst(gateway, manager: SandboxManager) -> Callable[[dict], None]:
    """A `Gateway.listeners` entry: on `rejections.burst` for an author key, delete that
    delegate's sandbox and tell the owner's event stream. Deployed programs are untouched."""

    def resolve(token: str) -> int | None:
        from gateway import keys                              # gateway-side only; the image never runs this
        with gateway.db.connect() as c:
            row = c.execute("SELECT id FROM keys WHERE hash=?", (keys.hash_key(token),)).fetchone()
        return int(row["id"]) if row else None

    def listener(ev: dict) -> None:
        if ev.get("type") != "rejections.burst":
            return
        kid = ev.get("author_key_id")
        if kid is None:
            return
        for h in manager.handles_for_author_key(int(kid), resolve):
            try:
                manager.delete(h)
            except Exception:
                log.exception("sandbox delete failed for %s", h.delegate_id)
                continue
            gateway.emit(ev.get("account"), None, "sandbox.deleted",
                         {"delegate_id": h.delegate_id, "author_key_id": kid, "reason": "rejections.burst",
                          "backend": h.backend})

    listener.__name__ = "delete_on_burst"
    return listener
