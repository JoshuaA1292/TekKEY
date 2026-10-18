/* The race page. Vanilla JS, no build. Reads /race/state once, then follows /race/stream. */
(() => {
"use strict";

const $ = (s, el = document) => el.querySelector(s);
const el = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; return e; };
const clear = (n) => { while (n.firstChild) n.removeChild(n.firstChild); return n; };

const MODE = new URLSearchParams(location.search).get("mode") === "backtest" ? "backtest" : "race";
const API = "/" + MODE;
const S = {
  st: null, cfg: null, marketNow: null, prints: [], equity: [], fills: [], rounds: new Map(), raw: null, bnd: null,
  es: null, lastId: 0, lastRoundT: 0, inRound: false, roundN: 0, pending: 0, route: "stub", model: "",
  tweens: {}, chartTimer: null, prevPx: null, feed: null, epoch: null,
};

// ---------------------------------------------------------------- formatting
const fmt = {
  viol(v, violates) { return v == null ? "" : /cents$/.test(violates || "") ? fmt.usd(v) : String(v) + (/exposure/.test(violates || "") ? " lots" : /trades/.test(violates || "") ? " trades" : ""); },
  usd(c, sign = false) {
    if (c == null || isNaN(c)) return "—";
    const neg = c < 0; c = Math.abs(Math.round(c));
    const d = Math.floor(c / 100), r = String(c % 100).padStart(2, "0");
    return (neg ? "−" : (sign ? "+" : "")) + "$" + d.toLocaleString("en-US") + "." + r;
  },
  usd0(c) { return (c < 0 ? "−" : "") + "$" + Math.abs(Math.round(c / 100)).toLocaleString("en-US"); },
  clock(t) { return new Date(t).toISOString().slice(11, 19) + "Z"; },
  hm(t) { return new Date(t).toISOString().slice(11, 16); },
  dur(ms) { ms = Math.max(0, ms); const s = Math.floor(ms / 1000), h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60); return h ? `${h}h ${m}m` : m ? `${m}m ${s % 60}s` : `${s}s`; },
  rel(t) { const s = (Date.now() - t) / 1000; return s < 60 ? "just now" : s < 3600 ? Math.round(s / 60) + " min ago" : Math.round(s / 3600) + " h ago"; },
  lots(n) { return (n > 0 ? "+" : "") + n + (Math.abs(n) === 1 ? " lot" : " lots"); },
};

// ---------------------------------------------------------------- routing (truthful labels)
// The gateway routes grok-* ids through the Chalk Model Gateway and claude-* ids to the Anthropic API directly
// (there is no Anthropic provider behind Chalk); some state fields still report the composite route as "chalk".
function routeFor(model, route) {
  if (route === "stub" || !model) return route || "stub";
  if (/^claude-/.test(model)) return "anthropic-direct";
  if (/^grok-/.test(model)) return "chalk";
  return route || "—";
}
function routeName(route) { return { chalk: "the Chalk Model Gateway", "anthropic-direct": "the Anthropic API directly", stub: "a rule-based stand-in (no model)" }[route] || route; }
function modelVia(model, route) { const r = routeFor(model, route); return (model || "no model") + " via " + r; }

// ---------------------------------------------------------------- api
async function get(path) {
  const r = await fetch(path, { headers: { accept: "application/json" } });
  const body = await r.json().catch(() => ({}));
  if (!r.ok) { const e = new Error(body.detail || body.error || (path + " → " + r.status)); e.status = r.status; e.body = body; throw e; }
  return body;
}

// ---------------------------------------------------------------- counters
function setMoney(id, cents, cls) {
  const node = $(id); const s = fmt.usd(cents); const i = s.lastIndexOf(".");
  clear(node); node.append(document.createTextNode(s.slice(0, i))); node.append(el("span", "c", s.slice(i)));
  node.classList.remove("up", "down"); if (cls) node.classList.add(cls);
}
function tweenMoney(id, target, cls) {
  const tw = S.tweens[id] || (S.tweens[id] = { cur: target });
  const from = tw.cur; if (tw.raf) cancelAnimationFrame(tw.raf);
  if (matchMedia("(prefers-reduced-motion: reduce)").matches || from === target) { tw.cur = target; setMoney(id, target, cls); return; }
  const t0 = performance.now(), dur = 450;
  const step = (now) => { const p = Math.min(1, (now - t0) / dur), e = 1 - Math.pow(1 - p, 3); tw.cur = from + (target - from) * e; setMoney(id, tw.cur, cls); if (p < 1) tw.raf = requestAnimationFrame(step); };
  tw.raf = requestAnimationFrame(step);
}

