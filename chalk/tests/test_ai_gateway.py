"""The two Chalk wire formats, byte for byte, against a mock transport. No network."""
import json
import os
import sys
from pathlib import Path

import httpx
import httpx2  # the Anthropic SDK's HTTP stack; the chat-completions path uses httpx
import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from chalk import ai_gateway  # noqa: E402

BASE = "https://api.chalk.ai/v1/router"


@pytest.fixture(autouse=True)
def _clean(monkeypatch):
    for v in ("CHALK_ROUTER_API_KEY", "CHALK_API_KEY", "CHALK_ENV_ID", "CHALK_ROUTER_URL", "CHALK_AI_GATEWAY_URL",
              "ANTHROPIC_API_KEY", "ANTHROPIC_AUTH_TOKEN"):
        monkeypatch.delenv(v, raising=False)
    ai_gateway.set_llm(None)
    yield
    ai_gateway.set_llm(None)


def _recorder(reply: dict, status: int = 200, lib=httpx):
    seen = []

    def handler(request):
        seen.append(request)
        return lib.Response(status, json=reply)
    return seen, lib.MockTransport(handler)


def test_claude_model_goes_through_anthropic_sdk_on_chalk_base_url():
    seen, transport = _recorder({
        "id": "msg_1", "type": "message", "role": "assistant", "model": "claude-opus-5-5",
        "content": [{"type": "text", "text": "```mpl\ninstrument ETH\n```"}],
        "stop_reason": "end_turn", "stop_sequence": None, "usage": {"input_tokens": 12, "output_tokens": 7}}, lib=httpx2)
    llm = ai_gateway.ChalkLLM(api_key="ck_test", env_id="env_123", base_url=BASE,
                              anthropic_http_client=httpx2.Client(transport=transport))
    out = llm.complete("SYS", "USER", model="claude-opus-5-5", max_tokens=55)
    assert out.route == "chalk" and out.model == "claude-opus-5-5" and out.text.startswith("```mpl")
    assert (out.input_tokens, out.output_tokens) == (12, 7)
    assert len(seen) == 1
    req = seen[0]
    assert str(req.url) == BASE + "/v1/messages"
    assert req.headers["authorization"] == "Bearer ck_test"
    assert req.headers["x-chalk-env-id"] == "env_123"
    body = json.loads(req.content)
    assert body["model"] == "claude-opus-5-5" and body["max_tokens"] == 55
    assert body["system"] == "SYS" and body["messages"] == [{"role": "user", "content": "USER"}]


def test_vendor_model_goes_through_chat_completions_with_same_headers():
    seen, transport = _recorder({
        "id": "chatcmpl-1", "model": "openai/gpt-5.6-luna",
        "choices": [{"index": 0, "finish_reason": "stop", "message": {"role": "assistant", "content": "```mpl\nx\n```"}}],
        "usage": {"prompt_tokens": 20, "completion_tokens": 9}})
    llm = ai_gateway.ChalkLLM(api_key="ck_test", env_id="env_123", base_url=BASE,
                              http_client=httpx.Client(transport=transport))
    out = llm.complete("SYS", "USER", model="openai/gpt-5.6-luna", max_tokens=77)
    assert out.route == "chalk" and out.model == "openai/gpt-5.6-luna" and out.text == "```mpl\nx\n```"
    assert (out.input_tokens, out.output_tokens) == (20, 9)
    req = seen[0]
    assert str(req.url) == BASE + "/chat/completions"
    assert req.headers["authorization"] == "Bearer ck_test"
    assert req.headers["x-chalk-env-id"] == "env_123"
    assert req.headers["content-type"] == "application/json"
    assert json.loads(req.content) == {
        "model": "openai/gpt-5.6-luna", "max_tokens": 77,
        "messages": [{"role": "system", "content": "SYS"}, {"role": "user", "content": "USER"}]}


def test_google_model_also_takes_the_chat_completions_path_and_errors_surface_as_llmerror():
    seen, transport = _recorder({"error": {"message": "over budget"}}, status=429)
    llm = ai_gateway.ChalkLLM(api_key="k", env_id="e", base_url=BASE, http_client=httpx.Client(transport=transport))
    with pytest.raises(ai_gateway.LLMError, match="rate limited"):
        llm.complete("s", "u", model="google/gemini-3-pro")
    assert str(seen[0].url) == BASE + "/chat/completions"


def test_get_llm_precedence_chalk_then_anthropic_then_stub(monkeypatch):
    assert ai_gateway.planned_route()[0] == "stub"
    assert isinstance(ai_gateway.get_llm(), ai_gateway.StubLLM)
    ai_gateway.set_llm(None)

    monkeypatch.setenv("ANTHROPIC_API_KEY", "sk-ant-x")
    assert ai_gateway.planned_route()[0] == "anthropic-direct"
    assert ai_gateway.get_llm().route == "anthropic-direct"
    ai_gateway.set_llm(None)

    monkeypatch.setenv("CHALK_ROUTER_API_KEY", "ck")
    assert ai_gateway.planned_route()[0] == "anthropic-direct"       # env id still missing
    monkeypatch.setenv("CHALK_ENV_ID", "env")
    assert ai_gateway.planned_route()[0] == "chalk"
    llm = ai_gateway.get_llm()
    assert isinstance(llm, ai_gateway.ChalkLLM) and llm.base_url == BASE and llm.headers["X-Chalk-Env-Id"] == "env"
    ai_gateway.set_llm(None)

    monkeypatch.delenv("CHALK_ROUTER_API_KEY")
    monkeypatch.setenv("CHALK_API_KEY", "old-key")                   # the older names still work
    monkeypatch.setenv("CHALK_AI_GATEWAY_URL", "https://example.test/router/")
    llm = ai_gateway.get_llm()
    assert isinstance(llm, ai_gateway.ChalkLLM) and llm.base_url == "https://example.test/router"
    assert llm.headers["Authorization"] == "Bearer old-key"


def test_get_llm_with_model_binds_a_default_without_touching_the_singleton(monkeypatch):
    monkeypatch.setenv("CHALK_ROUTER_API_KEY", "ck")
    monkeypatch.setenv("CHALK_ENV_ID", "env")
    base = ai_gateway.get_llm()
    bound = ai_gateway.get_llm("openai/gpt-5.6-luna")
    assert bound is not base and bound.default_model == "openai/gpt-5.6-luna"
    assert base.default_model == ai_gateway.DEFAULT_MODEL and ai_gateway.get_llm() is base
    assert bound.http is base.http                                   # one client, shared


def test_stub_and_helpers_unchanged():
    out = ai_gateway.StubLLM().complete("", "TASK: envelope\n<context>\n{\"transcript\": \"forty dollars a day, gap 25\"}\n</context>")
    assert out.route == "stub" and json.loads(ai_gateway.extract_json(out.text))["max_loss_cents"] == 4000
    assert ai_gateway.extract_mpl("```mpl\na\n```") == "a\n"
