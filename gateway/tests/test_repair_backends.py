"""The Droid backend order in gateway/repair.py: SDK, CLI, Chalk LLM, stub, with the SDK and the
CLI mocked. The real `droid` binary is not needed; the boundary (source in, source out) is."""
import json
import os
import subprocess
import sys
import types
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from chalk import ai_gateway  # noqa: E402
from gateway import repair  # noqa: E402

SRC = "instrument ETH\nentry buy 1 at 3000\nsl: exit all at entry - 20 when price <= entry - 20   oco tp\ntp: take all at entry + 30   oco sl\n"
ENV = {"max_loss_cents": 4000, "max_fees_cents": 500, "max_trades": 60, "max_exposure_lots": 2, "instruments": ["ETH"],
       "require_live_stop": True, "env": {"max_gap_ticks": 25, "fill": "full_at_print", "fee_bps": {"maker": 1.0, "taker": 3.5}, "trigger_ref": "last"}}
REJ = {"violates": "max_loss_cents", "limit": 4000, "value_on_path": 4500, "counterexample": [{"env": "gap", "to": 2950}]}
PATCHED = "instrument ETH\nentry buy 1 at 3000\nsl: exit all at entry - 10 allow 25 when price <= entry - 10   oco tp\ntp: take all at entry + 30   oco sl\n"


@pytest.fixture(autouse=True)
def _offline(monkeypatch):
    monkeypatch.delenv("FACTORY_API_KEY", raising=False)
    monkeypatch.setattr(repair.shutil, "which", lambda name: None)
    ai_gateway.set_llm(ai_gateway.StubLLM())
    yield
    ai_gateway.set_llm(None)


class _Result:
    def __init__(self, text, success=True):
        self.text, self.success, self.subtype = text, success, "success" if success else "error_during_execution"
        self.turn_count, self.duration, self.session_id, self.error = 1, "0:00:01", "sess-1", None


def _fake_sdk(monkeypatch, text, success=True, seen=None):
    """A stand-in for the installed droid_sdk module with the three names repair.py uses."""
    mod = types.ModuleType("droid_sdk")
    mod.PACKAGE_VERSION = "0.5.0-test"

    class SessionConfig:
        def __init__(self, **kw):
            self.kw = kw

    class Runtime:
        def __init__(self, **kw):
            self.kw = kw

    async def run(prompt, **kw):
        if seen is not None:
            seen.append({"prompt": prompt, **kw})
        return _Result(text, success)
    mod.SessionConfig, mod.Runtime, mod.run = SessionConfig, Runtime, run
    monkeypatch.setitem(sys.modules, "droid_sdk", mod)
    return mod


def test_sdk_runs_first_and_sees_only_the_three_inputs(monkeypatch):
    seen = []
    _fake_sdk(monkeypatch, "Here you go:\n```mpl\n" + PATCHED + "```\n", seen=seen)
    monkeypatch.setenv("FACTORY_API_KEY", "fk-test")
    monkeypatch.setattr(repair.shutil, "which", lambda name: "/usr/local/bin/droid")
    src, backend = repair.ask_droid(SRC, ENV, REJ, 100)
    assert backend == "factory-droid-sdk" and src == PATCHED
    call = seen[0]
    assert call["api_key"] == "fk-test" and call["runtime"].kw["executable"] == "/usr/local/bin/droid"
    assert call["config"].kw["auto_reject_permission_requests"] is True
    assert not os.path.exists(call["cwd"]) or not os.listdir(call["cwd"])    # an empty scratch cwd, gone afterwards
    ctx = json.loads(call["prompt"].split("<context>\n")[1].split("\n</context>")[0])
    assert set(ctx) == {"source", "envelope", "rejection", "tick_cents"}
    assert ctx["source"] == SRC and ctx["rejection"]["counterexample"] == REJ["counterexample"]


def test_sdk_needs_key_and_binary_otherwise_cli_then_llm(monkeypatch):
    _fake_sdk(monkeypatch, "```mpl\n" + PATCHED + "```")
    # package present, key present, no binary: the SDK cannot drive anything and the CLI is absent too
    monkeypatch.setenv("FACTORY_API_KEY", "fk-test")
    assert repair.droid_sdk_available()[0] is False and repair.droid_cli_available()[0] is False
    src, backend = repair.ask_droid(SRC, ENV, REJ, 100)
    assert backend == "droid-via-stub" and "allow 25" in src


def test_cli_parses_the_json_result_field(monkeypatch):
    monkeypatch.setattr(repair.shutil, "which", lambda name: "/usr/local/bin/droid")
    monkeypatch.delitem(sys.modules, "droid_sdk", raising=False)
    monkeypatch.setitem(sys.modules, "droid_sdk", None)                      # import fails: SDK skipped
    calls = []

    def fake_run(argv, **kw):
        calls.append((argv, kw))
        doc = {"type": "result", "subtype": "success", "is_error": False, "duration_ms": 1200, "num_turns": 1,
               "result": "```mpl\n" + PATCHED + "```", "session_id": "s-1"}
        return subprocess.CompletedProcess(argv, 0, stdout=json.dumps(doc), stderr="")
    monkeypatch.setattr(repair.subprocess, "run", fake_run)
    src, backend = repair.ask_droid(SRC, ENV, REJ, 100)
    assert backend == "factory-droid-cli" and src == PATCHED
    argv, kw = calls[0]
    assert argv[:4] == ["droid", "exec", "-o", "json"] and argv[-1].startswith(repair.DROID_SYSTEM)
    assert kw["capture_output"] and kw["cwd"]


def test_cli_error_falls_through_to_the_llm(monkeypatch):
    monkeypatch.setattr(repair.shutil, "which", lambda name: "/usr/local/bin/droid")
    monkeypatch.setitem(sys.modules, "droid_sdk", None)

    def fake_run(argv, **kw):
        return subprocess.CompletedProcess(argv, 0, stdout=json.dumps({"type": "result", "is_error": True, "result": "no"}), stderr="")
    monkeypatch.setattr(repair.subprocess, "run", fake_run)
    src, backend = repair.ask_droid(SRC, ENV, REJ, 100)
    assert backend == "droid-via-stub" and "allow 25" in src


def test_sdk_failure_value_falls_through_to_cli(monkeypatch):
    _fake_sdk(monkeypatch, "", success=False)
    monkeypatch.setenv("FACTORY_API_KEY", "fk-test")
    monkeypatch.setattr(repair.shutil, "which", lambda name: "/usr/local/bin/droid")

    def fake_run(argv, **kw):
        return subprocess.CompletedProcess(argv, 0, stdout=json.dumps({"is_error": False, "result": "```mpl\n" + PATCHED + "```"}), stderr="")
    monkeypatch.setattr(repair.subprocess, "run", fake_run)
    src, backend = repair.ask_droid(SRC, ENV, REJ, 100)
    assert backend == "factory-droid-cli" and src == PATCHED


def test_chalk_backend_is_named_with_its_model(monkeypatch):
    class Fake(ai_gateway.LLM):
        route = "chalk"

        def complete(self, system, user, model=None, max_tokens=4000):
            return ai_gateway.Completion(text="```mpl\n" + PATCHED + "```", model="openai/gpt-5.6-luna", route="chalk")
    ai_gateway.set_llm(Fake())
    src, backend = repair.ask_droid(SRC, ENV, REJ, 100)
    assert backend == "droid-via-chalk:openai/gpt-5.6-luna" and src == PATCHED
