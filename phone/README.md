# phone/: the owner's phone on Photon 2.0 Spectrum

The gateway's owner-facing channel (`BUILD_B_GATEWAY.md` B7) lives on a **Photon Spectrum
iMessage/SMS line**. This directory is a small Node sidecar on the real SDK, `spectrum-ts` 12.11.0
(npm, from github.com/photon-hq/spectrum-ts), that the Python gateway talks to over localhost.

```
owner's iPhone  <-- iMessage/SMS -->  Photon Spectrum (project line)  <-- spectrum-ts stream -->  phone/index.js
                                                                                                        |
                                                             POST /send  <-- gateway/photon.py          |  POST /photon/text
                                                             (alarms, replies)                          v  POST /photon/voice
                                                                                                   gateway :8080
```

## Which channel the agent lives on

The project's iMessage line: the number Photon assigns to the Spectrum project (shared or
dedicated, see the project page at spectrum.photon.codes). The owner texts that number; alarms
come back from it. `GET /health` on the sidecar reports the project name, slug and line type once
connected, and the line's number as soon as the first inbound message reveals it (`line_number`).

## Run it

```sh
cd phone && npm install
PHOTON_PROJECT_ID=... PHOTON_PROJECT_SECRET=... GATEWAY_URL=http://127.0.0.1:8080 npm start
# -> [phone] sidecar on http://127.0.0.1:8787 (photon mode, gateway http://127.0.0.1:8080)
```

Then start the gateway with the same `PHOTON_PROJECT_ID` (it only needs the id to know the
sidecar is the delivery path) and, optionally, `PHONE_SIDECAR_URL` if the port differs.

| Variable | Meaning | Default |
|---|---|---|
| `PHOTON_PROJECT_ID`, `PHOTON_PROJECT_SECRET` | the Spectrum project (dashboard: project settings) | unset: log mode |
| `GATEWAY_URL` | where `/photon/text` and `/photon/voice` live | `http://127.0.0.1:8080` |
| `PHONE_SIDECAR_PORT` | the sidecar's own port, loopback only | `8787` |

`npm test` runs the sidecar against a fake gateway and a fake Spectrum session (no credentials).

## What it does

| Route | Direction | Real call |
|---|---|---|
| `POST /send {to, text, call}` | gateway -> phone | `imessage(app).space.create(to)` then `space.send(text)`; the reply carries `delivered`, `via: "photon"`, `message_id`. With `call: true` the text is sent and `voice: "unsupported"` is reported (see below) |
| inbound texts | phone -> gateway | `for await (const [space, message] of app.messages)`; each `text`/`markdown` message is POSTed to `GATEWAY_URL/photon/text {from_number, body}`; the gateway's reply is relayed with `space.send` only when the gateway reports it could not deliver it itself (it normally sends through `/send`), so nothing arrives twice. Deliveries are at-least-once; the sidecar dedupes on `message.id` |
| `POST /voice {from_number, transcript, session_id}` | SIP app -> gateway | forwarded to `GATEWAY_URL/photon/voice`; the gateway's `say` (the envelope read-back, "confirm?") is also texted to the owner on the line |
| `GET /health` | | `live`, `mode` (`photon` or `log`), project, line, counters, last error |
| `GET /outbox` | | the last 50 sends (useful in log mode) |

Without credentials the sidecar runs in **log mode**: `/send` logs and keeps the message in
`/outbox`, answers `delivered: false, via: "log"`, and `/health` says `live: false`. The gateway
then falls back to its own outbox (`GET /photon/outbox`). Nothing is reported as delivered unless
Spectrum accepted it.

## Voice, honestly

Spectrum 2.0 voice is SIP, not SDK: calls are placed and received by a SIP application registered
to `sip.spectrum.photon.codes` (TLS on 5061, username = project id, password = project secret;
inbound needs an "inbound route" saved in the dashboard). The docs state that call control through
`spectrum-ts` is coming soon and that calls do not appear in `app.messages` or `app.webhook()`.
This sidecar therefore does not place calls: `call: true` sends the alarm as a text and reports
`voice: "unsupported"`, and the envelope-by-voice flow is exercised through `POST /voice` with a
transcript (from a SIP application with speech-to-text, or by hand in the demo), with the gateway's
spoken answer relayed as a text. The SIP leg itself has not been exercised from this repository.

## Verified and not

Verified offline: the package installs and imports under Node 25; the `Spectrum()` options,
`app.messages` tuple shape, `message.content.type === "text"`, `message.sender.id` (E.164 handle),
`imessage(app).space.create(handle)` and `space.send(text)` were read from the installed type
definitions (`node_modules/@spectrum-ts/core/dist/*.d.ts`, `@spectrum-ts/imessage`). Not verified:
a live round trip on a real project line, because no Photon credentials were available when this
was written. If the project uses webhooks instead of streaming, `app.webhook(request, handler)`
with `webhookSecret` is the documented alternative; this sidecar uses the stream.