// ---------------------------------------------------------------- chart (SVG, time on x)
function lineChart(container, series, opts = {}) {
  clear(container); container.classList.add("chart");
  const names = Object.keys(series).filter((k) => series[k] && series[k].length);
  if (!names.length) { container.append(el("div", "empty", opts.empty || "no data yet")); return; }
  const W = Math.max(320, Math.round(container.clientWidth || 600)), H = opts.height || 220, m = { t: 12, r: 16, b: 24, l: 64 };
  const pts = names.flatMap((n) => series[n]);
  let x0 = Math.min(...pts.map((p) => p[0])), x1 = Math.max(...pts.map((p) => p[0]));
  if (opts.xmin != null) x0 = Math.min(x0, opts.xmin);
  if (x1 - x0 < 60000) x1 = x0 + 60000;
  const ys = pts.map((p) => p[1]).concat((opts.refs || []).filter((r) => r.fit !== false).map((r) => r.y)).concat((opts.dots || []).map((d) => d.y));
  let y0 = Math.min(...ys), y1 = Math.max(...ys); if (y1 - y0 < (opts.minSpan || 1)) { const c = (y0 + y1) / 2; y0 = c - (opts.minSpan || 1) / 2; y1 = c + (opts.minSpan || 1) / 2; }
  const pad = (y1 - y0) * 0.08; y0 -= pad; y1 += pad;
  const X = (x) => m.l + (x - x0) / (x1 - x0) * (W - m.l - m.r), Y = (y) => m.t + (1 - (y - y0) / (y1 - y0)) * (H - m.t - m.b);
  const ns = "http://www.w3.org/2000/svg", svg = document.createElementNS(ns, "svg");
  svg.setAttribute("viewBox", `0 0 ${W} ${H}`); svg.setAttribute("width", W); svg.setAttribute("height", H);
  const g = (cls) => { const e = document.createElementNS(ns, "g"); e.setAttribute("class", cls); svg.append(e); return e; };
  const grid = g("grid"), axis = g("axis");
  const yfmt = opts.yfmt || ((v) => String(Math.round(v)));
  for (let i = 0; i <= 4; i++) {
    const y = y0 + (y1 - y0) * i / 4, ln = document.createElementNS(ns, "line");
    ln.setAttribute("x1", m.l); ln.setAttribute("x2", W - m.r); ln.setAttribute("y1", Y(y)); ln.setAttribute("y2", Y(y)); grid.append(ln);
    const tx = document.createElementNS(ns, "text"); tx.setAttribute("x", m.l - 8); tx.setAttribute("y", Y(y) + 4); tx.setAttribute("text-anchor", "end"); tx.textContent = yfmt(y); axis.append(tx);
  }
  const ticks = 5;
  for (let i = 0; i <= ticks; i++) {
    const x = x0 + (x1 - x0) * i / ticks, tx = document.createElementNS(ns, "text");
    tx.setAttribute("x", X(x)); tx.setAttribute("y", H - 6); tx.setAttribute("text-anchor", i === 0 ? "start" : i === ticks ? "end" : "middle"); tx.textContent = fmt.hm(x); axis.append(tx);
  }
  for (const r of opts.refs || []) {
    const rg = g("ref"), ln = document.createElementNS(ns, "line");
    ln.setAttribute("x1", m.l); ln.setAttribute("x2", W - m.r); ln.setAttribute("y1", Y(r.y)); ln.setAttribute("y2", Y(r.y)); if (r.color) ln.style.stroke = r.color; rg.append(ln);
    const tx = document.createElementNS(ns, "text"); tx.setAttribute("x", W - m.r); tx.setAttribute("y", Y(r.y) - 4); tx.setAttribute("text-anchor", "end"); tx.textContent = r.label; if (r.color) tx.style.fill = r.color; rg.append(tx);
  }
  const colors = opts.colors || {};
  names.forEach((n, i) => {
    const sg = g("ser"), p = document.createElementNS(ns, "path");
    const d = series[n].map((pt, k) => (k ? "L" : "M") + X(pt[0]).toFixed(1) + " " + Y(pt[1]).toFixed(1)).join(" ");
    p.setAttribute("d", d); p.style.stroke = colors[n] || ["var(--accent)", "var(--raw)", "#199e70"][i % 3]; if (opts.step) p.setAttribute("shape-rendering", "crispEdges"); sg.append(p);
  });
  for (const d of opts.dots || []) {
    const dg = g("ser"), c = document.createElementNS(ns, "circle");
    c.setAttribute("cx", X(d.t)); c.setAttribute("cy", Y(d.y)); c.setAttribute("r", 3.5); c.style.fill = d.color; c.style.stroke = "var(--surface)"; dg.append(c);
    const title = document.createElementNS(ns, "title"); title.textContent = d.label; c.append(title);
  }
  container.append(svg);
  if (opts.legend) {
    const lg = el("div", "legend");
    names.forEach((n, i) => { const s = el("span"); const sw = el("i"); sw.style.background = colors[n] || ["var(--accent)", "var(--raw)"][i % 2]; s.append(sw, document.createTextNode(n)); lg.append(s); });
    container.append(lg);
  }
}

function nowMs() { if (MODE !== "backtest") return Date.now(); const p = S.prints[S.prints.length - 1]; return p ? p[0] : (S.marketNow || Date.now()); }
function scheduleCharts() { if (S.chartTimer) return; S.chartTimer = setTimeout(() => { S.chartTimer = null; renderCharts(); }, 250); }
function renderCharts() {
  const now = nowMs(), from = now - 30 * 60000;
  const tape = S.prints.filter((p) => p[0] >= from).map((p) => [p[0], p[1]]);
  const dots = S.fills.filter((f) => f.t >= from).map((f) => ({ t: f.t, y: f.price, color: f.lane === "raw" ? "var(--raw)" : "var(--accent)", label: `${f.lane === "raw" ? "ordinary" : "bounded"} ${f.side} ${f.qty} @ ${f.price} (${f.ref || ""}, fee ${fmt.usd(f.fee_cents)})` }));
  lineChart($("#tape-chart"), { "ETH-USD": tape }, { yfmt: (v) => "$" + Math.round(v).toLocaleString("en-US"), colors: { "ETH-USD": "var(--fg-2)" }, dots, minSpan: 6, height: 230, empty: "waiting for prints" });
  $("#tape-note").textContent = tape.length ? `last 30 ${MODE === "backtest" ? "market " : ""}min · ${tape.length} prints · fills as dots` : "";
  const cap = S.cfg ? S.cfg.capital_cents : 1000000, lim = S.cfg ? cap - S.cfg.envelope.max_loss_cents : cap - 50000;
  const eq = S.equity;
  const rawS = eq.map((r) => [r[0], r[2]]), bndS = eq.map((r) => [r[0], r[3]]);
  lineChart($("#eq-chart"), { "ordinary key": rawS, "bounded key": bndS }, {
    yfmt: (v) => fmt.usd0(v), colors: { "ordinary key": "var(--raw)", "bounded key": "var(--accent)" }, legend: true, height: 230, minSpan: 20000,
    refs: [{ y: cap, label: "start " + fmt.usd0(cap) }, { y: lim, label: "the owner's limit " + fmt.usd0(lim) + " (24 h)", color: "var(--warn)" }],
    empty: "waiting for the first print",
  });
  $("#eq-note").textContent = eq.length ? `since the race started · ${fmt.dur(now - eq[0][0])}${MODE === "backtest" ? " of market time" : ""}` : "";
}

// ---------------------------------------------------------------- header
function renderHeader() {
  const st = S.st, f = S.feed || st.feed, inv = st.invariants;
  const live = f && f.connected;
  $("#live-dot").classList.toggle("off", !live);
  const src = { "coinbase-ws": "Coinbase ETH-USD, live", "coinbase-rest": "Coinbase ETH-USD, polled", replay: "REPLAYED tape (RACE_TAPE=replay)", backtest: "BACKTEST · Coinbase ETH-USD history", idle: "backtest idle: no window started", connecting: "connecting to Coinbase…", reconnecting: "reconnecting to Coinbase…" }[f && f.source] || (f && f.source) || "—";
  const ml = $("#mode-link"); if (ml) { ml.textContent = MODE === "backtest" ? "← live race" : "backtest →"; ml.href = MODE === "backtest" ? "race.html" : "race.html?mode=backtest"; }
  renderBacktestBanner();
  $("#feed-line").textContent = src + (f && f.ticks ? ` · ${f.ticks.toLocaleString("en-US")} trades seen` : "") + (f && f.error ? ` · ${f.error}` : "");
  const strip = clear($("#invariants"));
  const stub = S.route === "stub";
  const items = [
    ["same exchange", inv.same_exchange, false],
    ["same model", modelVia(S.model, S.route), stub],
    ["same prompt history", "sha256 per round", false],
    ["same tape", MODE === "backtest" ? "Coinbase history, no look-ahead" : ((f && f.source === "replay") ? "replayed" : "Coinbase live"), f && f.source === "replay"],
    ["same fees", `${S.cfg.envelope.env.fee_bps.maker} / ${S.cfg.envelope.env.fee_bps.taker} bp`, false],
  ];
  for (const [k, v, warn] of items) { const s = el("span", "inv" + (warn ? " warn" : "")); s.title = inv[k.replace(/ /g, "_")] || ""; s.append(el("i", "dot"), el("span", "k", k), el("span", "v", v)); strip.append(s); }
  renderModelOracle();
  const banner = $("#stub-banner");
  banner.hidden = !stub;
  if (stub) banner.textContent = "No model is answering right now (" + (st.route_reason || "no credentials") + "). A rule-based stand-in that complies with every request is trading both lanes, so what you see is the mechanism, not a model. It flips to the real model on its own once a provider key is configured.";
}

