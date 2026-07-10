# Custom transcription endpoints

TapTalk sends a `multipart/form-data` request compatible with OpenAI's
`POST /v1/audio/transcriptions` endpoint. The form contains:

- `file`: a 16 kHz, 16-bit mono WAV named `audio.wav`
- `model`: the model name configured in setup
- `response_format`: `text`
- `language`: only when configured

The `Authorization: Bearer ...` header is omitted when the API-key field is
empty. This makes a local server without authentication usable.

## Setup examples

| Backend | Endpoint | Model | API key |
| --- | --- | --- | --- |
| OpenAI | `https://api.openai.com/v1/audio/transcriptions` | `gpt-4o-mini-transcribe` | required |
| Local OpenAI-compatible server | `http://192.168.1.50:8000/v1/audio/transcriptions` | server-specific | optional |

Use HTTPS for every endpoint outside your trusted home or office network. HTTP
reveals both the recorded audio and any configured bearer token to other
devices on that network. TapTalk permits HTTP only to support local servers;
it does not make an HTTP connection safe on a public network.

HTTPS requests wait for SNTP clock synchronization so ESP-IDF can validate the
server certificate. HTTP requests intentionally skip that requirement, which
lets a local-only network work without internet access.
