(() => {
"use strict";
const $ = (s, el = document) => el.querySelector(s);
const el = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; return e; };
const usd = (c, sign = true) => { if (c == null) return "—"; const neg = c < 0; c = Math.abs(Math.round(c)); const s = "$" + Math.floor(c / 100).toLocaleString("en-US") + "." + String(c % 100).padStart(2, "0"); return (neg ? "−" : sign ? "+" : "") + s; };
const dur = (ms) => { const m = Math.round(ms / 60000); return m >= 60 ? `${Math.floor(m / 60)}h ${m % 60}m` : `${m}m`; };
const day = (t) => t ? new Date(t).toISOString().slice(0, 10) : "—";

async function get(p) { const r = await fetch(p, { headers: { accept: "application/json" } }); if (!r.ok) return null; return r.json(); }

function cell(v, cls) { const td = el("td", cls || ""); if (v instanceof Node) td.append(v); else td.textContent = v; return td; }
function signed(c) { const s = el("span", c > 0 ? "pos" : c < 0 ? "neg" : ""); s.textContent = usd(c); return s; }

const routeFor = (model, route) => /^claude-/.test(model || "") ? "anthropic-direct" : /^grok-/.test(model || "") ? "chalk" : (route || "—");
const isCrash = (r) => /flash crash/i.test(r.label || "");

function modelCell(r) { const sp = el("span"); sp.append(document.createTextNode(r.model || "—"), el("span", "via", "via " + routeFor(r.model, r.route))); return sp; }

function row(r) {
  const tr = el("tr", isCrash(r) ? "crash" : "");
  const failed = (r.raw.actions.failed || 0) + (r.bounded.actions.failed || 0);
  const name = el("span"); name.append(document.createTextNode(r.label));
  name.append(el("span", "flag " + (r.in_progress ? "live" : failed ? "dirty" : "clean"), r.in_progress ? "in progress" : failed ? `incomplete: ${failed} failed calls` : "clean"));
  const mk = r.market || {};
  tr.append(cell(name), cell(modelCell(r), "model"), cell(r.mode === "live" ? "live, " + day(r.started_t) : day(mk.from_t)),
            cell(mk.from_t ? dur((mk.to_t || mk.from_t) - mk.from_t) : "—", "n"), cell(String(r.rounds), "n"), cell(String((r.messages || {}).attack || 0), "n"),
            cell(mk.low != null ? `${mk.low}–${mk.high}` : "—", "n"), cell(String(mk.violations || 0), "n"),
            cell(signed(r.raw.pnl_cents), "n raw-ink"), cell(usd(-r.raw.max_drawdown_cents, false).replace("−", "−"), "n"), cell(String(r.raw.fills), "n"), cell(usd(r.raw.fees_cents, false), "n"),
            cell(r.raw.liquidated ? "yes" : "no", "n"), cell(Object.entries(r.raw.breaches || {}).filter(([, v]) => v && v.count).map(([k, v]) => `${k}×${v.count}`).join(" ") || "0", "n"),
            cell(signed(r.bounded.pnl_cents), "n bnd-ink"), cell(usd(-r.bounded.max_drawdown_cents, false), "n"), cell(String(r.bounded.fills), "n"), cell(usd(r.bounded.fees_cents, false), "n"),
            cell(`${r.bounded.programs} / ${(r.bounded.http || {})["200"] || 0} / ${(r.bounded.http || {})["422"] || 0} / ${(r.bounded.http || {})["409"] || 0}`, "n"),
            cell(String(r.bounded.certificate_breaches), "n " + (r.bounded.certificate_breaches ? "neg" : "pos")),
            cell(r.bounded.worst_realized_vs_bound ? `${usd(r.bounded.worst_realized_vs_bound[0], false)} of ${usd(r.bounded.worst_realized_vs_bound[1], false)}` : "—", "n"));
  return tr;
}

async function main() {
  const [live, bt] = await Promise.all([get("/race/compare"), get("/backtest/compare")]);
  const rows = [];
  if (live) for (const r of live.rows) rows.push(r);
  if (bt) for (const r of bt.rows) rows.push(r);
  const tbl = $("#tbl");
  const head = el("thead"), hr = el("tr");
  for (const h of ["run", "model", "when", "span", "rounds", "attacks", "ETH range", "gap > model", "ordinary P&L", "max drawdown", "fills", "fees", "liquidated", "rule breaches", "bounded P&L", "max drawdown", "fills", "fees", "programs / 200 / 422 / 409", "cert. breaches", "worst realized vs cert."])
    hr.append(el("th", /P&L|drawdown|fills|fees|rounds|attacks|range|gap|programs|breaches|worst|span/.test(h) ? "n" : "", h));
  head.append(hr); tbl.append(head);
  const body = el("tbody");
  const ordered = rows.filter((r) => r.in_progress || r.finished).sort((a, b) => (b.in_progress ? 1 : 0) - (a.in_progress ? 1 : 0) || (b.started_t - a.started_t));
  for (const r of ordered) body.append(row(r));
  tbl.append(body);
  // KPIs: the three scenarios = the live run in progress + the latest clean finished backtests
  const clean = ordered.filter((r) => !r.in_progress && !((r.raw.actions.failed || 0) + (r.bounded.actions.failed || 0)));
  const liveRow = ordered.find((r) => r.mode === "live");
  const kp = $("#kpis");
  const k = (label, v, sub) => { const d = el("div", "kpi"); d.append(el("div", "k", label), el("div", "v", v), el("div", "small muted", sub || "")); kp.append(d); };
  // The ledger numbers of a window are real whether or not every round got a model answer (a failed call means
  // both lanes held that round), so the headline counts cover EVERY finished window plus the live race.
  const all = ordered.filter((r) => !r.in_progress).concat(liveRow ? [liveRow] : []);
  const cb = all.reduce((a, r) => a + r.bounded.certificate_breaches, 0), progs = all.reduce((a, r) => a + r.bounded.ended, 0);
  k("certificate breaches", `${cb} of ${progs}`, `programs whose realized loss exceeded their certificate, across all ${all.length} runs (every finished window and the live race)`);
  const rb = all.reduce((a, r) => a + Object.values(r.raw.breaches || {}).reduce((x, v) => x + (v ? (typeof v === "number" ? v : v.count || 0) : 0), 0), 0);
  const rbRuns = all.filter((r) => Object.values(r.raw.breaches || {}).some((v) => v && (typeof v === "number" ? v : v.count))).length;
  k("ordinary key rule breaches", String(rb), `times the ordinary account was outside the owner's rules (lots, stop, daily loss), in ${rbRuns} of ${all.length} runs`);
  const worst = all.map((r) => r.raw.max_drawdown_cents).reduce((a, b) => Math.max(a, b), 0), worstB = all.map((r) => r.bounded.max_drawdown_cents).reduce((a, b) => Math.max(a, b), 0);
  k("worst drawdown, ordinary vs bounded", `${usd(-worst, false)} vs ${usd(-worstB, false)}`, "deepest equity dip in any run; the bounded lane's is capped by its 24 h budget by construction");
  // the flash-crash pair: the finished backtest with the largest ordinary loss
  const crashRows = ordered.filter((r) => r.mode !== "live" && r.finished && !r.in_progress);
  const cr = crashRows.slice().sort((a, b) => a.raw.pnl_cents - b.raw.pnl_cents)[0];
  if (cr) {
    const d = el("div", "kpi crash"); d.append(el("div", "k", "the flash-crash pair: largest ordinary loss in a finished backtest"));
    const v = el("div", "v two"); v.append(el("span", "raw-ink", usd(cr.raw.pnl_cents)), document.createTextNode(" vs "), el("span", "bnd-ink", usd(cr.bounded.pnl_cents))); d.append(v);
    const brs = Object.entries(cr.raw.breaches || {}).filter(([, x]) => x && x.count).map(([k2, x]) => `${k2.replace("_", " ")} ×${x.count}`).join(", ");
    const sub = el("div", "small muted");
    sub.append(document.createTextNode(`#${cr.epoch} · ${cr.label} · ${cr.model} via ${routeFor(cr.model, cr.route)} · ordinary rule breaches: ${brs || "none"} · bounded certificate breaches: ${cr.bounded.certificate_breaches} · `));
    const a = el("a", "", "watch it again →"); a.href = "race.html?mode=backtest#replay"; a.title = "replays the same five hours on a model of your choice (public, rate-limited)"; sub.append(a);
    d.append(sub); kp.append(d);
  }
  const models = [...new Set(ordered.map((r) => r.model).filter(Boolean))];
  $("#note").textContent = `${ordered.length} runs · ${clean.length} clean finished · ${models.length === 1 ? "model " + models[0] : models.length + " models: " + models.join(", ")}`;
}
main().catch((e) => { $("#note").textContent = "could not load: " + e.message; });
})();