function renderModelOracle() {
  const o = $("#model-lead"), rn = $("#model-route"); if (!o || !rn) return;
  const st = S.st || {}, inv = st.invariants || {}, route = routeFor(S.model, S.route);
  const lead = S.route === "stub" ? "No model is answering right now: a rule-based stand-in trades both lanes."
    : `One model, ${S.model}, through ${routeName(route)}.`;
  o.textContent = lead;
  clear(rn);
  const bits = [];
  if (st.route_reason) bits.push("The gateway's own routing note: " + st.route_reason + ".");
  if (inv.same_model) bits.push("Header invariant as the backend reports it: \u201c" + inv.same_model + "\u201d.");
  if (S.route && S.route !== route) bits.push(`The backend reports the composite route as \u201c${S.route}\u201d; for a ${S.model.split("-")[0]}-* id the call actually goes through ${routeName(route)}.`);
  rn.textContent = bits.join(" ");
}

function renderBacktestBanner() {
  const b = $("#bt-banner"); if (!b) return;
  if (MODE !== "backtest") { b.hidden = true; return; }
  const st = S.st, bt = st && st.backtest; b.hidden = false; clear(b); b.classList.toggle("done", !!(st && st.finished));
  if (!bt) { b.append(el("span", "", "No backtest window is replaying right now. Replay the flash crash with the control below (public, about 4 minutes), or the operator can start any window with POST /backtest/start.")); return; }
  const now = nowMs(), from = bt.from * 1000, to = bt.to * 1000, prog = Math.max(0, Math.min(1, (now - from) / (to - from)));
  const day = new Date(from).toISOString().slice(0, 10), dayTo = new Date(to).toISOString().slice(0, 10);
  const span = dayTo === day ? `${day} ${fmt.clock(from).slice(0, 5)}–${fmt.clock(to).slice(0, 5)} UTC` : `${day} ${fmt.clock(from).slice(0, 5)} → ${dayTo} ${fmt.clock(to).slice(0, 5)} UTC`;
  b.append(el("b", "", st.finished ? "Backtest finished" : "Backtest running"), el("span", "", bt.label || day),
           el("span", "mono", `${span} · ${bt.speed}× · a decision every ${bt.round_minutes} market min`),
           el("span", "mono", "market clock " + fmt.clock(now)));
  const bar = el("div", "bar"); const i = el("i"); i.style.width = Math.round(prog * 100) + "%"; bar.append(i); b.append(bar);
  b.append(el("span", "small muted", st.finished ? `final: ordinary ${fmt.usd(st.finished.raw.pnl_cents, true)} · bounded ${fmt.usd(st.finished.bounded.pnl_cents, true)} · ${st.finished.rounds} rounds · ${st.finished.attacks} attacks` : "no look-ahead: prints are released by the market clock; it pauses while the models decide"));
}

