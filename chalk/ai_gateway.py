"""Every LLM call in Build B goes through here (BUILD_B_GATEWAY.md B10, lab/CONTRACT.md §5-6).

Routing, in order:
  1. Chalk Model Gateway  -> when a router key (CHALK_ROUTER_API_KEY, or the older CHALK_API_KEY)
                             and CHALK_ENV_ID are set. Base URL https://api.chalk.ai/v1/router
                             (override: CHALK_ROUTER_URL, or the older CHALK_AI_GATEWAY_URL).
                             Anthropic ids (`claude-*`) go through the Anthropic SDK with
                             base_url=<base> and the X-Chalk-Env-Id header, which Chalk serves at
                             <base>/v1/messages. Any id with a slash (`openai/*`, `google/*`) goes
                             to the OpenAI-compatible <base>/chat/completions over httpx with the
                             same two headers. Every Completion records the route of the backend that answered
                             it ("chalk" here) and the model; `route_of(llm, model)` says it in advance.
  2. Anthropic credentials only -> the Anthropic API directly, with a logged warning that Chalk
                             is being bypassed (development only). route="anthropic-direct".
  3. nothing              -> StubLLM: deterministic, rule-based, no network. Good enough to run
                             the pipeline and the evals offline; labelled route="stub".

What is real: the two Chalk call shapes are exercised byte-for-byte in chalk/tests/test_ai_gateway.py
against a mock transport (URL, headers, body). What is not verified from this repository: a live
round trip, because no Chalk key was available when this was written.
"""
from __future__ import annotations

import time

import copy
import logging
import os
import re
from dataclasses import dataclass
from typing import Any

import httpx

log = logging.getLogger("chalk.ai_gateway")

DEFAULT_MODEL = os.environ.get("GATEWAY_MODEL", "claude-opus-5-5")
CHALK_ROUTER_DEFAULT_URL = "https://api.chalk.ai/v1/router"


@dataclass
class Completion:
    text: str
    model: str
    route: str          # chalk | anthropic-direct | stub
    input_tokens: int = 0
    output_tokens: int = 0


class LLMError(Exception):
    pass


class LLM:
    route = "unset"
    default_model: str = DEFAULT_MODEL

    def complete(self, system: str, user: str, model: str | None = None, max_tokens: int = 4000) -> Completion:
        raise NotImplementedError

    def with_model(self, model: str | None) -> "LLM":
        """The same backend (clients shared) with a different default model."""
        if not model or model == self.default_model:
            return self
        other = copy.copy(self)
        other.default_model = model
        return other

    def describe(self) -> str:
        return f"{self.route} (default model {self.default_model})"


def is_anthropic_model(model: str) -> bool:
    """`claude-*` ids take the Messages API path; anything with a slash is an OpenAI-compatible id."""
    return model.startswith("claude") and "/" not in model


def _anthropic_call(client: Any, anthropic_mod: Any, model: str, system: str, user: str, max_tokens: int, route: str) -> Completion:
    try:
        resp = client.messages.create(
            model=model, max_tokens=max_tokens, system=system,
            messages=[{"role": "user", "content": user}],
            output_config={"effort": "medium"},
        )
    except anthropic_mod.RateLimitError as e:
        raise LLMError(f"rate limited: {e.message}") from e
    except anthropic_mod.APIStatusError as e:
        raise LLMError(f"api error {e.status_code}: {e.message}") from e
    except anthropic_mod.APIConnectionError as e:
        raise LLMError(f"connection error: {e}") from e
    if resp.stop_reason == "refusal":
        raise LLMError("model refused")
    text = "".join(b.text for b in resp.content if b.type == "text")
    return Completion(text=text, model=resp.model or model, route=route,
                      input_tokens=resp.usage.input_tokens, output_tokens=resp.usage.output_tokens)


