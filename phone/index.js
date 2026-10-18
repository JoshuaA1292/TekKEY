// The owner's phone: a Node sidecar on Photon 2.0 Spectrum (spectrum-ts 12.11.0).
//
// What is real: with PHOTON_PROJECT_ID + PHOTON_PROJECT_SECRET the sidecar opens a Spectrum
// session on the project's iMessage/SMS line (`Spectrum({projectId, projectSecret,
// providers: [imessage.config()]})`), streams inbound messages with
// `for await (const [space, message] of app.messages)`, forwards each text to the gateway's
// `POST /photon/text`, and sends outbound texts with `space.send(text)` on a space resolved
// from the owner's number (`imessage(app).space.create("+1...")`).
//
// What is not: voice calls. Spectrum 2.0 voice is SIP only (sip.spectrum.photon.codes, TLS 5061,
// username = project id, password = project secret) and the docs say call control through
// spectrum-ts is "coming soon" and calls never appear in `app.messages`. So `POST /send` with
// `call: true` delivers the text and reports `voice: "unsupported"`, and `POST /voice` is the hook
// a SIP application (or a transcriber) calls with a transcript; the gateway's spoken answer is
// relayed as a text on the same line so the read-back still reaches the owner.
//
// Without credentials the sidecar runs in "log" mode: every /send is logged and kept in /outbox,
// /health says live: false, and nothing pretends to be delivered.
import http from "node:http";
import { pathToFileURL } from "node:url";

const DEFAULT_PORT = 8787;