// ---------------------------------------------------------------- lanes
function kv(node, pairs) { clear(node); for (const [k, v] of pairs) { node.append(el("span", "k", k), el("span", "v", v)); } }
function renderLanes() {
  const r = S.raw, b = S.bnd; if (!r || !b) return;
  const cap = S.cfg.capital_cents;
  // ordinary
  $("#raw-acct").textContent = "exchange account " + r.account;
  tweenMoney("#raw-eq", r.equity_cents, r.equity_cents > cap ? "up" : r.equity_cents < cap ? "down" : "");
  const rd = clear($("#raw-delta")); rd.append(el("b", "", fmt.usd(r.pnl_cents, true)), document.createTextNode(" since start · unrealized " + fmt.usd(r.unrealized_cents, true)));
  $("#raw-liq").hidden = !(r.liquidated || r.liquidating);
  $("#raw-liq").textContent = r.liquidated ? "LIQUIDATED" : "LIQUIDATING: flattening at the next print";
  kv($("#raw-kv"), [
    ["position", fmt.lots(r.position) + (r.avg_cost_cents ? " @ " + Math.round(r.avg_cost_cents / 100) : "")],
    ["fees paid", fmt.usd(r.fees_cents)], ["fills", String(r.trades)], ["open orders", String(r.open_orders.length)],
    ["peak exposure", r.max_exposure + " lots"], ["decisions", String(r.decisions)],
    ["exchange says", r.exchange_check && r.exchange_check.agree != null ? (r.exchange_check.agree ? "position and cash agree with the ledger" : "MISMATCH " + JSON.stringify(r.exchange_check)) : "—"],
  ]);
  const br = clear($("#raw-breaches"));
  const names = { exposure: "> " + S.cfg.envelope.max_exposure_lots + " lots", no_stop: "position with no stop", daily_loss: "loss > " + fmt.usd0(S.cfg.envelope.max_loss_cents) + " / 24 h", fees: "fees > " + fmt.usd0(S.cfg.envelope.max_fees_cents) };
  let any = false;
  for (const [k, v] of Object.entries(r.breaches || {})) {
    if (!v.count) continue; any = true;
    br.append(el("span", "breach" + (v.active ? "" : " past"), (v.active ? "● " : "") + names[k] + (v.total_ms ? " · " + fmt.dur(v.total_ms) : "") + (v.count > 1 ? " ×" + v.count : "")));
  }
  if (!any) br.append(el("span", "breach none", "inside the owner's rules so far"));
  const tk = clear($("#raw-tickets"));
  const orders = (r.recent_orders || []).slice().reverse();
  if (!orders.length) tk.append(el("span", "empty", "no orders yet"));
  for (const o of orders) {
    const t = el("div", "ticket " + ({ filled: "filled", resting: "live", held: "live", pending: "live", queued: "live", rejected: "rejected", cancelled: "", done: "" }[o.state] || ""));
    const what = `${o.kind.replace("_", "-")} ${o.side} ${o.qty}` + (o.price ? ` @ ${o.price}` : "") + (o.trigger ? ` trig ${o.trigger}` : "") + (o.reduce_only ? " · reduce-only" : "") + (o.group ? ` · oco ${o.group}` : "");
    t.append(el("span", "ref", o.ref), el("span", "what", what), el("span", "st", o.state + (o.filled ? ` ${o.filled}/${o.qty}` : "")));
    if (o.label && o.label !== o.kind && !o.label.startsWith(o.kind)) t.append(el("span", "sub", o.label));
    tk.append(t);
  }
  // bounded
  $("#bnd-acct").textContent = "gateway account " + b.account;
  tweenMoney("#bnd-eq", b.equity_cents, b.equity_cents > cap ? "up" : b.equity_cents < cap ? "down" : "");
  const bd = clear($("#bnd-delta")); bd.append(el("b", "", fmt.usd(b.pnl_cents, true)), document.createTextNode(" since start · free balance after escrow " + fmt.usd(b.balance_cents)));
  renderBankIds(b); renderPhone();
  const lp = b.live_program;
  kv($("#bnd-kv"), [
    ["position", lp ? fmt.lots(lp.position || 0) : "flat"], ["fees paid", fmt.usd(b.fees_cents)], ["fills", String(b.trades)],
    ["live program", lp ? `#${lp.program_id} · max loss ${fmt.usd(lp.bound.max_loss_cents)} · halts ≤ ${lp.bound.halts_within_trades} trades` : "none"],
    ["live nodes", lp ? (lp.live_nodes || []).join(", ") || "—" : "—"],
    ["programs", `${b.submitted} submitted · ${b.certified} certified · ${b.rejected} rejected (422) · ${b.refused_409} refused (409)`],
    ["realized loss", fmt.usd(b.realized_loss_cents)],
  ]);
  const bud = b.budget || {}, maxL = S.cfg.envelope.max_loss_cents, left = bud.remaining_loss_cents;
  const bg = clear($("#bnd-budget"));
  if (left != null) {
    bg.append(el("div", "small muted", `24 h loss budget: ${fmt.usd(left)} of ${fmt.usd(maxL)} left · ${fmt.usd(bud.reserved_by_live_programs_cents || 0)} reserved by the live program · ${fmt.usd(bud.realized_loss_in_window_cents || 0)} realized`));
    const bar = el("div", "bar" + (left < maxL * 0.25 ? " hot" : "")); const i = el("i"); i.style.width = Math.max(0, Math.min(100, 100 * left / maxL)) + "%"; bar.append(i); bg.append(bar);
  }
  const bt = clear($("#bnd-tickets"));
  const progs = (b.programs || []).slice().reverse();
  if (!progs.length) bt.append(el("span", "empty", "no programs yet"));
  for (const p of progs) {
    const cls = p.http === 200 ? "ok" : (p.http === 422 || p.http === 400) ? "bad" : "";
    const t = el("div", "ticket " + cls);
    let what, st;
    if (p.http === 200 && p.bound) { what = `certified · max loss ${fmt.usd(p.bound.max_loss_cents)} · fees ≤ ${fmt.usd(p.bound.max_fees_cents)} · halts ≤ ${p.bound.halts_within_trades}`; st = p.final_status ? p.final_status + (p.realized ? " · lost " + fmt.usd(p.realized.loss_cents) : "") : "running"; }
    else if (p.http === 200) { what = p.outcome || p.summary || "accepted"; st = p.final_status || "200"; }
    else if (p.http === 422) { what = `rejected · violates ${p.violates} · ${p.value_on_path != null ? fmt.viol(p.value_on_path, p.violates) + " on the path" : ""} vs limit ${fmt.viol(p.limit, p.violates)}`; st = "422"; }
    else if (p.http === 409) { what = p.outcome === "budget" ? "certified, but the 24 h budget is spent" : p.outcome === "instrument_busy" ? "a program is already live on ETH" : p.outcome; st = "409"; }
    else if (p.http === 400) { what = `static error line ${p.line}: ${p.message}`; st = "400"; }
    else { what = p.outcome || "—"; st = String(p.http || ""); }
    t.append(el("span", "ref", p.program_id ? "#" + p.program_id : "—"), el("span", "what", what), el("span", "st", st));
    if (p.source) { const d = el("details"); d.append(el("summary", "", "program"), el("pre", "", p.source.trim())); d.className = "sub"; t.append(d); }
    bt.append(t);
  }
  kv($("#env-kv"), [["max loss / 24 h", fmt.usd(S.cfg.envelope.max_loss_cents)], ["max fees / 24 h", fmt.usd(S.cfg.envelope.max_fees_cents)], ["max trades", String(S.cfg.envelope.max_trades)],
                     ["max exposure", S.cfg.envelope.max_exposure_lots + " lots"], ["gap assumption", "≤ " + S.cfg.envelope.env.max_gap_ticks + " ticks between prints"], ["live stop", S.cfg.envelope.require_live_stop ? "required" : "not required"]]);
  $("#brief").textContent = S.cfg.brief;
}

function bankName(bank) { return bank === "nessie" ? "Nessie (Capital One sandbox)" : bank === "local-sim" || bank === "sim" || bank === "local" ? "local simulator" : bank ? String(bank) : "no bank reported"; }
function monoId(v, cls) { const s = el("span", "mono id" + (cls ? " " + cls : "")); const str = v == null ? "—" : String(v); s.textContent = str.length > 14 ? str.slice(0, 6) + "…" + str.slice(-6) : str; s.title = str; return s; }
function renderBankIds(b) {
  const node = $("#bnd-bank"); if (!node) return; clear(node);
  const ids = b.bank_ids, bank = (ids && ids.bank) || b.bank || (S.st && S.st.world && S.st.world.bank);
  const head = el("div", "small muted"); head.append(document.createTextNode("bank: " + bankName(bank)));
  if (b.bank_equity_cents != null && bank) head.append(document.createTextNode(` · the bank's view of the account ${fmt.usd(b.bank_equity_cents)}`));
  node.append(head);
  if (!ids) { node.append(el("div", "small faint", bank ? "customer and account ids are not in this state yet (bank_ids arrives after the operator's next restart)" : "")); return; }
  const kvn = el("div", "kv ids"); node.append(kvn);
  const row = (k, v) => { kvn.append(el("span", "k", k)); const sp = el("span", "v"); sp.append(v instanceof Node ? v : document.createTextNode(v)); kvn.append(sp); };
  row("customer", monoId(ids.customer_id)); row("account", monoId(ids.account_id));
  const esc = ids.escrows || [];
  if (!esc.length) row("escrows", "none live");
  for (const e of esc) {
    const v = el("span"); v.append(document.createTextNode(`#${e.program_id} · ${fmt.usd(e.cents)} in `), monoId(e.escrow_account_id));
    v.append(document.createTextNode(e.settled ? " · settled, tx " : " · unsettled"));
    if (e.settled) v.append(monoId(e.settlement_tx_id));
    row("escrow", v);
  }
}
function renderPhone() {
  const node = $("#bnd-phone"); if (!node) return; clear(node);
  const ob = (S.st && S.st.outbox) || [];
  if (!Array.isArray(ob) || !ob.length) { node.append(el("span", "empty", S.st && "outbox" in S.st ? "no messages yet" : "no messages yet (the outbox feed arrives after the operator's next restart)")); return; }
  for (const m of ob.slice().sort((a, b) => (b.t || 0) - (a.t || 0)).slice(0, 5)) {
    const d = el("div", "sms" + (m.delivered ? " sent" : ""));
    const top = el("div", "sms-h");
    top.append(el("span", "mono", m.t ? fmt.clock(m.t) : "—"), el("span", "", m.to_masked || "no number"), el("span", "pill " + (m.delivered ? "live" : "skipped"), (m.delivered ? "delivered" : "not delivered") + " · " + (m.via || "log")));
    d.append(top, el("div", "sms-t", m.text || ""));
    const meta = []; if (m.event_type) meta.push(m.event_type); if (m.message_id) meta.push("id " + m.message_id);
    if (meta.length) d.append(el("div", "small faint mono", meta.join(" · ")));
    node.append(d);
  }
}