class AnthropicLLM(LLM):
    """The Anthropic API directly. Development only; the gateway logs that Chalk is bypassed."""

    def __init__(self, base_url: str | None = None, api_key: str | None = None, route: str = "anthropic-direct",
                 default_headers: dict[str, str] | None = None, http_client: Any = None, auth_token: str | None = None):
        import anthropic
        kwargs: dict[str, Any] = {}
        if base_url:
            kwargs["base_url"] = base_url
        if api_key:
            kwargs["api_key"] = api_key          # sent as x-api-key (Anthropic direct)
        if auth_token:
            kwargs["auth_token"] = auth_token    # sent as Authorization: Bearer (what Chalk's router checks)
        if default_headers:
            kwargs["default_headers"] = default_headers
        if http_client is not None:
            kwargs["http_client"] = http_client
        self.client = anthropic.Anthropic(**kwargs)
        self.route = route
        self._anthropic = anthropic

    def complete(self, system: str, user: str, model: str | None = None, max_tokens: int = 4000) -> Completion:
        return _anthropic_call(self.client, self._anthropic, model or self.default_model, system, user, max_tokens, self.route)


class ChalkLLM(LLM):
    """The Chalk Model Gateway. One key, one environment, two wire formats:

      claude-*   -> Anthropic SDK, base_url=<base>, headers Authorization: Bearer + X-Chalk-Env-Id
                    (the SDK appends /v1/messages, so the request lands on <base>/v1/messages)
      <vendor>/* -> POST <base>/chat/completions, OpenAI chat-completions body, same headers, via httpx

    Budgets and rate limits are per key on Chalk's side; a 429 surfaces as LLMError("rate limited").
    """
    route = "chalk"

    def __init__(self, api_key: str, env_id: str, base_url: str = CHALK_ROUTER_DEFAULT_URL,
                 default_model: str | None = None, http_client: httpx.Client | None = None,
                 anthropic_http_client: Any = None, timeout: float = 120.0):
        # `anthropic_http_client` is an httpx2.Client (the Anthropic SDK's HTTP stack); `http_client` is httpx.
        self.api_key = api_key
        self.env_id = env_id
        self.base_url = base_url.rstrip("/")
        self.default_model = default_model or DEFAULT_MODEL
        self.headers = {"Authorization": f"Bearer {api_key}", "X-Chalk-Env-Id": env_id}
        self.http = http_client or httpx.Client(timeout=timeout)
        self._anthropic_http = anthropic_http_client
        self._anthropic_llm: AnthropicLLM | None = None

    # ---- claude-* : Anthropic Messages API through the router ------------------------------
    def _anthropic(self) -> AnthropicLLM:
        if self._anthropic_llm is None:
            self._anthropic_llm = AnthropicLLM(base_url=self.base_url, auth_token=self.api_key, route="chalk",
                                               default_headers={"X-Chalk-Env-Id": self.env_id},
                                               http_client=self._anthropic_http)
        return self._anthropic_llm

    # ---- vendor/* : OpenAI-compatible chat completions --------------------------------------
    def _chat_completion(self, model: str, system: str, user: str, max_tokens: int) -> Completion:
        body = {"model": model, "max_tokens": max_tokens,
                "messages": [{"role": "system", "content": system}, {"role": "user", "content": user}]}
        try:
            r = self.http.post(f"{self.base_url}/chat/completions", json=body,
                               headers={**self.headers, "Content-Type": "application/json"})
        except httpx.HTTPError as e:
            raise LLMError(f"connection error: {e}") from e
        if r.status_code == 429:
            raise LLMError(f"rate limited: {r.text[:200]}")
        if r.status_code >= 300:
            raise LLMError(f"api error {r.status_code}: {r.text[:300]}")
        try:
            data = r.json()
            choice = data["choices"][0]
            content = choice["message"]["content"]
        except (ValueError, KeyError, IndexError, TypeError) as e:
            raise LLMError(f"malformed chat completion: {r.text[:300]}") from e
        if isinstance(content, list):                      # some providers return content parts
            content = "".join(p.get("text", "") for p in content if isinstance(p, dict))
        if choice.get("finish_reason") == "content_filter":
            raise LLMError("model refused")
        usage = data.get("usage") or {}
        return Completion(text=content or "", model=data.get("model") or model, route="chalk",
                          input_tokens=int(usage.get("prompt_tokens") or 0),
                          output_tokens=int(usage.get("completion_tokens") or 0))

    def complete(self, system: str, user: str, model: str | None = None, max_tokens: int = 4000) -> Completion:
        model = model or self.default_model
        if is_anthropic_model(model):
            return self._anthropic().complete(system, user, model=model, max_tokens=max_tokens)
        return self._chat_completion(model, system, user, max_tokens)

    def describe(self) -> str:
        return f"chalk router at {self.base_url} env {self.env_id} (default model {self.default_model})"


