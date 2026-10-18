// node --test: the sidecar against a fake gateway and a fake Spectrum session. No credentials.
import assert from "node:assert/strict";
import http from "node:http";
import { test } from "node:test";

import { createSidecar } from "../index.js";

function fakeGateway(handlers) {
  const calls = [];
  const server = http.createServer(async (req, res) => {
    const chunks = [];
    for await (const c of req) chunks.push(c);
    const body = JSON.parse(Buffer.concat(chunks).toString() || "{}");
    calls.push({ path: req.url, body });
    const out = handlers[req.url]?.(body) ?? { error: "nope" };
    res.writeHead(handlers[req.url] ? 200 : 404, { "content-type": "application/json" });
    res.end(JSON.stringify(out));
  });
  return new Promise((resolve) => server.listen(0, "127.0.0.1", () => resolve({ server, calls, url: `http://127.0.0.1:${server.address().port}` })));
}

function fakeSpectrum(inbound = []) {
  const sent = [];
  const space = { id: "chat-1", phone: "+15550009999", send: async (text) => { sent.push(text); return { id: `m${sent.length}` }; } };
  const im = { space: { create: async (to) => ({ ...space, to }) } };
  const app = { config: { name: "demo", slug: "demo-1" }, messages: (async function* () { for (const m of inbound) yield [space, m]; })() };
  return { app, im, sent };
}

async function post(url, body) {
  const r = await fetch(url, { method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify(body) });
  return { status: r.status, body: await r.json() };
}

test("log mode: /send is kept in the outbox and reports delivered:false, call reports voice unsupported", async () => {
  const sc = createSidecar({ gatewayUrl: "http://127.0.0.1:1", log: { info() {}, warn() {}, error() {} } });
  await new Promise((r) => sc.server.listen(0, "127.0.0.1", r));
  const url = `http://127.0.0.1:${sc.server.address().port}`;
  const health = await (await fetch(`${url}/health`)).json();
  assert.equal(health.live, false);
  assert.equal(health.mode, "log");
  const out = await post(`${url}/send`, { to: "+15550001111", text: "hi", call: true });
  assert.equal(out.status, 200);
  assert.equal(out.body.delivered, false);
  assert.equal(out.body.via, "log");
  assert.equal(out.body.voice, "unsupported");
  const outbox = await (await fetch(`${url}/outbox`)).json();
  assert.equal(outbox.length, 1);
  sc.server.close();
});

test("photon mode: /send resolves a space from the number and sends on it", async () => {
  const fs = fakeSpectrum();
  const sc = createSidecar({ gatewayUrl: "http://127.0.0.1:1", spectrum: fs, log: { info() {}, warn() {}, error() {} } });
  const out = await sc.send({ to: "+15550001111", text: "Account 1: budget low." });
  assert.equal(out.delivered, true);
  assert.equal(out.via, "photon");
  assert.equal(out.message_id, "m1");
  assert.deepEqual(fs.sent, ["Account 1: budget low."]);
});

test("inbound text is forwarded to /photon/text and the reply relayed only when the gateway could not send it", async () => {
  const gw = await fakeGateway({
    "/photon/text": (b) => (b.body === "status" ? { reply: "Account 1: 0 live.", delivered: false, via: "log" } : { reply: "already sent", delivered: true, via: "photon" }),
  });
  const fs = fakeSpectrum([
    { id: "a", direction: "inbound", sender: { id: "+15550001111" }, content: { type: "text", text: "status" } },
    { id: "a", direction: "inbound", sender: { id: "+15550001111" }, content: { type: "text", text: "status" } },   // duplicate delivery
    { id: "b", direction: "inbound", sender: { id: "+15550001111" }, content: { type: "text", text: "freeze" } },
    { id: "c", direction: "outbound", sender: { id: "+15550009999" }, content: { type: "text", text: "echo" } },
  ]);
  const sc = createSidecar({ gatewayUrl: gw.url, spectrum: fs, log: { info() {}, warn() {}, error() {} } });
  await sc.listen();
  await new Promise((r) => setTimeout(r, 50));
  assert.deepEqual(gw.calls.map((c) => c.body), [
    { from_number: "+15550001111", body: "status" },
    { from_number: "+15550001111", body: "freeze" },
  ]);
  assert.deepEqual(fs.sent, ["Account 1: 0 live."]);
  assert.equal(sc.state.inbound, 2);
  gw.server.close();
});

test("/voice forwards a transcript to /photon/voice and relays the spoken answer as a text", async () => {
  const gw = await fakeGateway({ "/photon/voice": (b) => ({ say: `heard: ${b.transcript}`, state: "awaiting_confirm" }) });
  const fs = fakeSpectrum();
  const sc = createSidecar({ gatewayUrl: gw.url, spectrum: fs, log: { info() {}, warn() {}, error() {} } });
  await new Promise((r) => sc.server.listen(0, "127.0.0.1", r));
  const url = `http://127.0.0.1:${sc.server.address().port}`;
  const out = await post(`${url}/voice`, { from_number: "+15550001111", transcript: "forty dollars a day", session_id: "call-1" });
  assert.equal(out.body.say, "heard: forty dollars a day");
  assert.equal(out.body.spoken_via, "photon");
  assert.deepEqual(gw.calls[0].body, { from_number: "+15550001111", transcript: "forty dollars a day", session_id: "call-1" });
  assert.deepEqual(fs.sent, ["heard: forty dollars a day"]);
  sc.server.close();
  gw.server.close();
});
