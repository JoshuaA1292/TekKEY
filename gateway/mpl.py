"""A Python MPL front end for the *fake* venue only (BOUNDED_KEYS.md §6, §7, CONTRACT.md §2.2, §3).

Build A owns the real `policyc`. This module exists so B-agents never wait on Build A: it parses
the grammar, applies the static rules, and emits a CompiledProgram with the same shape. With
VENUE=real none of this code runs.
"""
from __future__ import annotations

import hashlib
import json
import math
import re
from dataclasses import dataclass, field

MAX_NODES = 64
MAX_INSTRUMENTS = 3

TOKEN_RE = re.compile(r"[ \t\r]*(?:(#[^\n]*)|(\n)|(<=|>=|[:,+\-])|(\d+)|([A-Za-z_][A-Za-z0-9_]*)|(.))")


class StaticError(Exception):
    def __init__(self, line: int, col: int, message: str):
        super().__init__(f"line {line}: {message}")
        self.line, self.col, self.message = line, col, message

    def body(self) -> dict:
        return {"contract_version": 1, "status": "static_error", "line": self.line,
                "col": self.col, "message": self.message}


@dataclass
class Tok:
    kind: str   # ident | int | punct | nl | eof
    text: str
    line: int
    col: int


def tokenize(src: str) -> list[Tok]:
    toks: list[Tok] = []
    line, line_start = 1, 0
    pos = 0
    while pos < len(src):
        m = TOKEN_RE.match(src, pos)
        if not m or m.end() == pos:
            break
        pos = m.end()
        col = m.start(m.lastindex) - line_start + 1 if m.lastindex else 1
        if m.group(1):
            continue
        if m.group(2):
            toks.append(Tok("nl", "\n", line, col))
            line += 1
            line_start = pos
        elif m.group(3):
            toks.append(Tok("punct", m.group(3), line, col))
        elif m.group(4):
            toks.append(Tok("int", m.group(4), line, col))
        elif m.group(5):
            toks.append(Tok("ident", m.group(5), line, col))
        else:
            raise StaticError(line, col, f"unexpected character {m.group(6)!r}")
    toks.append(Tok("eof", "", line, 1))
    return toks


@dataclass
class Node:
    id: str
    action: str                 # exit_limit | exit_market | take | scale | hedge | halt
    line: int
    qty: object = None          # int | "all" | "half" | "position"
    price: object = None        # ("abs", n) | ("rel", +-ticks) | ("bps", +-bps)
    allow: int | None = None
    side: str | None = None
    instrument: str | None = None
    cond: tuple | None = None   # ("price", cmp, price_expr) | ("fill", ident) | ("position", cmp, qty_expr)
    after: str | None = None
    oco: list[str] = field(default_factory=list)
    rearm: int = 0


@dataclass
class Program:
    instruments: list[str]
    entry_side: str
    entry_qty: int
    entry_price: tuple
    nodes: list[Node]
    header_line: int = 1


