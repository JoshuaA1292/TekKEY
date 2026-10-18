"""Test isolation: never let a test touch the live databases.

Every suite builds gateways, labs or race conductors; without these defaults a test that forgets to pass a path
writes into gateway/data/*.db, which is what the hosted page reads. Tests that set their own paths are unaffected.
"""
import os
import tempfile

_tmp = tempfile.mkdtemp(prefix="bk-tests-")
for var, name in (("GATEWAY_DB", "gateway.db"), ("RACE_DB", "race.db"), ("BACKTEST_DB", "race-backtest.db"), ("LAB_DB", "lab.db")):
    os.environ.setdefault(var, os.path.join(_tmp, name))
os.environ.setdefault("LAB_SCHEDULE_FIRST_S", "36000")
os.environ.setdefault("BACKTEST_AUTOSTART", "0")
os.environ.setdefault("RACE_TAPE", os.environ.get("RACE_TAPE", "replay"))