class OpenAICompatLLM(LLM):
    """An OpenAI-compatible chat-completions endpoint called directly (xAI at https://api.x.ai/v1 when
    XAI_API_KEY is set). Used per model id by `RoutedLLM`; the route name says which vendor answered."""

    def __init__(self, base_url: str, api_key: str, route: str, default_model: str, http_client: httpx.Client | None = None, timeout: float = 120.0):
        self.base_url, self.route, self.default_model = base_url.rstrip("/"), route, default_model
        self.headers = {"Authorization": f"Bearer {api_key}", "Content-Type": "application/json"}
        self.http = http_client or httpx.Client(timeout=timeout)

    def complete(self, system: str, user: str, model: str | None = None, max_tokens: int = 4000) -> Completion:
        model = (model or self.default_model).split("/", 1)[-1]
        body = {"model": model, "max_tokens": max_tokens, "messages": [{"role": "system", "content": system}, {"role": "user", "content": user}]}
        try:
            r = self.http.post(f"{self.base_url}/chat/completions", json=body, headers=self.headers)
        except httpx.HTTPError as e:
            raise LLMError(f"connection error: {e}") from e
        if r.status_code == 429:
            raise LLMError(f"rate limited: {r.text[:200]}")
        if r.status_code >= 300:
            raise LLMError(f"api error {r.status_code}: {r.text[:300]}")
        try:
            data = r.json(); choice = data["choices"][0]; content = choice["message"]["content"]
        except (ValueError, KeyError, IndexError, TypeError) as e:
            raise LLMError(f"malformed chat completion: {r.text[:300]}") from e
        if isinstance(content, list):
            content = "".join(p.get("text", "") for p in content if isinstance(p, dict))
        usage = data.get("usage") or {}
        return Completion(text=content or "", model=data.get("model") or model, route=self.route,
                          input_tokens=int(usage.get("prompt_tokens") or 0), output_tokens=int(usage.get("completion_tokens") or 0))


def is_xai_model(model: str) -> bool:
    return model.startswith("grok") or model.startswith("xai/")


class StubLLM(LLM):
    """Offline stand-in. Writes a conservative MPL program from the brief's numbers and repairs
    by raising `allow` / tightening the stop. It never reads attack text: the eval marks its
    results as `stub`."""
    route = "stub"
    default_model = "stub"

    def complete(self, system: str, user: str, model: str | None = None, max_tokens: int = 4000) -> Completion:
        from . import stub_policy
        return Completion(text=stub_policy.respond(system, user), model="stub", route="stub")


# ---- configuration -------------------------------------------------------------------------

def chalk_settings() -> dict[str, str]:
    """The Chalk credentials as the environment gives them, new names first, old names accepted.
    `key` and `env_id` must both be present for the Chalk route."""
    return {
        "key": os.environ.get("CHALK_ROUTER_API_KEY") or os.environ.get("CHALK_API_KEY") or "",
        "env_id": os.environ.get("CHALK_ENV_ID") or "",
        "base_url": os.environ.get("CHALK_ROUTER_URL") or os.environ.get("CHALK_AI_GATEWAY_URL") or CHALK_ROUTER_DEFAULT_URL,
    }


_providers_cache: dict = {"t": 0.0, "models": None}


def chalk_models(ttl_s: float = 60.0) -> list[str] | None:
    """Model ids the Chalk Model Gateway will route for this environment (GET <base>/v1/models).
    None when the probe cannot tell (network error, non-200); [] when the environment has no provider."""
    import time
    s = chalk_settings()
    if not (s["key"] and s["env_id"]) or os.environ.get("CHALK_SKIP_PROBE") == "1":
        return None
    now = time.time()
    if _providers_cache["models"] is not None and now - _providers_cache["t"] < ttl_s:
        return _providers_cache["models"]
    try:
        import httpx
        r = httpx.get(f"{s['base_url'].rstrip('/')}/v1/models", timeout=8,
                      headers={"Authorization": f"Bearer {s['key']}", "X-Chalk-Env-Id": s["env_id"]})
        if r.status_code != 200:
            return None
        models = [m.get("id") for m in (r.json().get("data") or []) if isinstance(m, dict)]
    except Exception:
        return None
    _providers_cache.update(t=now, models=models)
    return models