class Parser:
    def __init__(self, src: str):
        self.toks = tokenize(src)
        self.i = 0

    def peek(self) -> Tok:
        return self.toks[self.i]

    def next(self) -> Tok:
        t = self.toks[self.i]
        self.i += 1
        return t

    def skip_nl(self):
        while self.peek().kind == "nl":
            self.i += 1

    def expect_ident(self, word: str | None = None) -> Tok:
        t = self.next()
        if t.kind != "ident" or (word and t.text != word):
            raise StaticError(t.line, t.col, f"expected {word or 'identifier'}, got {t.text!r}")
        return t

    def expect_int(self) -> int:
        t = self.next()
        if t.kind != "int":
            raise StaticError(t.line, t.col, f"expected integer, got {t.text!r}")
        return int(t.text)

    def accept(self, kind: str, text: str | None = None) -> Tok | None:
        t = self.peek()
        if t.kind == kind and (text is None or t.text == text):
            return self.next()
        return None

    def price_expr(self) -> tuple:
        t = self.next()
        if t.kind == "int":
            return ("abs", int(t.text))
        if t.kind == "ident" and t.text == "entry":
            sign_t = self.next()
            if sign_t.kind != "punct" or sign_t.text not in "+-":
                raise StaticError(sign_t.line, sign_t.col, "expected + or - after entry")
            n = self.expect_int()
            sign = 1 if sign_t.text == "+" else -1
            if self.accept("ident", "bps"):
                return ("bps", sign * n)
            return ("rel", sign * n)
        raise StaticError(t.line, t.col, f"expected price, got {t.text!r}")

    def qty_expr(self):
        t = self.next()
        if t.kind == "int":
            return int(t.text)
        if t.kind == "ident" and t.text in ("all", "half", "position"):
            return t.text
        raise StaticError(t.line, t.col, f"expected quantity, got {t.text!r}")

    def side(self) -> str:
        t = self.expect_ident()
        if t.text not in ("buy", "sell"):
            raise StaticError(t.line, t.col, f"expected buy or sell, got {t.text!r}")
        return t.text

    def parse(self) -> Program:
        self.skip_nl()
        h = self.expect_ident("instrument")
        instruments = [self.expect_ident().text]
        while self.accept("punct", ","):
            instruments.append(self.expect_ident().text)
        self.skip_nl()
        self.expect_ident("entry")
        side = self.side()
        qty = self.expect_int()
        self.expect_ident("at")
        price = self.price_expr()
        nodes: list[Node] = []
        while True:
            self.skip_nl()
            if self.peek().kind == "eof":
                break
            nodes.append(self.node())
        return Program(instruments, side, qty, price, nodes, h.line)

    def node(self) -> Node:
        name = self.expect_ident()
        t = self.next()
        if not (t.kind == "punct" and t.text == ":"):
            raise StaticError(t.line, t.col, f"expected ':' after node name {name.text!r}")
        act = self.expect_ident()
        n = Node(name.text, "", name.line)
        if act.text == "exit":
            n.qty = self.qty_expr()
            k = self.expect_ident()
            if k.text == "market":
                n.action = "exit_market"
            elif k.text == "at":
                n.action = "exit_limit"
                n.price = self.price_expr()
                if self.accept("ident", "allow"):
                    n.allow = self.expect_int()
                if n.allow is None or n.allow < 1:
                    raise StaticError(name.line, name.col, "exit requires allow >= 1 tick")
            else:
                raise StaticError(k.line, k.col, f"expected 'at' or 'market', got {k.text!r}")
        elif act.text == "take":
            n.action = "take"
            n.qty = self.qty_expr()
            self.expect_ident("at")
            n.price = self.price_expr()
        elif act.text == "scale":
            n.action = "scale"
            n.side = self.side()
            n.qty = self.expect_int()
            self.expect_ident("at")
            n.price = self.price_expr()
        elif act.text == "hedge":
            n.action = "hedge"
            n.instrument = self.expect_ident().text
            n.side = self.side()
            n.qty = self.expect_int()
            self.expect_ident("at")
            n.price = self.price_expr()
        elif act.text == "halt":
            n.action = "halt"
        else:
            raise StaticError(act.line, act.col, f"unknown action {act.text!r}")
        # clauses, any order
        while True:
            t = self.peek()
            if t.kind != "ident":
                break
            if t.text == "when":
                self.next()
                c = self.expect_ident()
                if c.text == "price":
                    cmp_t = self.next()
                    if cmp_t.text not in ("<=", ">="):
                        raise StaticError(cmp_t.line, cmp_t.col, "expected <= or >=")
                    n.cond = ("price", cmp_t.text, self.price_expr())
                elif c.text == "fill":
                    n.cond = ("fill", self.expect_ident().text)
                elif c.text == "position":
                    cmp_t = self.next()
                    if cmp_t.text not in ("<=", ">="):
                        raise StaticError(cmp_t.line, cmp_t.col, "expected <= or >=")
                    n.cond = ("position", cmp_t.text, self.qty_expr())
                else:
                    raise StaticError(c.line, c.col, f"unknown condition {c.text!r}")
            elif t.text == "after":
                self.next()
                n.after = self.expect_ident().text
            elif t.text == "oco":
                self.next()
                n.oco.append(self.expect_ident().text)
                while self.accept("punct", ","):
                    n.oco.append(self.expect_ident().text)
            elif t.text == "rearm":
                self.next()
                n.rearm = self.expect_int()
            else:
                raise StaticError(t.line, t.col, f"unexpected {t.text!r}")
        t = self.peek()
        if t.kind not in ("nl", "eof"):
            raise StaticError(t.line, t.col, f"unexpected {t.text!r}")
        return n