function renderPrice() {
  const p = S.prints[S.prints.length - 1]; if (!p) return;
  const node = $("#px"); node.textContent = p[2].toLocaleString("en-US");
  node.classList.remove("up", "down"); if (S.prevPx != null && p[2] !== S.prevPx) node.classList.add(p[2] > S.prevPx ? "up" : "down"); S.prevPx = p[2];
  const t = S.st && S.st.tape; const n = S.printsTotal || (t && t.prints && t.prints.n) || S.prints.length;
  $("#px-sub").textContent = `$${p[1].toFixed(2)} · ${n.toLocaleString("en-US")} prints` + (S.violations ? ` · ${S.violations} outside the gap model` : "") + (p[3] > 1 ? ` · gap ${p[3]}` : "");
}

function renderRoundState() {
  const node = clear($("#round-state"));
  if (!S.cfg) return;
  if (S.inRound) { node.append(el("span", "", "round "), el("b", "", String(S.roundN)), el("span", "", ": the model is deciding…")); return; }
  if (MODE === "backtest") {
    const bt = S.st && S.st.backtest; const mins = bt ? bt.round_minutes : 10;
    const nextM = S.lastRoundT ? Math.max(0, S.lastRoundT + mins * 60000 - nowMs()) : null;
    node.append(el("span", "", S.roundN ? "round " : "first round "), el("b", "", S.roundN ? String(S.roundN) : ""), el("span", "", S.st && S.st.finished ? " · finished" : (nextM == null ? " soon" : ` · next in ${Math.ceil(nextM / 60000)} market min`)));
    return;
  }
  const next = S.lastRoundT ? Math.max(0, S.lastRoundT + S.cfg.round_s * 1000 - Date.now()) : null;
  node.append(el("span", "", S.roundN ? "round " : "first round "), el("b", "", S.roundN ? String(S.roundN) : ""), el("span", "", next == null ? " soon" : ` · next in ${Math.ceil(next / 1000)}s`));
  if (S.pending) node.append(el("div", "small", S.pending + " visitor message" + (S.pending > 1 ? "s" : "") + " queued"));
}

// ---------------------------------------------------------------- rounds
function roundCard(r) {
  let card = document.getElementById("round-" + r.round_id);
  if (!card) {
    card = el("article", "round"); card.id = "round-" + r.round_id;
    card.append(el("div", "rh"), el("div", "shared"), el("div", "cols"));
    const cols = $(".cols", card); cols.append(el("div", "col raw"), el("div", "col bnd"));
    $("#rounds").append(card);
    const empty = $("#rounds .empty"); if (empty) empty.remove();
  }
  const rh = clear($(".rh", card));
  rh.append(el("b", "", "Round " + r.n), el("span", "", fmt.clock(r.t)));
  const atk = (r.shared || []).filter((e) => e.kind === "message" && e.meta && e.meta.source === "attack");
  const vis = (r.shared || []).filter((e) => e.kind === "message" && e.meta && e.meta.source === "visitor");
  if (atk.length) rh.append(el("span", "bad-ink", "attack: " + atk.map((a) => a.meta.class.replace("_", " ")).join(", ")));
  if (vis.length) rh.append(el("span", "", vis.length + " visitor message" + (vis.length > 1 ? "s" : "")));
  card.classList.toggle("attacked", atk.length > 0 || vis.length > 0);
  const hr = r.raw && r.raw.shared_hash, hb = r.bounded && r.bounded.shared_hash;
  const hash = el("span", "hash" + (hr && hb && hr === hb ? " ok" : ""), hr && hb ? (hr === hb ? "same history ✓ " + hr.slice(0, 10) : "history differs!") : (hr || hb ? "history " + (hr || hb).slice(0, 10) : ""));
  const pl = el("a", "small", "prompts"); pl.href = "#"; pl.style.marginLeft = "12px"; pl.onclick = (e) => { e.preventDefault(); showPrompts(r.round_id, r.n); };
  rh.append(hash, pl);
  const sh = clear($(".shared", card));
  for (const e of r.shared || []) {
    const src = e.meta && e.meta.source || e.kind;
    const m = el("div", "msg " + (src === "attack" ? "attack" : src === "visitor" ? "visitor" : e.kind));
    const who = el("span", "who", src === "attack" ? "message" : src === "visitor" ? "message" : e.kind);
    const tx = el("span", "text", e.text);
    if (src === "attack") tx.append(el("span", "cls", "(from the attack set: " + e.meta.class.replace("_", " ") + ", wants " + e.meta.intent.replace(/_/g, " ") + ")"));
    if (src === "visitor") tx.append(el("span", "cls", "(a visitor to this page)"));
    m.append(who, tx); sh.append(m);
  }
  laneCol($(".col.raw", card), "raw", r.raw);
  laneCol($(".col.bnd", card), "bounded", r.bounded);
  return card;
}

function actSummary(d) {
  if (!d) return "";
  if (d.action === "orders") {
    const parts = (d.orders || []).map((o) => `${o.kind} ${o.side} ${o.qty}` + (o.price != null ? ` @${o.price}` : "") + (o.trigger != null ? ` trig ${o.trigger}` : "") + (o.allow ? ` allow ${o.allow}` : "") + (o.reduce_only ? " reduce-only" : "") + (o.after != null ? ` after#${o.after}` : "") + (o.group ? ` oco:${o.group}` : ""));
    if (d.cancel && d.cancel.length) parts.push("cancel " + d.cancel.join(", "));
    return parts.join("\n");
  }
  if (d.action === "program") return (d.mpl || "").trim();
  if (d.action === "cancel") return "cancel the live program";
  return "hold";
}

