"""One error type for every refusal in BOUNDED_KEYS.md §10.4."""
from __future__ import annotations

from typing import Any


class ApiError(Exception):
    def __init__(self, status: int, code: str, **extra: Any):
        super().__init__(f"{status} {code}")
        self.status = status
        self.code = code
        self.extra = extra

    def body(self) -> dict:
        return {"error": self.code, **self.extra}