# ---------------------------------------------------------------------------------------------
# Compilation (§7)

def source_hash(src: str) -> str:
    return "sha256:" + hashlib.sha256(src.encode()).hexdigest()


def resolve_price(expr: tuple, entry: int) -> int:
    kind, n = expr
    if kind == "abs":
        return n
    if kind == "rel":
        return entry + n
    return entry + int(round(entry * n / 10000))


def opposite(side: str) -> str:
    return "sell" if side == "buy" else "buy"


def compile_program(src: str, instruments_cfg: dict, env: dict | None = None) -> dict:
    """MPL -> CompiledProgram (CONTRACT.md §2.2). Raises StaticError (exit 2 in policyc terms)."""
    env = env or {}
    prog = Parser(src).parse()
    if len(set(prog.instruments)) > MAX_INSTRUMENTS:
        raise StaticError(prog.header_line, 1, "at most 3 instruments")
    for ins in prog.instruments:
        if ins not in instruments_cfg:
            raise StaticError(prog.header_line, 1, f"unknown instrument {ins}")
    main_ins = prog.instruments[0]
    tick = int(instruments_cfg[main_ins]["tick_cents"])
    if prog.entry_qty < 1:
        raise StaticError(prog.header_line, 1, "entry quantity must be >= 1 lot")
    entry_price = resolve_price(prog.entry_price, 0) if prog.entry_price[0] == "abs" else None
    if entry_price is None:
        raise StaticError(prog.header_line, 1, "entry price must be absolute")
    if entry_price < 1:
        raise StaticError(prog.header_line, 1, "entry price must be >= 1 tick")

    ids = {"entry"}
    for n in prog.nodes:
        if n.id in ids:
            raise StaticError(n.line, 1, f"duplicate node {n.id!r}")
        ids.add(n.id)
    for n in prog.nodes:
        for ref in ([n.after] if n.after else []) + n.oco + ([n.cond[1]] if n.cond and n.cond[0] == "fill" else []):
            if ref not in ids:
                raise StaticError(n.line, 1, f"unknown node {ref!r}")
        if n.action == "hedge" and n.instrument not in prog.instruments:
            raise StaticError(n.line, 1, f"hedge instrument {n.instrument} not declared in header")
        if isinstance(n.qty, int) and n.qty < 1:
            raise StaticError(n.line, 1, "quantity must be >= 1 lot")
        if n.rearm < 0:
            raise StaticError(n.line, 1, "rearm count must be >= 0")

    # Maximum reachable position: entry plus every same-side scale (reduce-only exits can never overshoot).
    max_pos = prog.entry_qty + sum(n.qty for n in prog.nodes if n.action == "scale" and n.side == prog.entry_side)
    def resolve_qty(q):
        if q == "all" or q == "position":
            return max_pos
        if q == "half":
            return max(1, max_pos // 2)
        return int(q)

    # OCO groups, labelled g1, g2, ... in order of first appearance.
    group_of: dict[str, str] = {}
    next_g = 1
    for n in prog.nodes:
        if not n.oco:
            continue
        members = [n.id] + n.oco
        existing = [group_of[m] for m in members if m in group_of]
        label = existing[0] if existing else f"g{next_g}"
        if not existing:
            next_g += 1
        for m in members:
            group_of[m] = label
    # every group needs >= 2 members (guaranteed by construction: a mention adds two ids)

    warnings: list[str] = []
    max_gap = int(env.get("max_gap_ticks", 0) or 0)
    out_nodes: list[dict] = []
    out_nodes.append({
        "id": "entry", "kind": "limit", "instrument": main_ins, "side": prog.entry_side,
        "qty": prog.entry_qty, "price": entry_price, "trigger": None, "reduce_only": False,
        "parent": None, "oco": None, "terminal": False,
    })
    exit_side = opposite(prog.entry_side)
    for n in prog.nodes:
        parent = n.after or (n.cond[1] if n.cond and n.cond[0] == "fill" else None)
        if parent is None and n.action in ("exit_limit", "exit_market", "take", "halt"):
            parent = "entry"
        if n.cond and n.cond[0] == "position" and parent is None:
            parent = "entry"
        trigger = None
        if n.cond and n.cond[0] == "price":
            trigger = resolve_price(n.cond[2], entry_price)
        base = {"id": n.id, "kind": None, "instrument": main_ins, "side": exit_side, "qty": None,
                "price": None, "trigger": trigger, "reduce_only": False, "parent": parent,
                "oco": group_of.get(n.id), "terminal": False}
        if n.action == "exit_limit":
            at = resolve_price(n.price, entry_price)
            limit = at - n.allow if exit_side == "sell" else at + n.allow
            if trigger is None:
                trigger = at
            if max_gap and n.allow < max_gap:
                warnings.append(f"line {n.line}: {n.id}: allow {n.allow} < max_gap_ticks {max_gap}; "
                                "the verifier will almost certainly find a gap path")
            base.update(kind="stop_limit", qty=resolve_qty(n.qty), price=limit, trigger=trigger, reduce_only=True)
        elif n.action == "exit_market":
            base.update(kind="stop_market", qty=resolve_qty(n.qty), price=None, trigger=trigger, reduce_only=True)
        elif n.action == "take":
            base.update(kind="limit", qty=resolve_qty(n.qty), price=resolve_price(n.price, entry_price),
                        trigger=trigger, reduce_only=True)
        elif n.action == "scale":
            base.update(kind="limit", side=n.side, qty=n.qty, price=resolve_price(n.price, entry_price),
                        trigger=trigger, reduce_only=False)
        elif n.action == "hedge":
            base.update(kind="limit", instrument=n.instrument, side=n.side, qty=n.qty,
                        price=resolve_price(n.price, entry_price), trigger=trigger, reduce_only=False)
        elif n.action == "halt":
            base.update(kind="halt", qty=0, price=None, trigger=trigger, terminal=True)
        if base.get("price") is not None and base["price"] < 1:
            raise StaticError(n.line, 1, f"{n.id}: price resolves below 1 tick")
        base["allow"] = n.allow
        out_nodes.append(base)
        # rearm K: K chained copies, each a child of the previous copy (approximation of §7's wording).
        prev = n.id
        for k in range(1, n.rearm + 1):
            copy = dict(base, id=f"{n.id}#{k}", parent=prev, oco=None)
            out_nodes.append(copy)
            prev = copy["id"]

    if len(out_nodes) > MAX_NODES:
        raise StaticError(prog.header_line, 1, f"{len(out_nodes)} nodes after expansion; limit is {MAX_NODES}")

    # reachability / cycles
    by_id = {n["id"]: n for n in out_nodes}
    for n in out_nodes:
        seen = set()
        p = n["parent"]
        while p is not None:
            if p in seen:
                raise StaticError(1, 1, f"node {n['id']} is in a parent cycle")
            seen.add(p)
            p = by_id[p]["parent"]

    # deploy plan: topological generations, parents before children
    depth = {}
    def d(nid):
        if nid in depth:
            return depth[nid]
        p = by_id[nid]["parent"]
        depth[nid] = 0 if p is None else d(p) + 1
        return depth[nid]
    gens: dict[int, list[str]] = {}
    for n in out_nodes:
        gens.setdefault(d(n["id"]), []).append(n["id"])
    deploy_plan = [gens[k] for k in sorted(gens)]

    locked = 0
    for n in out_nodes:
        if n["kind"] == "limit" and not n["reduce_only"] and n["trigger"] is None:
            t = int(instruments_cfg.get(n["instrument"], {"tick_cents": tick})["tick_cents"])
            locked += n["price"] * n["qty"] * t
    native = sum(1 for n in out_nodes if n["kind"] != "halt")
    for n in out_nodes:
        n.pop("allow", None)
    return {
        "contract_version": 1,
        "backend": "minsky",
        "source_hash": source_hash(src),
        "instruments": list(dict.fromkeys(prog.instruments)),
        "nodes": out_nodes,
        "deploy_plan": deploy_plan,
        "report": {"nodes_total": len(out_nodes), "native": native, "relayed": 0,
                   "locked_collateral_cents": locked},
        "warnings": warnings,
    }


def compiled_json(compiled: dict) -> str:
    """Deterministic bytes: the same input gives byte-identical output."""
    return json.dumps(compiled, sort_keys=True, separators=(",", ":"))