function laneCol(col, lane, rec) {
  clear(col);
  const who = el("div", "who " + (lane === "raw" ? "raw-ink" : "bnd-ink"), lane === "raw" ? "ordinary key" : "bounded key");
  col.append(who);
  if (!rec) { const th = el("div", "thinking"); th.append(el("i"), document.createTextNode("deciding…")); col.append(th); return; }
  const exchanges = rec.exchanges && rec.exchanges.length ? rec.exchanges : [{ decision: rec.decision, result: rec.result, llm: rec.llm }];
  exchanges.forEach((x, i) => {
    if (i) col.append(el("div", "meta", "repair attempt " + i + ": the model saw the counterexample and tried again"));
    const d = x.decision || {}, res = x.result || {};
    col.append(el("div", "note", d.note || (d.action === "hold" ? "(holds)" : "")));
    const act = actSummary(d); if (act && d.action !== "hold") col.append(el("pre", "act", act));
    if (lane === "raw") {
      if (d.action !== "hold") col.append(el("div", "res", res.summary || ""));
      else col.append(el("span", "verdict hold", "hold"));
    } else {
      if (d.action === "hold") col.append(el("span", "verdict hold", "hold"));
      else if (res.http) {
        const v = el("span", "verdict v" + res.http);
        v.textContent = res.http === 200 ? (res.bound ? `200 certified · max loss ${fmt.usd(res.bound.max_loss_cents)} · halts ≤ ${res.bound.halts_within_trades} trades · deployed` : `200 · ${res.summary || res.outcome || (d.action === "cancel" ? "live program cancelled" : "accepted")}`)
          : res.http === 422 ? `422 rejected · ${res.violates}` + (res.value_on_path != null ? ` · ${fmt.viol(res.value_on_path, res.violates)} on the path vs ${fmt.viol(res.limit, res.violates)}` : "")
          : res.http === 409 ? `409 ${res.outcome}` : res.http === 400 ? `400 static error, line ${res.line}` : String(res.http);
        col.append(v);
        if (res.http === 422 && res.counterexample && res.counterexample.length) {
          const det = el("details"); det.append(el("summary", "", "the path that breaks the envelope (" + res.counterexample.length + " steps)"));
          det.append(el("pre", "", res.counterexample.map(stepText).join("\n"))); col.append(det);
        } else if (res.http === 409 || res.http === 400) col.append(el("div", "res", res.summary || ""));
      } else col.append(el("div", "res", res.summary || res.outcome || ""));
    }
    if (x.llm) col.append(el("div", "meta", (x.llm.model || "") + (x.llm.latency_ms != null ? ` · ${(x.llm.latency_ms / 1000).toFixed(1)}s` : "") + (x.llm.output_tokens ? ` · ${x.llm.output_tokens} tokens out` : "") + (x.llm.error ? " · " + x.llm.error : "")));
  });
}
function stepText(s) {
  if (s.env === "print") return `print ${s.at}`;
  if (s.env === "gap") return `gap ${s.from} → ${s.to}`;
  if (s.keyholder) return `key holder: ${s.keyholder}`;
  if (s.event === "filled") return `  ${s.node} fills ${s.qty} @ ${s.price} (fee ${fmt.usd(s.fee_cents || 0)})`;
  if (s.event === "triggered") return `  ${s.node} triggers at ${s.print}`;
  if (s.event === "rests") return `  ${s.node} rests: ${s.reason}`;
  if (s.event === "mark") return `mark: position ${s.position}, loss ${fmt.usd(s.loss_cents)}`;
  return JSON.stringify(s);
}
async function showPrompts(id, n) {
  const dlg = $("#prompts");
  try {
    const r = await get(API + "/rounds/" + id);
    $("#dlg-n").textContent = n;
    const rx = r.raw && r.raw.exchanges && r.raw.exchanges[0], bx = r.bounded && r.bounded.exchanges && r.bounded.exchanges[0];
    $("#dlg-hash").textContent = rx && bx ? (rx.shared_hash === bx.shared_hash ? "shared history sha256 " + rx.shared_hash.slice(0, 16) + "… identical in both lanes" : "shared history differs") : "";
    $("#dlg-raw").textContent = r.raw ? "SYSTEM:\n" + r.raw.prompt_system + "\n\nUSER:\n" + (rx ? rx.prompt_user : "") + "\n\nMODEL:\n" + (rx ? rx.response : "") : "(not yet)";
    $("#dlg-bnd").textContent = r.bounded ? "SYSTEM:\n" + r.bounded.prompt_system + "\n\nUSER:\n" + (bx ? bx.prompt_user : "") + "\n\nMODEL:\n" + (bx ? bx.response : "") : "(not yet)";
    dlg.showModal();
  } catch (e) { alert("could not load the round: " + e.message); }
}
$("#dlg-close").onclick = () => $("#prompts").close();

function renderRounds() {
  const list = [...S.rounds.values()].sort((a, b) => a.n - b.n).slice(-40);
  for (const r of list) roundCard(r);
  $("#conv-note").textContent = S.roundN ? `${S.roundN} rounds · ${S.st.epoch.attacks} attacks · ${S.st.epoch.visitor_messages} visitor messages · every ${S.cfg.round_s}s` : "";
}

function renderHistory() {
  const hist = S.st.history || [], top = $("#history-top"), sec = $("#history-sec"), side = $("#history");
  const bt = MODE === "backtest";
  if (sec) sec.hidden = !bt;
  const h = clear(bt && top ? top : side);
  if (bt && side) { clear(side).append(el("span", "small faint", "listed under the two accounts above")); }
  if (!hist.length) { h.append(el("span", "empty", "none yet")); if ($("#hist-note")) $("#hist-note").textContent = ""; return; }
  if ($("#hist-note")) $("#hist-note").textContent = `${hist.length} windows · read back from ${(S.st.world && S.st.world.db) || "the race database"}`;
  for (const e of hist) {
    const f = e.final || {}, c = e.config || {}, label = (c.backtest && c.backtest.label) || "";
    const crash = /flash crash/i.test(label);
    const row = el("div", "h" + (crash ? " crash" : ""));
    const name = el("span", "hn");
    name.append(el("b", "", `#${e.id}`), document.createTextNode(` · ${fmt.dur((e.ended_t || Date.now()) - e.started_t)} · ${f.rounds || 0} rounds` + (f.attacks != null ? ` · ${f.attacks} attacks` : "") + (f.reason ? ` · ${f.reason}` : "")));
    if (label || c.model) name.append(el("span", "small " + (crash ? "crash-ink" : "muted"), (label ? label : "") + (c.model ? (label ? " · " : "") + modelVia(c.model, c.route) : "")));
    row.append(name,
               el("span", "mono raw-ink", f.raw ? fmt.usd(f.raw.pnl_cents, true) + (f.raw.liquidated ? " liquidated" : "") : "—"),
               el("span", "mono bnd-ink", f.bounded ? fmt.usd(f.bounded.pnl_cents, true) : "—"));
    h.append(row);
  }
}

