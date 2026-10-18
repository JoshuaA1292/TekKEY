"""A small client library over httpx for the bounded-key gateway (BOUNDED_KEYS.md §10).

What is real: every method is a real HTTP request to the gateway. The same code talks to a live
URL (`GatewayClient("https://gw.example", ak)`), to an in-process FastAPI app through
`transport=httpx.ASGITransport(app=app)`, or through an existing `httpx.Client` such as
`fastapi.testclient.TestClient`. Nothing in this module is simulated.

What is deliberately missing: `GatewayClient` has no deploy, pause or cancel. Those need a
program key (`bk_`), and the delegate is only ever handed a `GatewayClient`. `ProgramKeyClient`
exists for the owner's tooling and the evals, not for the delegate.
"""
from __future__ import annotations

import asyncio
import threading
from typing import Any

import httpx

JsonDict = dict[str, Any]


class GatewayError(Exception):
    """A non-2xx answer on a route where only one outcome is expected (GET routes, bk_ routes)."""

    def __init__(self, status: int, body: JsonDict):
        super().__init__(f"{status} {body.get('error', body)}")
        self.status = status
        self.body = body


class _AsyncBridge:
    """Drives an `httpx.AsyncClient` from synchronous code. `httpx.ASGITransport` is async-only,
    so an in-process app needs this. A dedicated thread owns the event loop, which keeps it safe
    to call from inside another running loop (pytest-asyncio, FastAPI handlers)."""

    def __init__(self, base_url: str, transport: httpx.AsyncBaseTransport, timeout: float):
        self._loop = asyncio.new_event_loop()
        self._thread = threading.Thread(target=self._loop.run_forever, name="gateway-client-loop", daemon=True)
        self._thread.start()
        self._client = httpx.AsyncClient(base_url=base_url, transport=transport, timeout=timeout)

    def request(self, method: str, url: str, **kw) -> httpx.Response:
        fut = asyncio.run_coroutine_threadsafe(self._client.request(method, url, **kw), self._loop)
        return fut.result()

    def close(self) -> None:
        asyncio.run_coroutine_threadsafe(self._client.aclose(), self._loop).result(timeout=5)
        self._loop.call_soon_threadsafe(self._loop.stop)


class _Http:
    """One place that knows which of the three transports is in use."""

    def __init__(self, base_url: str, key: str, *, transport=None, client: httpx.Client | None = None,
                 timeout: float = 60.0):
        self.base_url = (base_url or "").rstrip("/")
        self._key = key
        self._bridge: _AsyncBridge | None = None
        self._owned = client is None
        if client is not None:
            self._sync: httpx.Client | None = client
        elif isinstance(transport, httpx.AsyncBaseTransport):
            self._sync = None
            self._bridge = _AsyncBridge(self.base_url or "http://gateway", transport, timeout)
        else:
            self._sync = httpx.Client(base_url=self.base_url, transport=transport, timeout=timeout)

    def _url(self, path: str) -> str:
        if self._sync is not None and not self._owned and not str(self._sync.base_url) and self.base_url:
            return self.base_url + path
        return path

    def request(self, method: str, path: str, json: JsonDict | None = None) -> tuple[int, JsonDict]:
        headers = {"Authorization": f"Bearer {self._key}"}
        if self._bridge is not None:
            r = self._bridge.request(method, path, json=json, headers=headers)
        else:
            assert self._sync is not None
            r = self._sync.request(method, self._url(path), json=json, headers=headers)
        try:
            body = r.json()
        except ValueError:
            body = {"error": "non_json", "status_code": r.status_code, "text": r.text[:500]}
        if not isinstance(body, dict):
            body = {"value": body}
        return r.status_code, body

    def expect_ok(self, method: str, path: str, json: JsonDict | None = None) -> JsonDict:
        status, body = self.request(method, path, json)
        if status // 100 != 2:
            raise GatewayError(status, body)
        return body

    def close(self) -> None:
        if self._bridge is not None:
            self._bridge.close()
        elif self._owned and self._sync is not None:
            self._sync.close()


class GatewayClient:
    """Author-key (`ak_`) client: BOUNDED_KEYS.md §10.2 plus the gateway's repair route.

    `submit` and `repair` return `(http_status, body)` because 200, 400, 409 and 422 are all
    legitimate outcomes the caller must branch on. The GET routes return the body and raise
    `GatewayError` otherwise.
    """

    def __init__(self, base_url: str, author_key: str, *, transport=None, client: httpx.Client | None = None,
                 timeout: float = 60.0):
        if not isinstance(author_key, str) or not author_key.startswith("ak_"):
            raise ValueError("GatewayClient takes an author key (ak_...); it never holds an owner or program key")
        self.key_class = "ak"
        self._http = _Http(base_url, author_key, transport=transport, client=client, timeout=timeout)

    def submit(self, account: int, source: str, backend: str = "minsky") -> tuple[int, JsonDict]:
        """POST /program -> 200 certificate + key, 422 counterexample, 409 budget or instrument busy,
        400 static error."""
        return self._http.request("POST", "/program", {"account": int(account), "backend": backend, "source": source})

    def get_program(self, program_id: int) -> JsonDict:
        return self._http.expect_ok("GET", f"/program/{int(program_id)}")

    def counterexample(self, program_id: int) -> JsonDict:
        return self._http.expect_ok("GET", f"/program/{int(program_id)}/counterexample")

    def repair(self, program_id: int) -> tuple[int, JsonDict]:
        """POST /program/{id}/repair: the gateway runs the Droid for up to 3 rounds and resubmits.
        Returns the last submission's (status, body) with a `repair` section of incident reports."""
        return self._http.request("POST", f"/program/{int(program_id)}/repair")

    def close(self) -> None:
        self._http.close()

    def __enter__(self) -> "GatewayClient":
        return self

    def __exit__(self, *exc) -> None:
        self.close()


class ProgramKeyClient:
    """Program-key (`bk_`) client: BOUNDED_KEYS.md §10.3. One certified program, once."""

    def __init__(self, base_url: str, bk_key: str, *, transport=None, client: httpx.Client | None = None,
                 timeout: float = 60.0):
        if not isinstance(bk_key, str) or not bk_key.startswith("bk_"):
            raise ValueError("ProgramKeyClient takes a program key (bk_...)")
        self.key_class = "bk"
        self._http = _Http(base_url, bk_key, transport=transport, client=client, timeout=timeout)

    def deploy(self, program_id: int) -> JsonDict:
        return self._http.expect_ok("POST", f"/program/{int(program_id)}/deploy")

    def pause(self, program_id: int) -> JsonDict:
        return self._http.expect_ok("POST", f"/program/{int(program_id)}/pause")

    def cancel(self, program_id: int) -> JsonDict:
        return self._http.expect_ok("POST", f"/program/{int(program_id)}/cancel")

    def state(self, program_id: int) -> JsonDict:
        return self._http.expect_ok("GET", f"/program/{int(program_id)}/state")

    def close(self) -> None:
        self._http.close()

    def __enter__(self) -> "ProgramKeyClient":
        return self

    def __exit__(self, *exc) -> None:
        self.close()
