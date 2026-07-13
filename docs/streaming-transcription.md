# Streaming transcription — design exploration

Status: **exploration, nothing here is shipped.** This document weighs how
TapTalk could stop waiting for the whole clip before transcribing, and what
that costs.

## The problem

Today the timeline is strictly serial:

```
hold ── speak (up to 120 s) ── release ── upload whole WAV ── server transcribes ── type
```

Nothing leaves the device until the finger lifts. For a 60-second dictation
that is ~1.9 MB of WAV; at a realistic ESP32-S3 TLS upload rate the upload
alone is seconds, then the server still has to transcribe a full minute of
audio. The user stares at "Transcribing…" for something like 5–10 seconds,
and the wait grows with the length of the dictation. (These are estimates;
measuring real upload throughput and server turnaround on hardware is step
zero of any implementation.)

The insight: almost all of that work could have happened *while the user was
still talking*. Speech has pauses. Each pause is a place where the audio so
far can be cut, shipped, and transcribed while the microphone keeps rolling.

## The constraint that shapes everything

TapTalk is a keyboard. A keyboard cannot take a word back without a volley of
backspaces, so **anything typed must be final**. True streaming STT engines
revise their partial hypotheses constantly — that is why the README rules out
streaming for *typing*. This exploration does not fight that constraint; it
works inside it: only transcripts of **closed, immutable audio segments** are
ever treated as final text.

## Options considered

### A. VAD-segmented pipelining — recommended

Cut the recording at natural pauses into contiguous segments. Each closed
segment is uploaded as its own complete, ordinary
`POST /v1/audio/transcriptions` request while recording continues. On
release, only the tail segment remains to upload and transcribe.

- Works with **every backend that works today** — same endpoint, same
  multipart format, same config. A segment is just a shorter clip.
- No revisions ever: a segment is closed before it is sent, its transcript is
  final by construction.
- Two delivery modes fall out of the same machinery (see below): show
  partials on screen and type once at the end, or type each part as it lands.

### B. True streaming protocol (WebSocket / Realtime APIs) — ruled out

Real partial hypotheses, lowest theoretical latency. But every provider has
its own WS protocol and JSON framing (the device deliberately carries no JSON
parser), it abandons the "any OpenAI-compatible endpoint" story that is the
product's spine, and the revisable partials it produces are exactly what a
keyboard cannot type. Wrong tool for this device.

### C. One chunked-transfer POST, streamed while recording — ruled out

Keep a single request but start sending the body during the hold
(Transfer-Encoding: chunked, since Content-Length is unknown until release).
Saves only the upload leg — the server cannot transcribe until the body
completes — and a 90-second hold means a 90-second open request, which trips
server/proxy timeouts. Chunked multipart is also the least-supported corner
of OpenAI-compatible shims. High compatibility risk, small win.

## Design of Option A

### Segmentation: contiguous partitions, cut inside pauses

Segments are **contiguous partitions of the clip** — no gaps, no overlap.
Every PCM byte is uploaded exactly once, so the concatenated transcripts
cover exactly what was said. VAD never discards audio; it only chooses
*where to cut*, and it cuts in the middle of a silence run so no word is ever
split at a boundary that mattered.

The capture task already computes a peak per 20 ms chunk. An energy VAD on
top of that is a small pure-C module:

- `core/vad.c`: consumes one peak per chunk, emits "cut here" decisions.
  Host-testable with synthetic peak sequences, like every other `core/` module.
- Cut when: silence run ≥ ~600 ms **and** the open segment is ≥ ~3 s.
  Cut point = middle of the silence run.
- Forced cut at ~20 s even without a pause, placed at the quietest chunk in
  the last 2 s, so one breathless speaker cannot re-create today's
  wait-for-everything behaviour.
- Threshold: start with the empirical numbers already in the tree (speech
  peaks ~2000 at 42 dB gain, the old silence guard sat at 600); consider an
  adaptive noise floor (rolling low-percentile of chunk peaks) if fixed
  thresholds prove brittle on hardware.

