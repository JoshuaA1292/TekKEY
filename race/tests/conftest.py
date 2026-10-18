"""A throwaway exchange (and venued) on free ports for the race's tests. Build A's binaries must
exist in minsky/build; otherwise the tests skip."""
from __future__ import annotations

import json
import os
import socket
import subprocess
import tempfile
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "minsky" / "build"


def free_port() -> int:
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def _wait_port(port: int, timeout: float = 5.0) -> None:
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError(f"port {port} never opened")


@pytest.fixture
def exchange():
    if not (BUILD / "exchange").exists() or not (BUILD / "venued").exists():
        pytest.skip("Build A binaries missing (minsky/build)")
    port, feed, vport = free_port(), free_port(), free_port()
    d = tempfile.mkdtemp(prefix="race-test-")
    ex = subprocess.Popen([str(BUILD / "exchange"), "--tcp", str(port), "--feed", str(feed), "--accounts", "255", "--quiet"],
                          stdout=open(os.path.join(d, "exchange.log"), "w"), stderr=subprocess.STDOUT)
    _wait_port(port)
    vd = subprocess.Popen([str(BUILD / "venued"), "--listen", str(vport), "--exchange", f"tcp://127.0.0.1:{port}",
                           "--state", os.path.join(d, "venued.state.json"), "--instruments", str(ROOT / "fixtures" / "instruments.json"),
                           "--quiet"], stdout=open(os.path.join(d, "venued.log"), "w"), stderr=subprocess.STDOUT)
    _wait_port(vport)
    try:
        yield {"url": f"tcp://127.0.0.1:{port}", "venued_port": vport, "dir": d}
    finally:
        for p in (vd, ex):
            p.terminate()
        for p in (vd, ex):
            try:
                p.wait(timeout=3)
            except subprocess.TimeoutExpired:
                p.kill()