// ---------------------------------------------------------------- backtest idle hero: the last finished window's numbers
let lastCompare = null;
async function renderIdleHero() {
  const box = $("#bt-last"), lanes = $("#hero .lanes"); if (!box || !lanes) return;
  const idle = MODE === "backtest" && S.st && !S.st.backtest;
  box.hidden = !idle; lanes.hidden = idle;
  if (!idle) return;
  clear(box); box.append(el("div", "small muted", "no window is replaying right now · loading the last finished one…"));
  let cmp = lastCompare;
  if (!cmp) { try { cmp = lastCompare = await get("/backtest/compare"); } catch (e) { clear(box).append(el("div", "small muted", "no window is replaying right now, and the results table could not be read: " + e.message)); return; } }
  if (!(S.st && !S.st.backtest)) return;
  const rows = (cmp.rows || []).filter((r) => r.finished && !r.in_progress).sort((a, b) => (b.ended_t || b.started_t) - (a.ended_t || a.started_t));
  const r = rows[0];
  clear(box);
  if (!r) { box.append(el("div", "small muted", "no window is replaying right now and none has finished yet. Use the replay control above.")); return; }
  const crash = /flash crash/i.test(r.label || "");
  const head = el("div", "bl-head");
  head.append(el("span", "label", "last finished window · #" + r.epoch), el("b", crash ? "crash-ink" : "", r.label || "backtest"), el("span", "small muted", modelVia(r.model, r.route) + ` · ${r.rounds} rounds · ${(r.messages || {}).attack || 0} attacks · finished ${fmt.rel(r.ended_t || r.started_t)}`));
  box.append(head);
  const g = el("div", "bl-grid");
  const lane = (cls, title, acct, pnl, lines) => {
    const d = el("div", "bl-lane " + cls); d.append(el("div", "label", title));
    const big = el("div", "big" + (pnl > 0 ? " up" : pnl < 0 ? " down" : "")); const sTxt = fmt.usd(acct); const i = sTxt.lastIndexOf("."); big.append(document.createTextNode(sTxt.slice(0, i)), el("span", "c", sTxt.slice(i))); d.append(big);
    const dl = el("div", "delta"); dl.append(el("b", "", fmt.usd(pnl, true)), document.createTextNode(" over the window")); d.append(dl);
    const k = el("div", "kv"); for (const [a, b] of lines) k.append(el("span", "k", a), el("span", "v", b)); d.append(k); return d;
  };
  const rw = r.raw || {}, bd = r.bounded || {}, br = Object.entries(rw.breaches || {}).filter(([, v]) => v && v.count);
  g.append(lane("raw", "Ordinary API key", rw.final_equity_cents, rw.pnl_cents, [
    ["max drawdown", fmt.usd(-(rw.max_drawdown_cents || 0))], ["fills · fees", `${rw.fills} · ${fmt.usd(rw.fees_cents)}`], ["peak exposure", rw.max_exposure != null ? rw.max_exposure + " lots" : "—"],
    ["rule breaches", br.length ? br.map(([k, v]) => `${k.replace("_", " ")} ×${v.count}`).join(", ") : rw.breaches ? "none" : "not recorded"], ["liquidated", rw.liquidated ? "yes" : "no"]]));
  const mid = el("div", "bl-mid"); mid.append(el("div", "px", r.market && r.market.low != null ? `${r.market.low}–${r.market.high}` : "—"), el("div", "label", "ETH range in the window"), el("div", "vs", "vs")); g.append(mid);
  const http = bd.http || {};
  g.append(lane("bounded", "Bounded API key", bd.final_equity_cents, bd.pnl_cents, [
    ["max drawdown", fmt.usd(-(bd.max_drawdown_cents || 0))], ["fills · fees", `${bd.fills} · ${fmt.usd(bd.fees_cents)}`],
    ["programs", `${bd.programs || 0} submitted · ${http["200"] || 0} certified · ${http["422"] || 0} rejected · ${http["409"] || 0} refused · ${http["400"] || 0} static errors`],
    ["certificate breaches", String(bd.certificate_breaches != null ? bd.certificate_breaches : "—") + (bd.ended != null ? ` of ${bd.ended} ended` : "")],
    ["worst realized vs certified", bd.worst_realized_vs_bound ? `${fmt.usd(bd.worst_realized_vs_bound[0])} of ${fmt.usd(bd.worst_realized_vs_bound[1])}` : "—"]]));
  box.append(g);
  const foot = el("div", "small muted"); foot.append(document.createTextNode("every number here is read back from " + ((S.st.world && S.st.world.db) || "the backtest database") + " via /backtest/compare · "));
  const a = el("a", "", "the full results table"); a.href = "compare.html"; foot.append(a); box.append(foot);
}

// ---------------------------------------------------------------- replay the flash crash
const REPLAY_MODELS = ["grok-4.20-0309-non-reasoning", "grok-4.7", "grok-4.20-0309-reasoning", "claude-sonnet-5-5"];
(function wireReplay() {
  const go = $("#replay-go"), sel = $("#replay-model"), st = $("#replay-state"); if (!go || !sel || !st) return;
  sel.value = REPLAY_MODELS[0];
  go.onclick = async () => {
    const model = REPLAY_MODELS.includes(sel.value) ? sel.value : REPLAY_MODELS[0];
    go.disabled = true; clear(st); st.textContent = "asking the backtest conductor…";
    try {
      const r = await fetch("/backtest/replay", { method: "POST", headers: { "content-type": "application/json", accept: "application/json" }, body: JSON.stringify({ scenario: "crash", model }) });
      const body = await r.json().catch(() => ({}));
      if (r.status === 409) { st.textContent = "A backtest window is already in progress; wait for it to finish (about 4 minutes) and try again."; return; }
      if (r.status === 429) { st.textContent = "Rate limited: one replay per visitor every 10 minutes and one every 4 minutes for everyone. " + (body.detail && typeof body.detail === "string" ? body.detail : "Try again in a few minutes."); return; }
      if (r.status === 404) { st.textContent = "The public replay route is not on this gateway yet (it arrives at the operator's next restart). Until then the operator can start it with POST /backtest/start."; return; }
      if (!r.ok) { st.textContent = "Not started: " + (body.detail && typeof body.detail === "string" ? body.detail : body.error || ("HTTP " + r.status)); return; }
      clear(st);
      const eta = body.eta_s != null ? Math.round(body.eta_s / 60) : 4;
      st.append(el("b", "", "Started"), document.createTextNode(` · window #${body.epoch != null ? body.epoch : "pending"} · ${body.model || model} via ${routeName(routeFor(body.model || model, body.route))} · about ${eta} min at 120× · `));
      if (MODE === "backtest") { st.append(document.createTextNode("following it below as it runs")); lastCompare = null; setTimeout(load, 1500); setTimeout(load, 6000); }
      else { const a = el("a", "", "watch it on the backtest page →"); a.href = "race.html?mode=backtest"; st.append(a); }
    } catch (e) { st.textContent = "Not started: " + e.message; }
    finally { go.disabled = false; }
  };
})();

// ---------------------------------------------------------------- talk
$("#talk-send").onclick = async () => {
  const text = $("#talk-text").value.trim(); const st = $("#talk-state");
  if (!text) { st.textContent = "write something first"; return; }
  $("#talk-send").disabled = true; st.textContent = "sending…";
  try {
    const r = await fetch(API + "/message", { method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify({ text }) });
    const body = await r.json().catch(() => ({}));
    if (!r.ok) throw new Error(body.detail || body.error || r.status);
    st.textContent = `queued (${body.pending} waiting). Both agents read it on the next round, word for word.`;
    $("#talk-text").value = "";
  } catch (e) { st.textContent = "not sent: " + e.message; }
  finally { $("#talk-send").disabled = false; }
};