Parameters live in the header, tests pin the behaviour.

### Silence-only segments

A long pause between sentences produces an interior segment that is entirely
below threshold. Whisper-class models *hallucinate* on pure silence ("thank
you", "you"), so all-silent interior segments are **skipped, not uploaded**.
The existing philosophy — a deliberate hold uploads even if quiet — is
preserved at the clip level: if recording ends and *no* segment was ever
worth sending, upload the whole clip as one segment, exactly today's
behaviour, and let the server say it heard nothing.

### Transport: one worker, one connection, strict FIFO

- Segments are descriptors (`offset`, `len`, `final?`) into the same
  append-only PSRAM clip, queued to a single STT worker task (same core-0,
  prio-5 placement as today's one-shot task).
- **Exactly one request in flight.** Ordering falls out for free, typing
  order is guaranteed, and serial requests are the friendly pattern for local
  whisper.cpp-style servers that process one job at a time anyway.
- **Connection reuse is the whole ballgame.** A TLS handshake on this chip
  costs on the order of a second; per-segment handshakes would eat the win.
  `esp_http_client` supports keep-alive across sequential requests on one
  handle — don't close between segments, reconnect transparently when the
  server drops the connection. This needs hardware validation against both
  OpenAI and a local server.
- Each segment gets its own 44-byte WAV header (`wav_write_header` into a
  stack buffer) followed by its PCM window streamed straight from PSRAM —
  the existing zero-copy property is untouched.

Concurrency note: today the state machine guarantees capture and network
never overlap; with pipelining they genuinely do. It stays safe because the
clip is append-only and a segment is closed (cursor already past it) before
its descriptor is queued — the FreeRTOS queue provides the memory barrier.
The capture task writes only ahead of every queued window.

### Context across segments: prompt chaining

Independent segments lose cross-segment context — punctuation and
capitalisation suffer at boundaries. The transcriptions API has a `prompt`
field ("continue a previous audio segment") that exists for exactly this:
send the tail (~200 chars) of the accumulated transcript as the prompt for
the next segment. `multipart_build()` grows one optional field; servers that
ignore `prompt` are unharmed. This is the standard chunked-Whisper trick and
substantially repairs boundary quality.

### Two delivery modes, one mechanism

**Mode 1 — pipeline (type at the end).** Partial transcripts accumulate on
the device and appear on the AMOLED as they arrive (the screen may freely
revise; only the keyboard may not). Typing happens once, on release, when the
tail segment's text lands. Nothing about the HID story changes — the host
still receives one final, ordered dictation. Perceived latency after release
drops from "proportional to dictation length" to roughly "tail-segment upload
+ transcription", a near-constant couple of seconds.

**Mode 2 — live typing (opt-in).** Each segment's transcript is typed as it
arrives, while the user is still speaking. Text you can't take back is being
committed mid-sentence, and even with prompt chaining a boundary will
occasionally read worse than the whole-clip transcript would have. That is a
taste trade-off, which is why it is a mode and not the default.

Rough post-release latency, 60 s dictation (estimates to verify on hardware):

| | upload after release | transcribe after release | felt wait |
|---|---|---|---|
| today | ~1.9 MB | full 60 s of audio | ~5–10 s |
| mode 1/2 | tail only (~150 KB) | tail only (~5 s of audio) | ~1.5–3 s |

### State machine changes (small, and host-testable)

No new states in mode 1 — only new events and actions:

- `EV_SEG_READY` — capture posts it when VAD closes a segment;
  `ST_RECORDING` handles it with a new `ACT_SEG_ENQUEUE`.
- `EV_STT_PART` — a non-final segment's transcript arrived; drives the
  on-screen partial (and, in mode 2, `ACT_TYPE_PART`). Ignored-but-legal in
  `ST_UPLOADING` while the queue drains.
- `EV_STT_OK` keeps its meaning of "the dictation's text is complete":
  the worker posts it when the *final* segment (enqueued by record-end)
  comes back. `ST_UPLOADING` becomes "draining the tail" rather than
  "uploading everything".

`sm_step()` stays pure; `test_sm.c` grows the new transitions the same way it
covers the existing ones.

### Text assembly, Send and Undo

- Parts are `textnorm_clean`ed individually and joined with a single space;
  the accumulator lives in PSRAM and is sized for a full two-minute
  dictation (~8 KB) rather than today's 1 KB (see "found along the way").
- Mode 2 changes Undo's bookkeeping: today `hid_kbd_undo()` reverses the
  last `hid_kbd_type()` call. With incremental typing, the counter must
  accumulate across all parts of one dictation — reset at record-start,
  accumulate per part, still "undoable once". On a mid-dictation failure the
  already-typed parts stay on the host (auto-untyping someone's sentence is
  worse), the error badge explains, and Undo is armed so one tap clears the
  partial text.

### Failure policy

Any segment request fails (after one transparent reconnect-and-retry, since
keep-alive connections die naturally): the dictation fails — `EV_STT_FAIL`,
error badge, exactly today's story. Mode 1 has typed nothing at that point;
mode 2 keeps what was typed and arms Undo, as above. Per-segment empty
responses are skipped silently, not errors.

### UI

- Partial transcript text on screen as it arrives — this is the biggest UX
  win of mode 1 and costs one new `ui_set_partial(text)` in the existing
  mutex-struct-poll pattern.
- The spectrum analyser keeps running; a subtle "N in flight" marker only if
  hardware testing shows people wonder whether it is working.

### Configuration

One new small-int enum in NVS (append-only, like `send_key`):
`stt_stream = off | stream | live`. Portal gains three radio buttons.
Default **off** at first — the pipeline touches capture/network overlap that
has to earn hardware confidence before it is anyone's default. Flipping the
default to `stream` later is a one-line change once validated.

## What this deliberately does not do

- No local STT, no local partials — the device still transcribes nothing.
- No auto-stop. The button is the endpointer. (The VAD module would be the
  natural home for a future tap-to-toggle mode that ends on N s of silence —
  "control activity" taken further — but that is a product decision, not
  this change.)
- No ring buffer. The 120 s linear clip stays. Freeing already-uploaded
  segments would make dictation length unbounded; noted as future work, not
  worth the concurrency surface now.

## Hardware validation list (before any default flips)

1. Real TLS upload throughput and per-request turnaround, OpenAI + local.
2. Keep-alive reuse across sequential requests in `esp_http_client`, and
   recovery when the server closes the connection mid-dictation.
3. Wi-Fi TX concurrent with I2S capture + LVGL flush: watch the diagnostics
   heartbeat for capture overruns (core split already matches: net on 0,
   audio/LVGL on 1, but PSRAM bandwidth is shared).
4. VAD thresholds against real rooms: fan noise, quiet speakers.
5. Segment-boundary transcript quality with and without prompt chaining.

## Test plan (host, `test/host/`)

- `test_vad.c` — synthetic peak sequences: onset, hangover, min-segment,
  forced cut placement, cut-at-quietest, all-silent clip fallback.
- `test_sm.c` — the new events in every state that can see them.
- `test_multipart.c` + `interop.py` — the optional `prompt` field, framing
  re-checked by Python's `email` parser.
- `test_textnorm.c` — part joining: spacing, empty parts, whitespace-only
  parts.
- Optional: a `tools/mock_stt.py` local server for end-to-end bench without
  burning API credit.

## Suggested phasing

1. **Phase 1 — pipeline mode**: `core/vad.c`, segment queue + keep-alive
   worker, prompt chaining, on-screen partials, type-at-end. All the risk
   retired, zero change to what the host receives.
2. **Phase 2 — live typing**: `ACT_TYPE_PART`, Undo accumulation, the
   failure policy above. Small delta on top of phase 1.

## Found along the way

`TRANSCRIPT_CAP` is 1024 bytes while the clip cap is 120 s. Two minutes of
English is roughly 300 words ≈ 2 KB — a max-length dictation's transcript is
silently truncated today, independent of anything in this document. Worth
fixing on its own.
