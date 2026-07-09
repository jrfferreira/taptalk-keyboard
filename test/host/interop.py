"""Validate the C-generated WAV and multipart body with independent parsers.

The point is to be judged by code that did not write the bytes: Python's `wave`
module for the RIFF header, and `email` (an RFC 2046 parser) for the multipart
framing. Both are stdlib, so this needs no install.

A field-by-field unit test cannot catch a wrong `data` chunk offset — the
header still reads back correctly to the code that wrote it, while a real
decoder hands you silence.
"""
import email
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

RATE, SECONDS, AMPLITUDE = 16000, 1, 12000
PCM_BYTES = RATE * SECONDS * 2
WAV_BYTES = 44 + PCM_BYTES

failures = []


def check(cond, msg):
    if not cond:
        failures.append(msg)


def check_wav(path: Path) -> None:
    with wave.open(str(path), "rb") as w:
        check(w.getnchannels() == 1, f"channels={w.getnchannels()}, want 1")
        check(w.getsampwidth() == 2, f"sampwidth={w.getsampwidth()}, want 2")
        check(w.getframerate() == RATE, f"framerate={w.getframerate()}, want {RATE}")
        check(w.getnframes() == RATE * SECONDS, f"nframes={w.getnframes()}")
        frames = w.readframes(w.getnframes())

    check(len(frames) == PCM_BYTES, f"decoded {len(frames)} pcm bytes, want {PCM_BYTES}")
    check(path.stat().st_size == WAV_BYTES, f"file {path.stat().st_size}B, want {WAV_BYTES}")

    # The 440 Hz tone must come back out. If the data chunk offset were wrong,
    # the decoder would happily return header bytes reinterpreted as samples.
    samples = struct.unpack(f"<{len(frames) // 2}h", frames)
    peak = max(abs(s) for s in samples)
    check(abs(peak - AMPLITUDE) < 200, f"peak {peak}, want ~{AMPLITUDE} (data offset wrong?)")
    check(samples[0] == 0, f"first sample {samples[0]}, want 0 (sin(0))")
    # A quarter period in (16000/440/4 ≈ 9 samples) the tone should be near peak.
    check(samples[9] > AMPLITUDE * 0.9, f"sample[9]={samples[9]}, tone not intact")


def check_multipart(path: Path, content_type: str, content_length: int) -> list:
    body = path.read_bytes()

    # Getting this wrong is a hang or a 400, and it is invisible on-device.
    check(len(body) == content_length,
          f"Content-Length says {content_length}, body is {len(body)} bytes")

    raw = b"Content-Type: " + content_type.encode() + b"\r\nMIME-Version: 1.0\r\n\r\n" + body
    msg = email.message_from_bytes(raw)
    check(msg.is_multipart(), "stdlib email parser does not see a multipart body")
    if not msg.is_multipart():
        return []

    parts = msg.get_payload()
    names = [p.get_param("name", header="content-disposition") for p in parts]
    by_name = dict(zip(names, parts))

    for want in ("model", "response_format", "language", "file"):
        check(want in by_name, f"missing form field {want!r}")

    if "model" in by_name:
        check(by_name["model"].get_payload().strip() == "gpt-4o-mini-transcribe",
              f"model={by_name['model'].get_payload()!r}")
    if "response_format" in by_name:
        check(by_name["response_format"].get_payload().strip() == "text",
              f"response_format={by_name['response_format'].get_payload()!r}")
    if "language" in by_name:
        check(by_name["language"].get_payload().strip() == "en",
              f"language={by_name['language'].get_payload()!r}")

    if "file" in by_name:
        f = by_name["file"]
        check(f.get_param("filename", header="content-disposition") == "audio.wav",
              "file part filename is not audio.wav (the server sniffs the extension)")
        check(f.get_content_type() == "audio/wav", f"file part type {f.get_content_type()}")
        payload = f.get_payload(decode=True)
        check(payload is not None, "file part has no payload")
        if payload is not None:
            check(len(payload) == WAV_BYTES, f"file part {len(payload)}B, want {WAV_BYTES}")
            check(payload[:4] == b"RIFF", "file part does not start with RIFF")

    # A field after a 1 MB body would only be read once the whole upload had
    # streamed, so the file must come last.
    check(names and names[-1] == "file", f"file is not the last part: {names}")
    return names


def main() -> int:
    tmp = Path(tempfile.mkdtemp())
    wav_path, mp_path = tmp / "t.wav", tmp / "t.multipart"

    out = subprocess.run(["./gen_fixtures", str(wav_path), str(mp_path)],
                         check=True, capture_output=True, text=True).stdout.splitlines()
    content_type, content_length = out[0], int(out[1])

    check_wav(wav_path)
    names = check_multipart(mp_path, content_type, content_length)

    if failures:
        print("FAIL interop")
        for f in failures:
            print(f"  - {f}")
        return 1
    print(f"PASS interop        wav decoded by stdlib, {len(names)} multipart fields parsed, "
          f"Content-Length exact")
    return 0


if __name__ == "__main__":
    sys.exit(main())