export function createSidecar({
  gatewayUrl = process.env.GATEWAY_URL || "http://127.0.0.1:8080",
  projectId = process.env.PHOTON_PROJECT_ID || "",
  projectSecret = process.env.PHOTON_PROJECT_SECRET || "",
  spectrum = null,            // tests inject { app, im } here; otherwise connect() builds them
  log = console,
} = {}) {
  const base = gatewayUrl.replace(/\/+$/, "");
  const state = {
    live: false,
    mode: "log",
    platform: "imessage",
    project: projectId ? { id: projectId, name: null, slug: null, line: null } : null,
    gateway: base,
    started_t: Date.now(),
    inbound: 0,
    outbound: 0,
    last_error: null,
    voice: "unsupported: Spectrum voice is SIP only (sip.spectrum.photon.codes:5061); no SDK call control yet",
  };
  const outbox = [];
  const seen = new Set();
  let app = spectrum?.app ?? null;
  let im = spectrum?.im ?? null;

  function remember(entry) {
    outbox.push(entry);
    if (outbox.length > 200) outbox.shift();
    return entry;
  }

  // ---- Spectrum session -------------------------------------------------------------------
  async function connect() {
    if (app) return app;
    if (!projectId || !projectSecret) {
      log.warn("[phone] PHOTON_PROJECT_ID / PHOTON_PROJECT_SECRET unset: log mode, nothing is delivered");
      return null;
    }
    const { Spectrum, cloud } = await import("spectrum-ts");
    const { imessage } = await import("spectrum-ts/providers/imessage");
    app = await Spectrum({ projectId, projectSecret, providers: [imessage.config()], telemetry: false });
    im = imessage(app);
    state.live = true;
    state.mode = "photon";
    state.project.name = app.config?.name ?? null;
    state.project.slug = app.config?.slug ?? null;
    try {
      const info = await cloud.getImessageInfo(projectId);
      state.project.line = info?.type ? `${info.type} iMessage line` : null;
    } catch (e) {
      log.warn("[phone] could not read iMessage line info:", e?.message ?? e);
    }
    log.info(`[phone] Spectrum connected: project ${state.project.name ?? projectId} (${state.project.line ?? "iMessage"})`);
    return app;
  }

  // ---- outbound ------------------------------------------------------------------------------
  async function send({ to, text, call = false }) {
    const entry = { to, text, call, t: Date.now(), delivered: false, via: "log", voice: call ? "unsupported" : undefined };
    if (!to || !text) {
      return { ...entry, error: "to and text are required", status: 400 };
    }
    if (!im) {
      log.info(`[phone] LOG ${call ? "CALL" : "TEXT"} -> ${to}: ${text}`);
      return remember(entry);
    }
    try {
      const space = await im.space.create(to);
      const msg = await space.send(text);
      state.outbound += 1;
      return remember({ ...entry, delivered: true, via: "photon", message_id: msg?.id ?? null });
    } catch (e) {
      state.last_error = String(e?.message ?? e);
      log.error("[phone] send failed:", state.last_error);
      return remember({ ...entry, via: "photon", error: state.last_error, status: 502 });
    }
  }

  // ---- inbound -------------------------------------------------------------------------------
  async function gatewayPost(path, body) {
    const r = await fetch(`${base}${path}`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify(body),
    });
    const data = await r.json().catch(() => ({}));
    if (!r.ok) throw new Error(`gateway ${path} -> ${r.status} ${JSON.stringify(data).slice(0, 200)}`);
    return data;
  }

  function textOf(content) {
    if (!content) return null;
    if (content.type === "text") return content.text;
    if (content.type === "markdown") return content.markdown;
    return null;
  }

  async function handleInbound(space, message) {
    if (message?.direction === "outbound") return null;
    if (message?.id) {
      if (seen.has(message.id)) return null;          // at-least-once delivery: dedupe on the id
      seen.add(message.id);
      if (seen.size > 5000) seen.delete(seen.values().next().value);
    }
    const from = message?.sender?.id;
    if (!from) return null;
    if (state.project && !state.project.line_number && space?.phone) state.project.line_number = space.phone;
    state.inbound += 1;
    const body = textOf(message.content);
    if (body == null) {
      if (message.content?.type === "voice") {
        // A voice memo arrives as audio; the SDK carries no transcript. Say so rather than guess.
        await space.send("I received a voice memo but cannot transcribe it here. Text `freeze` or `status`, or call the line.");
      }
      return null;
    }
    try {
      const res = await gatewayPost("/photon/text", { from_number: from, body });
      // The gateway already sent its reply through POST /send when it could (res.delivered);
      // relay it here only when it could not, so the owner never gets it twice.
      if (res?.reply && !res.delivered) {
        await space.send(res.reply);
        state.outbound += 1;
      }
      return res;
    } catch (e) {
      state.last_error = String(e?.message ?? e);
      log.error("[phone] inbound forward failed:", state.last_error);
      return null;
    }
  }

  async function listen() {
    if (!app) return;
    try {
      for await (const [space, message] of app.messages) {
        handleInbound(space, message).catch((e) => log.error("[phone] inbound handler:", e));
      }
    } catch (e) {
      state.last_error = `message stream ended: ${e?.message ?? e}`;
      state.live = false;
      log.error("[phone]", state.last_error);
    }
  }

  async function handleVoice({ from_number, transcript, session_id }) {
    const res = await gatewayPost("/photon/voice", { from_number, transcript, session_id: session_id || from_number });
    let spoken = { via: "log" };
    if (res?.say && from_number) spoken = await send({ to: from_number, text: res.say });
    return { ...res, spoken_via: spoken.via, spoken_delivered: !!spoken.delivered };
  }

  // ---- HTTP ------------------------------------------------------------------------------------
  function json(res, status, body) {
    res.writeHead(status, { "content-type": "application/json" });
    res.end(JSON.stringify(body));
  }

  async function readJson(req) {
    const chunks = [];
    for await (const c of req) chunks.push(c);
    const raw = Buffer.concat(chunks).toString("utf8");
    return raw ? JSON.parse(raw) : {};
  }

  const server = http.createServer(async (req, res) => {
    try {
      const url = new URL(req.url, "http://localhost");
      if (req.method === "GET" && url.pathname === "/health") {
        return json(res, 200, { ok: true, ...state, uptime_s: Math.round((Date.now() - state.started_t) / 1000) });
      }
      if (req.method === "GET" && url.pathname === "/outbox") return json(res, 200, outbox.slice(-50));
      if (req.method === "POST" && url.pathname === "/send") {
        const out = await send(await readJson(req));
        return json(res, out.status ?? 200, out);
      }
      if (req.method === "POST" && url.pathname === "/voice") {
        return json(res, 200, await handleVoice(await readJson(req)));
      }
      return json(res, 404, { error: "not_found" });
    } catch (e) {
      return json(res, 500, { error: String(e?.message ?? e) });
    }
  });

  return { state, outbox, server, connect, listen, send, handleInbound, handleVoice };
}

export async function main() {
  const port = Number(process.env.PHONE_SIDECAR_PORT || DEFAULT_PORT);
  const sidecar = createSidecar();
  try {
    await sidecar.connect();
  } catch (e) {
    sidecar.state.last_error = String(e?.message ?? e);
    console.error("[phone] Spectrum connect failed, staying in log mode:", sidecar.state.last_error);
  }
  await new Promise((resolve) => sidecar.server.listen(port, "127.0.0.1", resolve));
  console.log(`[phone] sidecar on http://127.0.0.1:${port} (${sidecar.state.mode} mode, gateway ${sidecar.state.gateway})`);
  sidecar.listen();
  const stop = async () => {
    sidecar.server.close();
    try { await sidecar.connect().then((app) => app?.stop?.()); } catch { /* already down */ }
    process.exit(0);
  };
  process.on("SIGINT", stop);
  process.on("SIGTERM", stop);
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  main().catch((e) => { console.error(e); process.exit(1); });
}
