# AGENTS.md

## Purpose

This file captures the non-obvious implementation details that made the Freenove Media Kit work reliably with the current speech-to-speech backend. Use it as a guardrail before changing audio, WebRTC, Opus, or libpeer behavior.

## Project Snapshot

- Platform target: ESP32-S3 (Freenove Media Kit)
- Transport: WebRTC (libpeer)
- Audio codec: Opus
- Signaling: HTTP SDP offer/answer
- Realtime endpoint: compile-time `OPENAI_REALTIMEAPI` in `CMakeLists.txt`
- Key app files:
  - `src/media.cpp`
  - `src/webrtc.cpp`
  - `src/http.cpp`
  - `components/peer/CMakeLists.txt`
  - `deps/libpeer/src/config.h`

## Critical Invariants (Do Not Break)

1. Mic capture must use 32-bit I2S slots at 16 kHz.
- In `src/media.cpp`, RX config uses:
  - `I2S_DATA_BIT_WIDTH_16BIT`
  - `I2S_SLOT_BIT_WIDTH_32BIT`
  - `I2S_SLOT_MODE_STEREO`
- Why: many I2S MEMS mics need BCLK >= 1 MHz. At 16 kHz, stereo 32-bit slots gives 1.024 MHz.
- Data extraction depends on this: samples are read from upper 16 bits via `>> 16`.

2. Keep stereo capture for mic input, then downmix/select channel in software.
- `MIC_I2S_CHANNELS` is 2 and code selects louder channel each frame.
- Mono capture on ESP32-S3 previously showed DMA padding artifacts.

3. Opus capture settings are tuned for 20 ms uplink frames.
- Mic encode: 16 kHz, mono, VOIP application.
- Frame size: 320 samples (20 ms at 16 kHz).
- Bitrate: 32 kbps, low complexity for ESP32 headroom.

4. Audio uplink must start only after `PEER_CONNECTION_COMPLETED`.
- In `src/webrtc.cpp`, audio task waits for completed state before sending.
- Starting earlier can produce send failures or unstable startup.

5. Data channel creation must happen inside SCTP `onopen` callback.
- In `src/webrtc.cpp`, `oai-events` is created in `oai_ondatachannel_onopen_task`.
- Session update and greeting are sent only after that channel exists.

6. libpeer audio ring buffer is intentionally disabled.
- In `components/peer/CMakeLists.txt`, build defines include `-DCONFIG_AUDIO_BUFFER_SIZE=0`.
- This avoids extra buffering/latency issues seen in the realtime path.

7. DTLS must use ECDSA for aiortc-style speech-to-speech interop.
- `deps/libpeer/src/config.h` sets `CONFIG_DTLS_USE_ECDSA 1`.
- Rationale in file comment: avoid DTLS handshake failure from no common ciphers with RSA-only server certs.

## Known Good Runtime Flow

1. `main.cpp` initializes NVS/network/audio and starts WebRTC loop.
2. `peer_connection_create_offer()` generates SDP offer.
3. `http.cpp` POSTs SDP to `OPENAI_REALTIMEAPI + "/calls"`.
4. Remote SDP answer is applied.
5. Peer reaches `PEER_CONNECTION_COMPLETED`.
6. SCTP `onopen` fires, creates `oai-events` channel.
7. Client sends `SESSION_UPDATE`, then `GREETING`.
8. Audio publisher task sends Opus mic frames continuously.

## File-Level Change Guide

### `src/media.cpp`

Safe to tweak:
- `MIC_GAIN`
- Opus bitrate/complexity
- Log cadence

Treat as high risk:
- I2S slot width/mode
- Mic sample rate
- Shift/unpack logic (`>> 16`)
- Frame size assumptions around 20 ms packets

If touching high-risk areas, validate with:
- Audible quality
- No clipped/distorted capture
- Stable send cadence
- Backend VAD responsiveness

### `src/webrtc.cpp`

Safe to tweak:
- Prompt/greeting text
- Session parameters (carefully)

Treat as high risk:
- Connection state handling
- Order/timing of data channel creation
- Audio task startup condition

If changing event order, confirm:
- `oai-events` opens every run
- `SESSION_UPDATE` send returns success
- First assistant response arrives reliably

### `components/peer/CMakeLists.txt`

This file injects compile-time behavior into vendored libpeer. Keep these defines deliberate:
- `CONFIG_USE_LWIP=1`
- `CONFIG_AUDIO_BUFFER_SIZE=0`
- `CONFIG_DATA_BUFFER_SIZE=102400`
- `CONFIG_USE_USRSCTP=0`

Changing these can alter memory pressure, handshake timing, and media/data stability.

### `deps/libpeer/src/config.h`

This is vendored third-party code but intentionally patched for this project.
Preserve and document any local deltas. Especially keep:
- `CONFIG_DTLS_USE_ECDSA 1`
- `KEEPALIVE_CONNCHECK 0`
- `CONFIG_AUDIO_BUFFER_SIZE 0` default

## Security and Secrets

- Device mode reads API key from NVS (`wifi_config`).
- Linux mode reads key via compile definitions from `privateConfig.json`.
- Do not commit real keys.
- `src/http.cpp` currently logs bearer token. Remove or gate this log before production release.

## Validation Checklist After Changes

1. Build success for ESP32-S3.
2. Device boots and reaches Wi-Fi setup / normal startup.
3. SDP HTTP call returns expected status (201 path today).
4. Peer reaches `PEER_CONNECTION_COMPLETED`.
5. Data channel opens and can send `SESSION_UPDATE`.
6. Mic uplink produces non-zero Opus packets.
7. Downlink audio decodes and plays without glitches.
8. End-to-end conversational turn works repeatedly.

## Suggested Debug Markers

When diagnosing regressions, log these checkpoints in order:
- Offer created
- Answer applied
- State changed to COMPLETED
- SCTP onopen fired
- Data channel created
- SESSION_UPDATE sent
- First mic packet encoded/sent
- First transcript or assistant event received

## How To Preserve Future Learnings

When a fix is confirmed, record it in both places:
- This file for agent-facing guardrails.
- `/memories/repo/audio-pipeline-notes.md` for concise repo memory.

Keep updates short and concrete:
- What changed
- Why it mattered
- What symptom it fixed
- Any measurable before/after behavior