// ---------------------------------------------------------------- stream
function connect(since) {
  if (S.es) S.es.close();
  const es = new EventSource(API + "/stream?since=" + (since || 0)); S.es = es;
  const on = (t, fn) => es.addEventListener(t, (e) => { let d; try { d = JSON.parse(e.data); } catch (_) { return; } S.lastId = e.lastEventId || S.lastId; fn(d); });
  on("print", (d) => { S.marketNow = d.t; S.prints.push([d.t, d.price, d.ticks, d.gap]); if (S.prints.length > 6000) S.prints.splice(0, 1000); S.printsTotal = (S.printsTotal || 0) + 1; if (d.violation) S.violations = (S.violations || 0) + 1; renderPrice(); scheduleCharts(); });
  on("equity", (d) => {
    const last = S.equity[S.equity.length - 1];
    if (!last || d.t - last[0] >= 1000) S.equity.push([d.t, d.price, d.raw.equity_cents, d.bounded.equity_cents, d.raw.position, d.bounded.position]);
    else { last[2] = d.raw.equity_cents; last[3] = d.bounded.equity_cents; }
    if (S.equity.length > 5000) S.equity = S.equity.filter((_, i) => i % 2 === 0);
    const cap = S.cfg.capital_cents;
    tweenMoney("#raw-eq", d.raw.equity_cents, d.raw.equity_cents > cap ? "up" : d.raw.equity_cents < cap ? "down" : "");
    tweenMoney("#bnd-eq", d.bounded.equity_cents, d.bounded.equity_cents > cap ? "up" : d.bounded.equity_cents < cap ? "down" : "");
    if (S.raw) { S.raw.equity_cents = d.raw.equity_cents; S.raw.pnl_cents = d.raw.equity_cents - cap; }
    if (S.bnd) { S.bnd.equity_cents = d.bounded.equity_cents; S.bnd.pnl_cents = d.bounded.equity_cents - cap; }
    scheduleCharts();
  });
  on("fill", (d) => { S.fills.push(d); scheduleCharts(); });
  on("lane.state", (d) => { S.raw = d.raw; S.bnd = d.bounded; renderLanes(); });
  on("round.started", (d) => { S.inRound = true; S.roundN = d.n; S.lastRoundT = d.t; S.rounds.set(d.round_id, { round_id: d.round_id, n: d.n, t: d.t, shared: d.shared, raw: null, bounded: null }); roundCard(S.rounds.get(d.round_id)); renderRoundState(); });
  on("lane.decision", (d) => { const r = S.rounds.get(d.round_id); if (!r) return; const rec = r[d.lane] || (r[d.lane] = { exchanges: [] }); rec.exchanges.push({ decision: d.decision, llm: d.llm, result: null, shared_hash: d.shared_hash }); rec.shared_hash = d.shared_hash; rec.decision = d.decision; roundCard(r); });
  on("lane.result", (d) => { const r = S.rounds.get(d.round_id); if (!r) return; const rec = r[d.lane] || (r[d.lane] = { exchanges: [] }); const x = rec.exchanges[rec.exchanges.length - 1]; if (x) x.result = d.result; else rec.exchanges.push({ decision: d.decision, result: d.result }); rec.result = d.result; roundCard(r); });
  on("round.finished", (d) => { S.inRound = false; S.lastRoundT = d.t; renderRoundState(); if (S.st) { S.st.epoch.rounds = d.n; } });
  on("message.queued", (d) => { S.pending = d.pending; renderRoundState(); });
  on("route", (d) => { S.route = d.route; S.model = d.model; if (S.st) { S.st.route_reason = d.reason; renderHeader(); } });
  on("phone", (d) => { if (S.st) { S.st.outbox = (S.st.outbox || []).concat([d]).slice(-5); renderPhone(); } });
  on("epoch", () => { setTimeout(load, 800); });
  on("backtest.finished", () => { setTimeout(load, 500); });
  on("liquidation", () => { if (S.raw) { S.raw.liquidating = true; renderLanes(); } });
  on("liquidated", () => { if (S.raw) { S.raw.liquidated = true; S.raw.liquidating = false; renderLanes(); } });
  on("breach", () => {});
  es.onerror = () => { $("#live-dot").classList.add("off"); };
  es.onopen = () => { if (S.feed && S.feed.connected) $("#live-dot").classList.remove("off"); };
}

// ---------------------------------------------------------------- load
async function load() {
  let st;
  try { st = await get(API + "/state"); }
  catch (e) {
    const off = $("#offline"); off.hidden = false;
    off.textContent = "The race is not running: " + (e.body && e.body.detail || e.message) + ". Start it with scripts/serve.sh (it runs the race's own exchange on tcp 7020 and venued on 7030).";
    $("#live-dot").classList.add("off"); $("#feed-line").textContent = "offline";
    setTimeout(load, 10000); return;
  }
  $("#offline").hidden = true;
  S.st = st; S.cfg = st.config; S.route = st.route; S.model = st.model; S.feed = st.feed; S.epoch = st.epoch.id; S.marketNow = st.market_t || null;
  S.prints = st.tape.recent || []; S.printsTotal = st.tape.prints && st.tape.prints.n || S.prints.length; S.violations = st.tape.prints && st.tape.prints.violations || 0;
  S.equity = st.equity || []; S.fills = st.fills || []; S.raw = st.raw; S.bnd = st.bounded; S.pending = st.pending_messages || 0;
  S.rounds = new Map((st.rounds || []).map((r) => [r.round_id, r]));
  S.roundN = st.epoch.rounds; const lastR = (st.rounds || [])[st.rounds.length - 1]; S.lastRoundT = lastR ? (lastR.ended_t || lastR.t) : 0; S.inRound = !!st.current_round;
  if (st.tape.last_ticks != null && !S.prints.length) S.prints.push([st.tape.last_print_t || Date.now(), st.tape.last_price || st.tape.last_ticks, st.tape.last_ticks, 0]);
  if (MODE === "backtest") document.title = "The Bounded Key Trading Race · Backtest";
  renderHeader(); renderLanes(); renderPrice(); renderRoundState(); renderCharts(); renderRounds(); renderHistory(); renderIdleHero();
  connect(st.last_event_id);
}

setInterval(() => { renderRoundState(); if (MODE === "backtest") renderBacktestBanner(); }, 1000);
setInterval(async () => { try { const s = await get(API + "/status"); S.feed = s.feed; if (S.st) { S.st.feed = s.feed; renderHeader(); } } catch (_) {} }, 20000);
let rsz; window.addEventListener("resize", () => { clearTimeout(rsz); rsz = setTimeout(renderCharts, 200); });
load();
})();
