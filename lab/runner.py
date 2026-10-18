"""The lab runner: one worker thread and a queue, so the venue is never shared between two
tests; a per-visitor rate limit; and the scheduler that re-runs the venue-only tests on the
fresh tape every `LAB_SCHEDULE_MIN` minutes (default 30), and the model tests too when
`LAB_AUTO_MODELS=1`.
"""
from __future__ import annotations

import logging
import os
import queue
import threading
import time
import traceback
from collections import deque
from typing import Callable

from . import tests as registry
from .context import Context
from .store import LabStore

log = logging.getLogger("lab.runner")

RATE_LIMIT_RUNS = 3
RATE_LIMIT_WINDOW_S = 600


class LabError(Exception):
    def __init__(self, status: int, code: str, **extra):
        super().__init__(f"{status} {code}")
        self.status, self.code, self.extra = status, code, extra

    def body(self) -> dict:
        return {"error": self.code, "detail": self.extra.get("detail", self.code), **{k: v for k, v in self.extra.items() if k != "detail"}}


class Runner:
    def __init__(self, store: LabStore, make_context: Callable[[dict], Context], models: list[str],
                 schedule_min: float | None = None, auto_models: bool | None = None, first_delay_s: float | None = None):
        self.store = store
        self.make_context = make_context
        self.models = list(models)
        self.schedule_min = float(os.environ.get("LAB_SCHEDULE_MIN", "30")) if schedule_min is None else float(schedule_min)
        self.auto_models = (os.environ.get("LAB_AUTO_MODELS") == "1") if auto_models is None else bool(auto_models)
        self.first_delay_s = float(os.environ.get("LAB_SCHEDULE_FIRST_S", "20")) if first_delay_s is None else float(first_delay_s)
        self.q: queue.Queue[str] = queue.Queue()
        self.running: str | None = None
        self._visitors: dict[str, deque] = {}
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._go = threading.Event()
        self._go.set()
        self._worker: threading.Thread | None = None
        self._sched: threading.Thread | None = None
        self.done = threading.Condition()

    # ------------------------------------------------------------------ lifecycle
    def start(self, schedule: bool = False) -> None:
        with self._lock:
            if self._worker is None:
                stale = self.store.requeue_stale()
                if stale:
                    log.warning("lab: %d run(s) left pending by a previous process marked error", len(stale))
                self._worker = threading.Thread(target=self._work, name="lab-worker", daemon=True)
                self._worker.start()
            if schedule and self._sched is None and self.schedule_min > 0:
                self._sched = threading.Thread(target=self._schedule, name="lab-scheduler", daemon=True)
                self._sched.start()

    def stop(self) -> None:
        self._stop.set()

    def hold(self) -> None:
        """Tests: keep the worker from taking the next run."""
        self._go.clear()

    def release(self) -> None:
        self._go.set()

    def position(self) -> int:
        """How many runs are ahead of a new one (the running one counts)."""
        return self.q.qsize() + (1 if self.running else 0)

    def position_of(self, run_id: str) -> int | None:
        """Where a pending run stands: 0 while it runs, else 1 + the queued runs ahead of it (the
        running one counts). None when this process does not hold it (left over from a restart)."""
        if run_id == self.running:
            return 0
        with self.q.mutex:
            waiting = list(self.q.queue)
        if run_id not in waiting:
            return None
        return waiting.index(run_id) + 1 + (1 if self.running else 0)

    # ------------------------------------------------------------------ submit
    def submit(self, test: str, model: str, params: dict | None = None, visitor: str | None = None,
               source: str = "visitor") -> tuple[dict, int]:
        if test not in registry.SPECS:
            raise LabError(404, "unknown_test", detail=f"unknown test {test!r}", tests=list(registry.ORDER))
        if not isinstance(model, str) or not model.strip():
            raise LabError(400, "bad_model", detail="model must be a non-empty string")
        if params is not None and not isinstance(params, dict):
            raise LabError(400, "bad_params", detail="params must be an object")
        if params and test != "injection" and "message" in params:
            raise LabError(400, "bad_params", detail="params.message is only accepted by the injection test (Try it)")
        with self._lock:
            if self.store.pending(test, model) is not None:
                raise LabError(429, "already_queued", detail=f"a run of {test} for {model} is already queued or running",
                               test=test, model=model)
            if source == "visitor" and visitor:
                now = time.time()
                hist = self._visitors.setdefault(visitor, deque())
                while hist and now - hist[0] > RATE_LIMIT_WINDOW_S:
                    hist.popleft()
                if len(hist) >= RATE_LIMIT_RUNS:
                    raise LabError(429, "rate_limited", detail=f"{RATE_LIMIT_RUNS} runs per {RATE_LIMIT_WINDOW_S // 60} minutes per visitor",
                                   retry_after_s=int(RATE_LIMIT_WINDOW_S - (now - hist[0])))
                hist.append(now)
            position = self.position()
            route = None if registry.SPECS[test]["model_test"] else "none"
            run = self.store.create_run(test, model, route, None, params, visitor, source, position=position)
            self.q.put(run["run_id"])
        return run, position

    def wait_idle(self, timeout_s: float = 300) -> bool:
        """Tests: block until the queue is drained and nothing is running."""
        deadline = time.time() + timeout_s
        with self.done:
            while (self.running is not None or not self.q.empty()) and time.time() < deadline:
                self.done.wait(timeout=0.1)
        return self.running is None and self.q.empty()

    # ------------------------------------------------------------------ the worker
    def _work(self) -> None:
        while not self._stop.is_set():
            if not self._go.wait(timeout=0.5):
                continue
            try:
                run_id = self.q.get(timeout=0.5)
            except queue.Empty:
                continue
            self.running = run_id
            while not self._go.wait(timeout=0.5):        # hold() called between the gate and the take: wait here
                if self._stop.is_set():
                    return
            try:
                self.run_now(run_id)
            except Exception:
                log.exception("lab run %s crashed outside the test", run_id)
            finally:
                self.running = None
                with self.done:
                    self.done.notify_all()

    def run_now(self, run_id: str) -> dict:
        run = self.store.get_run(run_id)
        test = registry.load(run["test"])
        ctx = None
        try:
            ctx = self.make_context(run)
            self.store.start_run(run_id, ctx.route, ctx.venue.name)
            t0 = time.perf_counter()
            result = test.run(ctx)
            status = result.status or ("skipped" if result.skipped_reason else "failed")
            if result.headline is None and status not in ("skipped",):
                result.headline = {"label": registry.SPECS[run["test"]]["headline"], "value": None, "unit": "", "pass": None}
            body = result.as_dict()
            body["stub"] = ctx.stub if registry.SPECS[run["test"]]["model_test"] else False
            body["elapsed_s"] = round(time.perf_counter() - t0, 3)
        except Exception as e:
            log.exception("lab run %s (%s) errored", run_id, run["test"])
            if ctx is None:
                self.store.start_run(run_id, None, None)
            status = "error"
            body = (ctx.result.as_dict() if ctx else {"log": [], "metrics": {}, "rows": [], "series": {}, "evidence": {}})
            body["headline"] = {"label": registry.SPECS[run["test"]]["headline"], "value": None, "unit": "", "pass": False}
            body["note"] = f"{type(e).__name__}: {e}"
            body["traceback"] = traceback.format_exc()[-4000:]
        finally:
            if ctx is not None:
                ctx.close()
        return self.store.finish_run(run_id, status, body)

    # ------------------------------------------------------------------ the scheduler
    def _schedule(self) -> None:
        if self._stop.wait(self.first_delay_s):
            return
        while not self._stop.is_set():
            model = self.models[0] if self.models else "claude-opus-5-5"
            for t in registry.VENUE_TESTS:
                self._enqueue_quietly(t, model)
            if self.auto_models:
                for m in self.models:
                    for t in registry.MODEL_TESTS:
                        self._enqueue_quietly(t, m)
            if self._stop.wait(self.schedule_min * 60):
                return

    def _enqueue_quietly(self, test: str, model: str) -> None:
        try:
            self.submit(test, model, source="schedule")
        except LabError as e:
            log.info("scheduler: %s %s not queued (%s)", test, model, e.code)
        except Exception:
            log.exception("scheduler: could not queue %s", test)