_vendor_cache: dict = {"t": 0.0, "ok": None}


def chalk_vendors(ttl_s: float = 600.0) -> dict[str, bool]:
    """Which vendors Chalk's router actually serves for this environment, by one tiny call each
    (its /v1/models listing stays empty even with providers configured). Cached ten minutes."""
    import time
    s = chalk_settings()
    if not (s["key"] and s["env_id"]) or os.environ.get("CHALK_SKIP_PROBE") == "1":
        return {}
    now = time.time()
    if _vendor_cache["ok"] is not None and now - _vendor_cache["t"] < ttl_s:
        return _vendor_cache["ok"]
    base = s["base_url"].rstrip("/")
    H = {"Authorization": f"Bearer {s['key']}", "X-Chalk-Env-Id": s["env_id"]}
    ok: dict[str, bool] = {}
    try:
        r = httpx.post(f"{base}/v1/messages", timeout=20, headers={**H, "anthropic-version": "2023-06-01", "content-type": "application/json"},
                       json={"model": os.environ.get("CHALK_PROBE_CLAUDE", "claude-haiku-5-5"), "max_tokens": 1, "messages": [{"role": "user", "content": "."}]})
        ok["anthropic"] = r.status_code < 400
    except httpx.HTTPError:
        ok["anthropic"] = False
    for vendor, model in (("xai", os.environ.get("CHALK_PROBE_XAI", "xai/grok-4.20-0309-non-reasoning")),
                          ("openai", os.environ.get("CHALK_PROBE_OPENAI", "openai/gpt-5-mini"))):
        try:
            r = httpx.post(f"{base}/chat/completions", timeout=20, headers={**H, "content-type": "application/json"},
                           json={"model": model, "max_tokens": 1, "messages": [{"role": "user", "content": "."}]})
            ok[vendor] = r.status_code < 400
        except httpx.HTTPError:
            ok[vendor] = False
    _vendor_cache.update(t=now, ok=ok)
    return ok


def vendor_of(model: str) -> str:
    if is_anthropic_model(model):
        return "anthropic"
    if is_xai_model(model):
        return "xai"
    return model.split("/", 1)[0] if "/" in model else "openai"


def planned_route() -> tuple[str, str]:
    """(route, one-line reason) for what `get_llm()` would build, without building it."""
    s = chalk_settings()
    if s["key"] and s["env_id"]:
        models = chalk_models()
        vendors = chalk_vendors() if models is not None else {}
        served = sorted(v for v, okv in vendors.items() if okv)
        anth_direct = bool(os.environ.get("ANTHROPIC_API_KEY") or os.environ.get("ANTHROPIC_AUTH_TOKEN"))
        if models == [] and not served:
            # The gateway answers, the key and environment are right, but no provider key is configured
            # behind it: every completion would fail with "provider ... is not configured".
            why = (f"Chalk Model Gateway reachable (env {s['env_id']}) but no provider is configured in it: "
                   "add an Anthropic or OpenAI key under Model Gateway in the Chalk dashboard")
            if anth_direct:
                return "anthropic-direct", why + "; Anthropic credentials present, so the Anthropic API is called directly (Chalk bypassed)"
            return "stub", why + "; rule-based stub until then"
        if served and not vendors.get("anthropic") and anth_direct and is_anthropic_model(DEFAULT_MODEL):
            return "chalk", (f"Chalk Model Gateway serves {', '.join(served)} (env {s['env_id']}); no Anthropic provider behind it, "
                             f"so claude-* ids go to the Anthropic API directly")
        return "chalk", f"CHALK_ROUTER_API_KEY and CHALK_ENV_ID set; router {s['base_url']}" + (f"; vendors {', '.join(served)}" if served else "") + (f"; {len(models)} models" if models else "")
    if s["key"] or os.environ.get("CHALK_AI_GATEWAY_URL"):
        missing = "CHALK_ENV_ID" if s["key"] else "CHALK_ROUTER_API_KEY"
        reason = f"Chalk half-configured ({missing} missing)"
    else:
        reason = "no Chalk credentials"
    if os.environ.get("ANTHROPIC_API_KEY") or os.environ.get("ANTHROPIC_AUTH_TOKEN"):
        return "anthropic-direct", reason + "; Anthropic credentials present, Chalk bypassed (dev only)"
    return "stub", reason + "; no Anthropic credentials; rule-based stub"


