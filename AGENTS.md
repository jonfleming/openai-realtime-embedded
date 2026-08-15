# AGENTS.md

## Purpose

This file captures the non-obvious implementation details that made the Freenove Media Kit work reliably with the current speech-to-speech backend. Use it as a guardrail before changing audio, WebRTC, Opus, or libpeer behavior.

## Project Snapshot

- Platform target: ESP32-S3 (Freenove Media Kit, AIPI-Lite, Waveshare ESP32-S3 Touch AMOLED 1.8, or 2.06 watch)
- Transport: WebRTC (libpeer)
- Audio codec: Opus
- Signaling: HTTP SDP offer/answer
- Realtime endpoint: compile-time `OPENAI_REALTIMEAPI` in `CMakeLists.txt`
- Build configuration: board chosen via CMake options (see Board Selection below); Waveshare is the current default
- Key app files:
  - `src/media.cpp`
  - `src/webrtc.cpp`
  - `src/http.cpp`
  - `src/aipi_lite_config.h/c`
  - `components/peer/CMakeLists.txt`
  - `deps/libpeer/src/config.h`

## Board Selection

Board is chosen by CMake options in `CMakeLists.txt` (mutually exclusive; Waveshare 1.8 is the default `ON`, precedence Waveshare 2.06 > 1.8 > AIPI-Lite > Freenove):
- `-DWAVESHARE_AMOLED_2_06_BOARD=ON`: Waveshare ESP32-S3 Touch AMOLED 2.06 (watch) — overlays `sdkconfig.waveshare_amoled_2_06` (32 MB QIO flash etc.)
- `-DWAVESHARE_AMOLED_1_8_BOARD=ON` (default): Waveshare ESP32-S3 Touch AMOLED 1.8 — overlays `sdkconfig.waveshare_amoled_1_8`
- `-DAIPI_LITE_BOARD=ON` (with `WAVESHARE_AMOLED_1_8_BOARD=OFF`): AIPI-Lite (stripped-down, cheaper variant)
- Both `OFF` (`-DWAVESHARE_AMOLED_1_8_BOARD=OFF -DAIPI_LITE_BOARD=OFF`): Freenove Media Kit (full-featured)

For the default Waveshare 1.8 build no flag is needed: plain `idf.py build` is correct.

`CMakeLists.txt` pins `IDF_TARGET` to `esp32s3` (guarded by `if(NOT DEFINED IDF_TARGET)`
so `IDF_TARGET=linux` still selects the Linux build). Do NOT remove this: the IDF
default target is `esp32`, and with a fresh build dir / deleted `sdkconfig` the
component manager silently filters out every board BSP (all esp32s3-only) and
fails version solving with a confusing "no versions match ^1.1.4" error.

### Exactly one Waveshare BSP per build (important)

The 1.8 and 2.06 boards use different managed BSPs (`waveshare/esp32_s3_touch_amoled_1_8`
and `waveshare/esp32_s3_touch_amoled_2_06`) that export the **same `bsp_*` symbols**
(`bsp_i2c_init`, `bsp_display_new`, `bsp_audio_init`, ...), so linking both would
fail with duplicate-symbol errors. The active BSP is selected in
`src/idf_component.yml` with `rules:` on the `FREEBUFF_BOARD` environment
variable, which `CMakeLists.txt` sets (to `waveshare_amoled_1_8` or
`waveshare_amoled_2_06`) before `project()` runs the component manager. The
unused BSP is not downloaded/built at all — never add both to
`src/CMakeLists.txt` `REQUIRES`.

