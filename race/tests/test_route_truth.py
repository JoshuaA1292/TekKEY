"""Every label that names a route names the route the model actually takes. A `RoutedLLM` built around
Chalk with no Anthropic provider behind it says "anthropic-direct" for a claude-* id and "chalk" for a
grok-* id, whatever it was bound to first; the evals' pinned model and the race's comparison rows say
the same; phone numbers in the state are masked."""
from __future__ import annotations

import json

import pytest

from chalk import ai_gateway
from gateway.db import Database
from race.conductor import mask_phone
from race.store import RaceStore


@pytest.fixture(autouse=True)
def _clean(monkeypatch):
    for v in ("CHALK_ROUTER_API_KEY", "CHALK_API_KEY", "CHALK_ENV_ID", "ANTHROPIC_API_KEY", "ANTHROPIC_AUTH_TOKEN", "XAI_API_KEY"):
        monkeypatch.delenv(v, raising=False)
    monkeypatch.setenv("CHALK_SKIP_PROBE", "1")
    ai_gateway.set_llm(None)
    yield
    ai_gateway.set_llm(None)


def _routed(vendors=None, xai=False):
    chalk = ai_gateway.ChalkLLM(api_key="ck", env_id="env", default_model="grok-4.20-0309-non-reasoning")
    anth = ai_gateway.AnthropicLLM(api_key="sk-ant-test")
    x = ai_gateway.OpenAICompatLLM("https://api.x.ai/v1", "xai-key", "xai-direct", "grok-4.7") if xai else None
    return ai_gateway.RoutedLLM(chalk, anth, served=[], xai=x, vendors=vendors if vendors is not None else {"xai": True, "anthropic": False})


def test_routed_llm_route_is_per_model_not_composite():
    llm = _routed()
    assert llm.primary_route == "chalk"
    assert llm.route == "chalk" and llm.default_model == "grok-4.20-0309-non-reasoning"
    assert llm.route_for("grok-4.7") == "chalk"
    assert llm.route_for("claude-sonnet-5-5") == "anthropic-direct"
    sonnet = llm.with_model("claude-sonnet-5-5")
    assert sonnet is not llm and sonnet.route == "anthropic-direct" and sonnet.default_model == "claude-sonnet-5-5"
    assert llm.route == "chalk"                                             # the original is untouched
    assert sonnet.with_model("grok-4.7").route == "chalk"
    assert ai_gateway.route_of(llm, "claude-sonnet-5-5") == "anthropic-direct"
    assert ai_gateway.route_of(llm, "grok-4.20-0309-non-reasoning") == "chalk"
    assert ai_gateway.route_of(sonnet) == "anthropic-direct"
    assert "via anthropic-direct" in sonnet.describe()


def test_claude_through_chalk_when_chalk_serves_anthropic_and_grok_direct_when_configured():
    llm = _routed(vendors={"xai": False, "anthropic": True}, xai=True)
    assert llm.route_for("claude-sonnet-5-5") == "chalk"
    assert llm.route_for("grok-4.7") == "xai-direct" and llm.route == "xai-direct"
    assert ai_gateway.route_of(ai_gateway.StubLLM(), "claude-sonnet-5-5") == "stub"
    assert ai_gateway.route_of(None) == "stub"
    assert ai_gateway.route_of(ai_gateway.AnthropicLLM(api_key="k"), "grok-4.7") == "anthropic-direct"   # a single backend has one route


def test_get_llm_binds_the_truthful_route(monkeypatch):
    monkeypatch.setenv("CHALK_ROUTER_API_KEY", "ck")
    monkeypatch.setenv("CHALK_ENV_ID", "env")
    monkeypatch.setenv("ANTHROPIC_API_KEY", "sk-ant-x")
    monkeypatch.delenv("CHALK_SKIP_PROBE", raising=False)                 # the probes below are patched: no network
    monkeypatch.setattr(ai_gateway, "chalk_models", lambda ttl_s=60.0: [])
    monkeypatch.setattr(ai_gateway, "chalk_vendors", lambda ttl_s=600.0: {"xai": True, "anthropic": False})
    monkeypatch.setattr(ai_gateway, "_vendor_cache", {"t": 0.0, "ok": None})
    assert ai_gateway.planned_route()[0] == "chalk"
    base = ai_gateway.get_llm("grok-4.20-0309-non-reasoning")
    assert isinstance(base, ai_gateway.RoutedLLM) and base.route == "chalk"
    sonnet = ai_gateway.get_llm("claude-sonnet-5-5")
    assert sonnet.route == "anthropic-direct"
    from evals.run import _PinnedModel
    assert _PinnedModel(base, "claude-sonnet-5-5").route == "anthropic-direct"
    assert _PinnedModel(base, "grok-4.7").route == "chalk"


def test_epoch_summary_route_comes_from_the_recorded_calls(tmp_path):
    store = RaceStore(Database(str(tmp_path / "r.db")))
    # an epoch whose config said the composite "chalk" while every answered call went to Anthropic directly
    e = store.open_epoch({"mode": "backtest", "model": "claude-sonnet-5-5", "route": "chalk", "backtest": None})
    for n in (1, 2):
        rid = store.open_round(e, n, [{"t": 1, "kind": "market", "text": "m"}])
        store.set_lane_round(rid, "raw", {"decision": {"action": "hold"}, "llm": {"route": "anthropic-direct", "model": "claude-sonnet-5-5"}})
        store.set_lane_round(rid, "bounded", {"decision": {"action": "hold"}, "llm": {"route": "chalk", "model": "x", "error": "LLMError: boom"}})
        store.close_round(rid)
    rows = store.rounds(e, 10)
    assert rows[0]["id"] == rows[0]["round_id"] and [r["n"] for r in rows] == [1, 2]
    s = store.epoch_summary(e, 25, 1_000_000)
    assert s["route"] == "anthropic-direct" and s["route_configured"] == "chalk" and s["routes_seen"] == {"anthropic-direct": 2}
    assert s["label"] == "backtest on claude-sonnet-5-5 via anthropic-direct"
    # with no answered call the configured route stands, and a given label is kept
    e2 = store.open_epoch({"mode": "backtest", "model": "grok-4.7", "route": "chalk", "backtest": {"label": "flash crash hours on grok-4.7 via chalk"}})
    s2 = store.epoch_summary(e2, 25, 1_000_000)
    assert s2["route"] == "chalk" and s2["label"] == "flash crash hours on grok-4.7 via chalk"


def test_mask_phone_never_shows_the_number():
    assert mask_phone("+15551234567") == "+1***4567"
    assert mask_phone("15551234567") == "+1***4567"
    assert mask_phone("+447700900123") == "+***0123"
    assert mask_phone("12") == "***" and mask_phone(None) is None
    assert "555123" not in json.dumps(mask_phone("+15551234567"))