def _transient(msg: str) -> bool:
    """Provider errors worth a retry: spending-limit 402s (they clear), 429, 5xx, connection errors."""
    m = msg.lower()
    return ("402" in m and "spending-limit" in m) or "rate limited" in m or "connection error" in m or \
        bool(re.search(r"api error 5\d\d", m)) or "overloaded" in m


class RoutedLLM(LLM):
    """Per model: ids Chalk's router serves go through Chalk; a `claude-*` id with no Anthropic provider
    behind Chalk goes to the Anthropic API directly. So a half-configured Chalk environment (say, a Groq
    key only) adds routes without taking the Claude route down.

    `route` is always the route the *default model* takes (`route_for(default_model)`), never a
    composite: a RoutedLLM bound to `claude-sonnet-5-5` on a Chalk environment with no Anthropic
    provider says "anthropic-direct", and the same backend bound to `grok-4.7` says "chalk". Every
    Completion carries the route of the backend that answered it. `primary_route` is the backend the
    process was configured around, for logs."""
    route = "chalk"

    def __init__(self, chalk: "ChalkLLM | None", anthropic: "AnthropicLLM | None", served: list[str], xai: "OpenAICompatLLM | None" = None,
                 default_model: str | None = None, vendors: dict | None = None):
        self.chalk, self.anthropic, self.xai, self.served = chalk, anthropic, xai, set(served)
        self.vendors = dict(vendors or {})
        self.default_model = default_model or (chalk.default_model if chalk else (anthropic.default_model if anthropic else DEFAULT_MODEL))
        self.primary_route = "chalk" if chalk is not None else ("anthropic-direct" if anthropic is not None else "xai-direct")
        self.route = self.route_for(self.default_model)                 # truthful for the bound model; see with_model

    def with_model(self, model: str | None) -> "LLM":
        other = super().with_model(model)
        if other is not self:
            other.route = self.route_for(other.default_model)
        return other

    @property
    def http(self):
        """The Chalk client's HTTP client (one per process; `with_model` copies share it)."""
        return self.chalk.http if self.chalk is not None else None

    def route_for(self, model: str) -> str:
        v = vendor_of(model)
        if self.chalk is not None and (model in self.served or self.vendors.get(v)):
            return "chalk"
        if v == "xai" and self.xai is not None:
            return "xai-direct"
        if v == "anthropic" and self.anthropic is not None:
            return "anthropic-direct"
        return "chalk" if self.chalk is not None else ("anthropic-direct" if self.anthropic is not None else "xai-direct")

    def complete(self, system: str, user: str, model: str | None = None, max_tokens: int = 4000) -> Completion:
        model = model or self.default_model
        r = self.route_for(model)
        target = {"chalk": self.chalk, "xai-direct": self.xai, "anthropic-direct": self.anthropic}[r]
        if target is None:
            raise LLMError(f"no route for model {model!r}")
        if r == "chalk" and vendor_of(model) == "xai" and not model.startswith("xai/"):
            model = "xai/" + model                       # Chalk names the vendor; xAI's own API does not
        # Transient provider answers (a spending-limit 402 that clears within a minute, 429, 5xx, a dropped
        # connection) are retried a few times with backoff before the caller sees an LLMError; the retry
        # count is logged so a run's record still shows the provider was flaky.
        delays = (5.0, 10.0, 20.0)
        for attempt in range(len(delays) + 1):
            try:
                return target.complete(system, user, model=model, max_tokens=max_tokens)
            except LLMError as e:
                if attempt == len(delays) or not _transient(str(e)):
                    raise
                log.warning("transient LLM error on %s via %s (attempt %d): %s; retrying in %.0fs", model, r, attempt + 1, str(e)[:160], delays[attempt])
                time.sleep(delays[attempt])
        raise LLMError("unreachable")

    def describe(self) -> str:
        return (f"chalk router for {sorted(self.served) or 'no models'}; Anthropic direct for claude-*"
                + ("; xAI direct for grok-*" if self.xai else "") + f" (default model {self.default_model} via {self.route})")