`src/CMakeLists.txt` must therefore NOT list the board BSPs (or the
1.8-only `esp_io_expander*`, which flow from the 1.8 BSP's public deps); they
come from `src/idf_component.yml`. When switching boards, delete the generated
`sdkconfig` (or use `idf.py fullclean`) so the per-board sdkconfig overlay
regenerates (an existing sdkconfig keeps stale values).

## Hardware Differences

### I²S Pinout Layout
| Feature | Freenove Media Kit | AIPI-Lite |
|---------|-------------------|-----------|
| MCLK | N/A (direct MEMS mic) | 6 (ES8311, 256 × fs) |
| BCLK | 42 | 14 |
| LRCLK | 41 | 12 |
| DIN (mic) | 46 | 13 |
| DOUT (speaker) | 1 | 11 |

### Key Differences
- **Power management**: AIPI-Lite has `POWER_KEEP_ALIVE_PIN=10` that must be HIGH to prevent battery cutoff
- **Backlight PWM**: Freenove uses GPIO2; AIPI-Lite uses GPIO3 (strapping pin, works but shows warning)
- **Display SPI**: Different pins due to different board layout
- **Buttons**: Left button on GPIO1 (AIPI-Lite) vs GPIO19 (Freenove); Right button on GPIO42

### Waveshare boards (1.8 and 2.06)

Both boards route display/touch/audio through their managed BSP; the BSP owns
the I2C bus (SCL 14 / SDA 15) and the I2S pins. Do NOT claim those pins with
raw drivers (GPIO 14 is the BSP's I2C SCL; claiming it breaks touch/display).

| Feature | 1.8 (V1.0) | 2.06 (watch) |
| --- | --- | --- |
| Display | 368x448 QSPI AMOLED, SH8601 | 410x502 QSPI AMOLED, CO5300 (driven via the SH8601 QSPI driver in the official BSP) |
| Touch | FT3168 (power-gated by TCA9554 expander — needs the boot pulse in `lcd.cpp`) | FT3168 (reset = GPIO9, no expander) |
| Audio | ES8311 codec, mono 16 kHz in/out | ES8311 DAC + ES7210 dual-mic ADC, **stereo 16 kHz** in/out |
| Console | USB-Serial-JTAG (native USB) — `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` | USB-UART bridge — default UART0 console |
| Flash | 16 MB | 32 MB (QIO) |

The 2.06 audio path (`src/media.cpp`) mirrors the Waveshare Spec_Analyzer
recipe: open both `esp_codec_dev` handles at 16 kHz / 16-bit / 2 channels,
`esp_codec_dev_set_in_gain(mic, 24.0)`, speaker volume 100 (0 dB on the codec-dev curve; the Waveshare example's 60 is -20 dB and far too quiet), read 16-bit stereo
L/R PCM and downmix to mono before Opus encode. The 1.8 path stays mono 16
kHz. `esp_codec_dev` reconfigures the shared I2S slot/clock on open, so the
BSP's mono 22050 Hz default is harmless.

Display init (`src/lcd.cpp`) replicates `bsp_display_start()` with public BSP
APIs (touch probe retried, never fatal) and uses this project's proven
LVGL buffer convention (DMA buffer, `sw_rotate=false`, 20 rows) instead of the
BSP defaults, which cause "Failed to allocate priv TX buffer" once
WebRTC/TLS consume the internal heap. `max_transfer_sz` must be nonzero (the
2.06 BSP asserts it).

## AIPI-Lite Audio (ES8311 codec)

The AIPI-Lite has one ES8311 codec with ADCLRC/DACLRC tied, so TX and RX must run at the SAME sample rate on ONE shared I2S bus. The AIPI path in `src/media.cpp` mirrors the working Arduino sketch / stock firmware for this exact board:

- Single full-duplex I2S controller (`I2S_NUM_0`), master, **16 kHz** both directions.
- 16-bit data in **32-bit slots** (BCLK = 64 × fs = 1.024 MHz, MCLK/BCLK = 4), MCLK = GPIO6 at 256 × fs (4.096 MHz).
- Mic captured at 16 kHz, extracted from the upper 16 bits of each 32-bit slot (L+R summed, ×12 gain, mirroring the sketch's `convert_input_to_backend_pcm`), encoded as Opus mono 16 kHz, 320 samples/frame (20 ms) — the RTP timestamp still advances 960/20 ms (48 kHz clock), matching `opus/48000` negotiation.
- Speaker plays decoded 16 kHz audio left-aligned (`<< 16`) into the same 32-bit slots.
- The ES8311 is configured over I2C (SDA=5, SCL=4, addr 0x18) by `src/es8311.c`, using the exact ESPHome/Arduino register sequence proven on this board (CLK1=0x3F, CLK2=0x08, CLK3=0x10, CLK4=0x20, CLK6=0x03, SDP 0x0C, REG17=0xC8, REG37=0x08, REG31=0x00). Speaker amp enable is GPIO9, driven high.

Critical: `src/es8311.c` must leave ES8311 register 0x00 (RESET) at **0x80 (power-on)** as the LAST write (0x1F → 0x00 → config → 0x80). An init that ends at 0x00 powers the codec down: I2C still ACKs and registers still read back, but both the ADC (all-zero mic) and DAC (silent speaker) are dead. Do NOT add register writes beyond the reference sequence — e.g. REG37 must be 0x08 (not 0x48) and REG16 (mic gain) must stay 0x00.

Do NOT run the mic at a different rate than the speaker on AIPI-Lite: the shared LRCK makes that impossible. Do NOT change the I2S slot width on AIPI: the codec's `bclk_div=4` expects BCLK = 64 × fs (32-bit slots); 16-bit slots would clock the codec wrong.

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

## ESP-IDF v5.5.5 Environment Setup (Windows)

This repo targets IDF v5.5.5 with the ESP32-S3. The Espressif IDF installer put the tools/toolchains and the Python venv under `C:\Espressif\tools`; the IDF source lives at `C:\esp\v5.5.5\esp-idf`.

### PowerShell (interactive build / flash / monitor)

The launcher opens a PowerShell session with the IDF environment:

    C:\Users\jonfl\Dropbox\Tools\esp.cmd

(`esp5.cmd` is the same thing; both just launch PowerShell and dot-source the installer-generated profile `C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1`. `C:\Users\jonfl\Dropbox\Tools` is on PATH, so `esp.cmd` can also be typed directly from any PowerShell.)

The profile sets:
- `IDF_PATH=C:\esp\v5.5.5\esp-idf`
- `IDF_TOOLS_PATH=C:\Espressif\tools`
- `IDF_PYTHON_ENV_PATH=C:\Espressif\tools\python\v5.5.5\venv`
- `IDF_CCACHE_ENABLE=1`
- PATH prepended with ccache, cmake 3.30.2, ninja 1.12.1, the xtensa/riscv toolchains, and the venv `Scripts` dir; defines `idf.py`, `esptool.py`, `espefuse.py`, `espsecure.py`, `parttool.py` aliases.

Then, from that PowerShell (board flag optional — Waveshare is the default):

    idf.py build
    idf.py -p COM4 flash
    idf.py -p COM4 monitor

VS Code: the ESP-IDF extension is already wired up in `.vscode/settings.json` (`idf.espIdfPathWin`, `idf.toolsPathWin`, `idf.currentSetup` = v5.5.5, `idf.portWin` = COM4, `IDF_TARGET=esp32s3`).

### Git Bash / agent shells

`source $IDF_PATH/export.sh` does **not** work here: it probes the system Python (3.14) and looks for `C:\Espressif\python_env\...`, which the installer layout doesn't create. Instead set the variables manually and call idf.py through the venv python:

    export IDF_PATH=C:/esp/v5.5.5/esp-idf
    export IDF_TOOLS_PATH=C:/Espressif/tools
    export IDF_PYTHON_ENV_PATH=C:/Espressif/tools/python/v5.5.5/venv
    export PATH="$IDF_TOOLS_PATH/python/v5.5.5/venv/Scripts:$IDF_TOOLS_PATH/ccache/4.12.1/ccache-4.12.1-windows-x86_64:$IDF_TOOLS_PATH/ninja/1.12.1:$IDF_TOOLS_PATH/xtensa-esp-elf/esp-14.2.0_20260121/xtensa-esp-elf/bin:$IDF_TOOLS_PATH/cmake/3.30.2/bin:$PATH"
    "$IDF_TOOLS_PATH/python/v5.5.5/venv/Scripts/python.exe" "$IDF_PATH/tools/idf.py" build

For fast, object-level rebuilds of an already-configured tree, drive ninja directly (ccache must be on PATH):

    export PATH="/c/Espressif/tools/ccache/4.12.1/ccache-4.12.1-windows-x86_64:$PATH"
    /c/Espressif/tools/ninja/1.12.1/ninja.exe -C build <target>

## Interrupt Conversation Feature

When the device is in an active conversation with the speech-to-speech backend, pressing the interrupt button (second button on each board) toggles interruption mode:

1. **First press** - Enters interruption mode:
   - Stops any currently playing audio response
   - Disables microphone capture (no more Opus packets sent to server)
   - All incoming WebRTC audio from server is ignored

2. **Second press** - Returns to normal operation:
   - Resume normal question/answer conversation
   - Microphone capture resumes
   - Server responses play normally

### Button Locations by Board

| Board | Interrupt Button Pin |
|-------|---------------------|
| Freenove Media Kit | GPIO19 (left button) |
| AIPI-Lite | GPIO1 (left button, also power button) |
| Waveshare 1.8 | GPIO42 (right button) |
| Waveshare 2.06 | GPIO42 (right button) |

### Implementation Details

- Interrupt state is shared globally (`s_interrupted` in `main.cpp`)
- Button press uses external interrupt handler with negative edge trigger
- `oai_send_audio_task()` checks interruption flag every 50ms when interrupted
- `oai_send_audio()` in media.cpp returns early if interrupted (no mic capture)
- Audio playback is stopped via BSP codec close or I2S TX channel disable

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

## TURN Relay (coturn) — WebRTC Media Path

- Signaling is unchanged: the SDP offer/answer still goes over HTTP to
  `OPENAI_REALTIMEAPI` (e.g. `https://speech.fleming.ai/v1/realtime`). The
  coturn server at `turn.fleming.ai` (UDP 3478/5349 + relay range
  49152-65535) is used for the WebRTC media path (ICE checks, DTLS, RTP,
  SCTP).
- TURN credentials are long-term (`lt-cred-mech` style): username + password
  plus a per-allocation nonce/realm. They come from `privateConfig.json`
  (gitignored) via `CMakeLists.txt`; without them the client silently skips
  the TURN allocation and uses only host/STUN candidates.
- Vendored libpeer (`deps/libpeer`) is patched to be a functional TURN UDP
  client: it allocates a relay, installs CreatePermission for every remote
  candidate IP, wraps all outbound packets to relay candidates in Send
  Indications, unwraps inbound Data Indications, and refreshes the allocation
  + permissions every 5 minutes from the peer loop. These are deliberate
  local deltas — see `deps/libpeer/src/agent.c` (`agent_turn_*`) and
  `stun.c` (`stun_msg_parse_data_indication`).
- Do not "simplify" the TURN path back to plain `agent_socket_send`: a remote
  relay candidate is only reachable through our allocation, and raw binding
  requests to the relayed address get 401 from coturn (upstream libpeer
  issue #215).
- Both peers must share the same TURN server for relay-to-relay media. The
  aiortc speech server needs `iceServers` with the same turn URL/credentials;
  the client alone is not enough when both sides are behind NAT.
- Limitations: UDP only (no `turns:`/TLS, no TCP, no ChannelData/ChannelBind,
  no STUN binding over the relay for ice-lite fallback). Opus frames and SCTP
  packets fit well under the 1300-byte MTU.

## Security and Secrets

- Device mode reads API key from NVS (`wifi_config`).
- Linux mode reads key via compile definitions from `privateConfig.json`.
- Do not commit real keys.
- `src/http.cpp` currently logs bearer token. Remove or gate this log before production release.
- TLS: `sdkconfig.defaults` enables the full ESP-IDF cert bundle (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y`).
  Do NOT enable `CONFIG_ESP_TLS_INSECURE` / `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY`, and do NOT set
  `config.skip_cert_common_name_check = true` in `oai_http_request()`: in IDF 5.5 that disables SNI
  entirely, and `speech.fleming.ai` rejects SNI-less handshakes with a fatal alert
  (`mbedtls_ssl_handshake returned -0x7780`, ESP_ERR_HTTP_CONNECT).

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