def route_of(llm: "LLM | None", model: str | None = None) -> str:
    """The route `llm` takes for `model` (its default model when None). For a RoutedLLM that is the
    per-model answer; for a single-backend LLM it is that backend's route; for nothing, "stub"."""
    if llm is None:
        return "stub"
    m = model or getattr(llm, "default_model", None)
    fn = getattr(llm, "route_for", None)
    if callable(fn) and m:
        try:
            return fn(m)
        except Exception:
            pass
    return getattr(llm, "route", None) or "unset"


def _xai_direct() -> "OpenAICompatLLM | None":
    k = os.environ.get("XAI_API_KEY")
    return OpenAICompatLLM(os.environ.get("XAI_BASE_URL", "https://api.x.ai/v1"), k, "xai-direct", os.environ.get("XAI_MODEL", "grok-4.20-0309-non-reasoning")) if k else None


_singleton: LLM | None = None


def get_llm(model: str | None = None) -> LLM:
    """The process-wide LLM. With `model`, the same backend bound to that default model."""
    global _singleton
    if _singleton is None:
        route, reason = planned_route()
        s = chalk_settings()
        if route == "chalk":
            chalk = ChalkLLM(api_key=s["key"], env_id=s["env_id"], base_url=s["base_url"])
            served = chalk_models() or []
            vendors = chalk_vendors() if os.environ.get("CHALK_SKIP_PROBE") != "1" else {}
            anth = None
            if os.environ.get("ANTHROPIC_API_KEY") or os.environ.get("ANTHROPIC_AUTH_TOKEN"):
                ws = os.environ.get("ANTHROPIC_WORKSPACE_ID")
                anth = AnthropicLLM(default_headers={"anthropic-workspace-id": ws} if ws else None)
            xai = _xai_direct()
            if (anth is not None or xai is not None) and (any(vendors.values()) or served):
                _singleton = RoutedLLM(chalk, anth, served, xai=xai, vendors=vendors)
                log.info("LLM route: Chalk Model Gateway for vendors %s; %s", [v for v, ok in vendors.items() if ok],
                         "claude-* through Chalk" if vendors.get("anthropic") else "claude-* to the Anthropic API directly")
            else:
                _singleton = chalk
                log.info("LLM route: Chalk Model Gateway at %s (env %s)", s["base_url"], s["env_id"])
        elif route == "anthropic-direct":
            log.warning("LLM route: Anthropic API directly (%s) - development only", reason)
            # A user-scoped key (sk-ant-usr-...) must name the workspace on every request.
            ws = os.environ.get("ANTHROPIC_WORKSPACE_ID")
            anth = AnthropicLLM(default_headers={"anthropic-workspace-id": ws} if ws else None)
            xai = _xai_direct()
            _singleton = RoutedLLM(None, anth, [], xai=xai, default_model=DEFAULT_MODEL) if xai else anth
            if xai:
                log.info("LLM route: grok-* ids go to xAI directly (XAI_API_KEY)")
        else:
            log.warning("LLM route: stub (%s)", reason)
            _singleton = StubLLM()
    return _singleton.with_model(model)


def set_llm(llm: LLM | None) -> None:
    global _singleton
    _singleton = llm


def extract_mpl(text: str) -> str:
    """The program is the first fenced block if there is one, else the text itself."""
    m = re.search(r"```(?:mpl)?\s*\n(.*?)```", text, re.S)
    body = m.group(1) if m else text
    lines = [l.rstrip() for l in body.strip().splitlines()]
    return "\n".join(lines) + "\n"


def extract_json(text: str) -> str:
    m = re.search(r"```(?:json)?\s*\n(.*?)```", text, re.S)
    if m:
        return m.group(1)
    m = re.search(r"\{.*\}", text, re.S)
    return m.group(0) if m else text
