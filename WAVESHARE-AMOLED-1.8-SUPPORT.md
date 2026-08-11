# Plan: Add support for Waveshare ESP32-S3 Touch AMOLED 1.8

Status: **In progress** — steps 1 (board selection + dependencies) and 2
(display/touch via BSP) done and build-verified. This document is the
plan/design for adding a third board profile to `openai-realtime-embedded`,
alongside Freenove Media Kit and AIPI-Lite.

---

## 1. Board facts (verified against Waveshare wiki + first-party repo)

| Feature | Detail |
| --- | --- |
| MCU | ESP32-S3R8, 240 MHz, 8 MB PSRAM (octal), 16 MB flash (QIO) |
| Display | 1.8" 368×448 QSPI AMOLED — SH8601 (V1.0) or CO5300 (V2) |
| Touch | FT3168 (V1.0) or CST820 (V2), I2C |
| Audio | ES8311 codec, onboard MEMS mic + speaker amp (pa pin), I2S |
| PMU | AXP2101 (battery/charging) |
| Extras | PCF85063A RTC, QMI8658 IMU, microSD (SDMMC), PWR/BOOT buttons |

Official managed BSP: **`waveshare/esp32_s3_touch_amoled_1_8` ^2.0.3**
(ESP Component Registry; targets esp32s3, supports ESP-IDF v5.5.x / v6.0.x).
This project's lockfile pins IDF 5.5.5 → compatible.

## 2. Key design decision: use the Waveshare managed BSP

Do **not** hand-roll the SH8601/CO5300 panel driver, a third ES8311 register
sequence, or a custom touch driver. The BSP provides, and Waveshare's own
`00_bsp_quickstart` / `12_i2s_codec` / `14_lvgl_demo_v9` examples use:

- `bsp_display_start()` → panel + touch + LVGL 9 + input device, one call
- `bsp_display_lock()/unlock()`, `bsp_display_brightness_set()`
- `bsp_audio_init()`, `bsp_audio_codec_speaker_init()`, BSP pins
  (`BSP_I2S_MCLK/SCLK/LCLK/DOUT/DSIN`, `BSP_I2C_NUM`, `BSP_POWER_AMP_IO`,
  `ES8311_CODEC_DEFAULT_ADDR`) + `esp_codec_dev` (`es8311_codec_new`)
- Handles the FT3168/CST820 and SH8601/CO5300 revision differences internally

The project's existing `src/es8311.c` stays AIPI-Lite-only (it is already
gated on `AIPI_LITE_BOARD`); the Waveshare path uses the BSP's codec driver
via `esp_codec_dev`, which is the maintained path.

## 3. Board selection (build config)

Current state: single boolean `AIPI_LITE_BOARD` (`ON` default) in
`CMakeLists.txt`; Freenove when `OFF`.

Add a third selector without breaking existing invocations:

- Keep `AIPI_LITE_BOARD` semantics as-is.
- Add `option(WAVESHARE_AMOLED_1_8_BOARD "Build for Waveshare ESP32-S3 Touch AMOLED 1.8" OFF)`.
- Precedence: `WAVESHARE_AMOLED_1_8_BOARD` > `AIPI_LITE_BOARD` > Freenove.
- Emit `add_compile_definitions(WAVESHARE_AMOLED_1_8_BOARD=1)` (and keep
  emitting `AIPI_LITE_BOARD=0/1` as today).
- Build examples:
  - `idf.py -DWAVESHARE_AMOLED_1_8_BOARD=ON build`
  - `idf.py -DAIPI_LITE_BOARD=ON build` (unchanged)
  - `idf.py -DAIPI_LITE_BOARD=OFF build` (unchanged, Freenove)

Longer-term (optional): migrate to a single `BOARD=` string option
(`freenove` | `aipi_lite` | `waveshare_amoled_1_8`) once all three profiles
exist; map the legacy booleans in CMake for backward compatibility.

✅ **DONE (build-verified with ESP-IDF 5.5.5):** `CMakeLists.txt` now defines
both options with the precedence above. Verified in `build/CMakeCache.txt`
(`AIPI_LITE_BOARD=ON`, `WAVESHARE_AMOLED_1_8_BOARD=ON`) and in all 1778 src
compile commands (`-DWAVESHARE_AMOLED_1_8_BOARD=1 -DAIPI_LITE_BOARD=0`).
Full `idf.py -DWAVESHARE_AMOLED_1_8_BOARD=ON build` completed; `src.bin`
≈1.48 MB, fits the 0x7F0000 factory partition.

## 4. Dependencies (`src/idf_component.yml`, `src/CMakeLists.txt`)

- Add `waveshare/esp32_s3_touch_amoled_1_8: ^2.0.3` to `src/idf_component.yml`
  (the BSP transitively pulls `esp_codec_dev`, `esp_lcd_*` panel drivers, its
  own `esp_lvgl_port`/`lvgl` expectations, `esp_lcd_touch`, etc.).
- **First validation step:** resolve the BSP's transitive constraints against
  the project's pins `lvgl/lvgl == 9.3.0` and `espressif/esp_lvgl_port ==
  2.5.0`. The Waveshare `14_lvgl_demo_v9` example uses LVGL 9 + the BSP, so a
  compatible combination exists; adjust pin versions (or BSP version) only if
  the resolver complains.
- `src/CMakeLists.txt`: add the BSP component to `REQUIRES` (component name
  `esp32_s3_touch_amoled_1_8`) plus `esp_driver_i2c` / `esp_driver_i2s` /
  `esp_codec_dev` as needed (the Waveshare `12_i2s_codec` example requires
  `esp32_s3_touch_amoled_1_8 esp_driver_i2c`).
- Keep the existing `waveshare/esp_lcd_st7735` etc. only where used (AIPI path).

✅ **DONE (build-verified):** dependency added to `src/idf_component.yml`.
Component manager resolved **16 dependencies with no conflicts** — the BSP
(`waveshare/esp32_s3_touch_amoled_1_8 2.0.3`) and its transitive deps
(`esp_codec_dev 1.5.11`, `esp_io_expander_tca9554 2.0.3`, `esp_lcd_co5300
2.1.0`, `esp_lcd_touch_cst816s 1.1.1~2`, `esp_lcd_touch_ft5x06 1.1.0~2`)
coexist with the project's pinned `lvgl 9.3.0`, `esp_lvgl_port 2.5.0`, and
`esp_lcd_panel_io_additions 1.0.1` (all kept at their pins; `dependencies.lock`
updated). BSP headers land in
`managed_components/waveshare__esp32_s3_touch_amoled_1_8/include/bsp/`
(`esp-bsp.h` available for later steps).

## 5. Display / LVGL (`src/lcd.cpp`, `src/main.cpp`)

- In `init_lvgl()`, add `#if defined(WAVESHARE_AMOLED_1_8_BOARD) &&
  WAVESHARE_AMOLED_1_8_BOARD` branch that:
  - calls `bsp_display_start()` (initializes panel, touch, LVGL, input device)
    and stores the returned `lv_display_t *` into the existing global
    `disp_handle`;
  - calls `bsp_display_brightness_set(85)`;
  - skips the Freenove/AIPI SPI + ST7735/ST7796 + LEDC-backlight code
    (AMOLED brightness is a panel command, not the LEDC backlight — keep
    `backlight_init()`/`set_backlight_brightness()` Freenove/AIPI-only).
- Locking: the other boards use `lvgl_port_lock()/unlock()` from
  `esp_lvgl_port` v2.5; the BSP examples use `bsp_display_lock()/unlock()`.
  Add small `#ifdef`s (or a `lcd_disp_lock()` helper) in `lvgl_ui()` and
  `lvgl_ui_label_set_text()` so the Waveshare branch locks via the BSP.
- Resolution is 368×448 (portrait, 2:1-ish smaller than Freenove 480×320):
  keep the existing label-list UI but size the container for the new
  dimensions; `lvgl_ui_label_set_text()` behavior (last-5 labels, auto-scroll)
  is unchanged.
- Fonts: `CONFIG_LV_FONT_MONTSERRAT_20` is already enabled; BSP quickstart
  uses montserrat_14/24 — consider enabling `CONFIG_LV_FONT_MONTSERRAT_24=y`
  for the smaller, denser screen.

✅ **DONE (build-verified, all three board profiles):** `src/lcd.cpp` now has
an `#elif WAVESHARE_AMOLED_1_8_BOARD` pin block (368×448), a Waveshare branch
in `init_lvgl()` calling `bsp_display_start()` + `bsp_display_brightness_set(85)`
(storing into the shared `disp_handle`), `lcd_disp_lock()/unlock()` helpers that
route to `bsp_display_lock/unlock` on this board and `lvgl_port_lock/unlock`
elsewhere (used by `lvgl_ui()` and `lvgl_ui_label_set_text()`), and the
LEDC-backlight / `reset_lcd()` / `create_freenove_panel()` machinery guarded
so it only compiles on the SPI-panel boards. `src/CMakeLists.txt` now
`REQUIRES` the BSP component. `idf.py build` passes for **Waveshare
(WAVESHARE=1/AIPI=0), AIPI-Lite (AIPI=1), and Freenove (both=0)** — no
warnings in lcd.cpp. NOTE: the shared `build/` dir was wiped/reconfigured by a
concurrent process mid-session (a stale VS2022-generator cache briefly
appeared); source, `dependencies.lock` and `managed_components/` were
unaffected.

## 6. Audio (`src/media.cpp`)

Waveshare path mirrors the project's proven AIPI-Lite topology (ES8311,
shared full-duplex I2S) but uses the BSP/`esp_codec_dev` API instead of the
raw register driver:

- `oai_init_audio_capture()` (`#ifdef WAVESHARE_AMOLED_1_8_BOARD` branch):
  - `bsp_audio_init()` (also brings up I2C via `bsp_i2c_init()` semantics);
  - create full-duplex I2S on `CONFIG_BSP_I2S_NUM` using
    `BSP_I2S_MCLK/SCLK/LCLK/DOUT/DSIN`, `audio_codec_new_i2s_data()`;
  - `es8311_codec_new()` with `pa_pin = BSP_POWER_AMP_IO`, `master_mode =
    false`, `use_mclk = true` (exactly the `12_i2s_codec` recipe);
  - create a speaker handle (`ESP_CODEC_DEV_TYPE_OUT`) and a mic handle
    (`ESP_CODEC_DEV_TYPE_IN`); enable PA via the codec dev (no manual
    `aipi_lite_enable_speaker_amp` — that stays AIPI-only).
- **Sample rate: 16 kHz both directions.** Rationale: the project's mic
  encode path is 16 kHz/20 ms Opus and the ES8311's shared LRCK forces
  mic/speaker rates to match; this is the same constraint the AIPI-Lite path
  already satisfies. Set `SPK_SAMPLE_RATE`/`SPK_BUFFER_SAMPLES` to
  16000/320 for this board.
- `oai_audio_decode()`: `esp_codec_dev_write(speaker, pcm, bytes)` instead of
  raw `i2s_channel_write`; no 32-bit-slot left-alignment needed if the codec
  dev consumes plain 16-bit PCM (verify against the example's 16-bit mono
  config; adjust channels if the BSP uses stereo).
- `oai_send_audio()`: read mic via `esp_codec_dev_read(...)`; adapt the
  sample extraction — likely straight 16-bit PCM (possibly mono), not the
  `>> 16` 32-bit-slot unpacking used by Freenove/AIPI. Reuse the existing
  ZERO-mic diagnostic logging to catch bit-position mismatches on first boot.
- Keep the AIPI `output_buffer_32` allocation AIPI-only.

## 7. `src/main.cpp` init order

- AIPI-only init calls (power keep-alive, button pins, `es8311_init()`,
  `aipi_lite_enable_speaker_amp`) remain under `#ifdef AIPI_LITE_BOARD`.
- Waveshare path: NVS → event loop → `init_lvgl()` (BSP display+touch+LVGL)
  → `peer_init()` → `oai_init_audio_capture()` (BSP audio+codec) →
  `oai_init_audio_decoder()` → `lvgl_ui()` → `wifi_config_init()` →
  `oai_webrtc()`. Display init before/after audio doesn't matter functionally;
  keep the current structure and only swap the guarded blocks.
- (Optional, not required for v1) PWR button → mute / push-to-talk, since
  the board has no GPIO button wired to the existing button code.

## 8. `sdkconfig.defaults`

Add the Waveshare-recommended settings (from their display/audio examples),
merging with the project's existing config:

```
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
CONFIG_SPIRAM_MODE_OCT=y        # already present
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_FREERTOS_HZ=1000
CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y
CONFIG_LV_FONT_MONTSERRAT_24=y  # optional
```

`CONFIG_ESPTOOLPY_FLASHSIZE_16MB` and the 0x7F0000 factory partition are
already in place; no partition change needed. Verify QIO vs the project's
current flash mode at first flash (Waveshare examples mandate QIO).

## 9. Documentation updates

- New `WAVESHARE-AMOLED-1.8-GPIO-Pins.md` at repo root (mirroring
  `AIPI-Lite-GPIO-Pins.md`) documenting the BSP-owned pin map, ES8311 I2C
  address, PA pin, and the note that pins are managed by the BSP.
- `README.md`: add the board to the supported list and document
  `idf.py -DWAVESHARE_AMOLED_1_8_BOARD=ON build`.
- `AGENTS.md`: extend "Board Selection" + "Hardware Differences" with the
  third profile and its guardrails (16 kHz shared-LRCK ES8311, BSP-owned
  display/touch, no LEDC backlight).
- `memories/repo/audio-pipeline-notes.md`: record confirmed audio findings
  after hardware validation (same policy as the AIPI-Lite entry).

## 10. Validation checklist (hardware bring-up, in order)

1. `idf.py -DWAVESHARE_AMOLED_1_8_BOARD=ON build` succeeds; component
   resolver accepts BSP + pinned lvgl/esp_lvgl_port.
2. Flash + monitor: boots, BSP logs panel/touch/audio init; no crashes in
   `app_main`.
3. Display: LVGL UI renders; `bsp_display_brightness_set()` works (no LEDC).
4. Touch: taps register (draw a test label or use the quickstart slider) —
   confirms `bsp_display_start()` input device + BSP revision detection.
5. Mic: reuse the ZERO-mic diagnostics — non-zero energy at 16 kHz; confirm
   PCM layout (16-bit mono vs 32-bit slots) and adjust extraction.
6. Speaker: assistant audio audible; if distorted, check channels/rate config
   passed to `esp_codec_dev_open`.
7. End-to-end: Wi-Fi → SDP → `PEER_CONNECTION_COMPLETED` → data channel →
   `SESSION_UPDATE` → conversational turn (reuse AGENTS.md debug markers).
8. Record results in AGENTS.md + memories; update this plan's status.

## 11. Risks / open questions

- ~~BSP transitive deps vs pinned lvgl/esp_lvgl_port~~ — **resolved:** no
  conflicts; all project pins were kept (see §4). Note: `idf.py` under Git
  Bash/MSYS prints a warning and exits without building — use the PowerShell
  environment (`Microsoft.v5.5.5.PowerShell_profile.ps1`) on this machine.
- **Mic PCM format via `esp_codec_dev`** — verify on hardware (16-bit mono is
  expected from the Waveshare example's `I2S_STD_PHILIPS_MONO` config).
- **AMOLED brightness** is panel-command based; the existing LEDC backlight
  code must not run on this board.
- **Power** relies on the AXP2101 handled inside the BSP; battery telemetry
  (like the AIPI battery monitor) is a later enhancement, not required for v1.
- Board has **V1.0 vs V2** display/touch silicon. The BSP does NOT support
  both: BSP `^1.x` = V1.0 (SH8601 panel + FT3168/FT5x06 touch), BSP `2.x` =
  V2.0 (CO5300 panel + CST816S/FT5x06). Must match BSP major to the actual
  board revision (see §12).

## 12. Hardware bring-up findings (2026-08-09)

Diagnosed on the physical board (attached at COM4 on this machine).

### 12.1 The board was NOT booting: stuck in ROM download mode

Symptom: `idf.py flash` succeeds, but the app never runs; the monitor just
waits. Root cause chain:

1. Every reset while in download mode reports
   `rst:0x15 (USB_UART_CHIP_RESET), boot:0x23 (DOWNLOAD(USB/UART0))` — the
   chip boots straight into the ROM download mode.
2. Verified GPIO0 strap is HIGH (`dump_mem 0x6000403C` → `GPIO0=1`) — the
   BOOT button is NOT stuck and the strap is fine.
3. This is the known ESP32-S3 USB-Serial-JTAG limitation (esptool
   issue #970): **USB-initiated resets cannot exit download mode once
   entered**; only a real power-on reset (EN pulse / power cycle) re-samples
   the strap. This board has NO reset button (only PWR + BOOT side buttons),
   so the only escape is unplugging/replugging USB (or toggling PWR).

Workaround (no power cycle needed): run `build/s3_recover.py COM4` after
flashing. It clears `RTC_CNTL_OPTION1_REG` FORCE_DOWNLOAD_BOOT and arms the
RTC watchdog, which performs a real chip reset that boots the app. NOTE: the
board runs on an internal battery, so unplugging USB does NOT power-cycle the
chip, and there is no RST button (only PWR + BOOT side buttons). After this
session's recovery, the latch is clear and `idf.py flash`'s own hard reset
boots the app directly (esptool's ESP32-S3 target clears the flag on every
hard reset).

### 12.2 Console must be USB-Serial-JTAG on this board

The board's USB-C is the ESP32-S3 native USB (no UART bridge). The project
sent console to UART0, so even a running app printed nowhere. Added
`sdkconfig.waveshare_amoled_1_8` (loaded when `WAVESHARE_AMOLED_1_8_BOARD=ON`)
with `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`.

### 12.3 BSP major version must match the board revision

- Wiki describes the current board as **SH8601 + FT3168** = **V1.0** →
  requires BSP **`^1.x`** (latest 1.1.4, depends on `esp_lcd_sh8601`).
- BSP **2.0.3** (currently pinned) targets the **V2.0** CO5300 board; on a
  V1.0 board it will NOT abort (the co5300 driver sends init commands without
  an ID check) but the display will be blank.
- TODO: confirm the board's silkscreen revision, then pin the matching BSP:
  `waveshare/esp32_s3_touch_amoled_1_8: ^1.1.4` for V1.0.

### 12.4 REAL ROOT CAUSE of "won't boot": a crash loop, not a boot failure

Once the chip actually boots, the app **crashed in a reboot loop** ~1.3s in:

```
E (1296) ESP32-S3-Touch-AMOLED-1.8: Touch not found
Guru Meditation Error: Core 0 panic'ed (LoadProhibited)  EXCVADDR: 0x3c
Backtrace: lvgl_port_add_touch (esp_lvgl_port_touch.c:51)
           bsp_display_indev_init (esp32_s3_touch_amoled_1_8.c:634)
```

Chain of causes, each confirmed:

1. **media.cpp claimed GPIO 14** (Freenove I2S WS) before the BSP ran. The
   BSP's I2C SCL is **GPIO 14** (`BSP_I2C_SCL`), so the I2C bus never worked.
2. The BSP's touch probe (CST816S@0x15, FT5x06@0x38) both NACKed →
   `bsp_touch_new` logged "Touch not found" and returned `ESP_ERR_NOT_FOUND`.
3. **BSP 2.0.3 bug**: the compiled `bsp_display_indev_init` **ignores the
   return value** of `bsp_touch_new` (verified in the disassembly — no check
   after the call) and passes the still-NULL `tp` to `lvgl_port_add_touch`,
   which dereferences `handle->config` (NULL + 0x3c) → LoadProhibited panic.
   The `assert(tp)` doesn't fire because the project builds with
   `CONFIG_COMPILER_ASSERT_NDEBUG_EVALUATE` (asserts compiled out).

**Fix (done, in `src/media.cpp`)**: on `WAVESHARE_AMOLED_1_8_BOARD`,
`oai_init_audio_capture()` now logs and returns early instead of initializing
I2S on the Freenove pins (audio is deferred to the planned
`bsp_audio_init()`/`esp_codec_dev` path). The I2S channels stay NULL and the
read/write paths are already NULL-safe. AIPI-Lite and Freenove builds are
untouched.

**Verified on hardware**: with GPIO 14 free, the FT3168 answers at 0x38
(`Touch FT5x06 0x38 found`), `bsp_display_start()` completes
(`LVGL init: display 368x448 ready`), and the app runs to the WiFi
config SoftAP (`192.168.4.1`). No crash. `idf.py flash`'s post-flash reset
now boots the app directly (see 12.1 update).

### 12.5 Residual issues after the boot fix

- **Audio**: still pending the `bsp_audio_init()`/`esp_codec_dev` step.
- **Board revision**: V1.0 (SH8601 + FT3168) confirmed by the wiki hardware
  description and the I2C scan device set (see 12.7).

### 12.6 Flash workflow on this machine (as of this session)

- `idf.py -p COM4 flash` then boots the app directly now (the force-download
  latch was cleared by the recovery; the S3 target of esptool clears it on
  every hard reset). `idf.py -p COM4 monitor` attaches to the running app.
- If the chip ever ends up stuck in download mode again (silent port,
  `boot:0x23 (DOWNLOAD)` on reset), run
  `build/s3_recover.py COM4` — it clears `RTC_CNTL_OPTION1_REG`
  FORCE_DOWNLOAD_BOOT and arms the RTC watchdog for a real chip reset that
  boots the app. No physical power cycle needed (the battery makes USB
  unplugging ineffective anyway; the board has no RST button).
- Build/flash helper: `build/ws_flash.ps1` (loads the user's IDF env, runs
  `idf.py build` + `flash`).

### 12.7 Debug session 2 (2026-08-09): monitor works, display + touch fixed

Goal: let the user monitor the app after Wi-Fi connect. Two problems were
found and fixed on hardware (COM4).

**A. The "reboot loop when the monitor is attached" was still the touch
crash.** The earlier diagnosis said "BSP 2.0.3 ignores the error return",
but the real mechanism is subtler and affects the whole project: this
project builds with `CONFIG_COMPILER_ASSERT_NDEBUG_EVALUATE=y` (asserts
compiled out → `NDEBUG` defined). With `CONFIG_BSP_ERROR_CHECK=y`, the BSP's
`BSP_ERROR_CHECK_RETURN_NULL(x)` becomes `ESP_ERROR_CHECK(x)`, and in
IDF 5.5 `ESP_ERROR_CHECK` is a **silent no-op under NDEBUG**
(esp_err.h: `#ifdef NDEBUG ... (void) sizeof(err_rc_); ...`). So a failed
touch probe logged "Touch not found" but the error was swallowed, `tp`
stayed NULL, `assert(tp)` was compiled out, and `lvgl_port_add_touch(NULL)`
NULL-dereferenced at esp_lvgl_port_touch.c:51 → panic → reboot loop.

Fix (`src/lcd.cpp`): the Waveshare branch no longer calls
`bsp_display_start()`. It replicates the sequence with public BSP APIs
(`lvgl_port_init` → `bsp_display_new` → `lvgl_port_add_disp_rgb` → touch →
brightness) so that **touch failure is never fatal**: the probe is retried 5×
(250 ms apart) and the app boots headless (no touch input) if it still
fails. Also added an I2C bus scan diagnostic at boot.

**B. Blank display: `Failed to allocate priv TX buffer` on every draw.** The
BSP's LVGL config (plain-RAM 100-row buffer, `sw_rotate=true`) makes the SPI
driver allocate a ~74 KB DMA bounce buffer per flush, which fails once
WebRTC/TLS consume the internal heap. Fix: use this project's own proven
SPI-panel convention instead — `buff_dma=true`, `sw_rotate=false`, 20-row
buffer (14.7 KB, DMA-capable, no per-flush allocation). SPI errors are now 0.

**C. Root cause of the touch not responding at all — the board is V1.0 and
the touch is power-gated by the TCA9554.** I2C scan at boot showed the bus
is healthy: `0x18` (ES8311), `0x20` (TCA9554), `0x34` (AXP2101), `0x51`
(PCF85063), `0x6B` (QMI8658) — but **no 0x38 (FT3168)**. The Waveshare
wiki's hardware description matches V1.0 (SH8601 AMOLED + FT3168 touch), and
the Arduino reference (`02_Drawing_board.ino`) shows the FT3168 only answers
after the **TCA9554 expander pulses pins 0-2 low→high** (reset/power
release). Neither BSP version configures the expander, so the touch never
ACKs and the panel may stay off.

Fixes:
- Pinned BSP `^1.1.4` (the V1.0 BSP — adds `esp_lcd_sh8601`, drops
  co5300/cst816s) in `src/idf_component.yml`.
- `src/lcd.cpp` now runs the **TCA9554 power-up pulse** (pins 0-2 low 20 ms,
then high) via `esp_io_expander` before display/touch init, mirroring
Waveshare's Arduino reference.
- `src/CMakeLists.txt`: added `esp_lcd_touch esp_io_expander
  esp_io_expander_tca9554` to REQUIRES.
- `sdkconfig.waveshare_amoled_1_8`: `CONFIG_BSP_I2C_FAST_MODE=n` (100 kHz
  I2C — margin on this board's bus; the "check pull-up resistances" warning
  is informational, it just means internal pull-ups are disabled).

**Verified on hardware (USB-Serial-JTAG monitor):**

```
I (1129) MediaKit: TCA9554 power-up pulse (pins 0-2) done
I (1129) sh8601: LCD panel create success, version: 2.0.0
I (1399) MediaKit: I2C scan (0x08-0x77): 0x18,0x20,0x34,0x38,0x51,0x6B
I (1399) MediaKit: Touch ready
I (1409) MediaKit: LVGL init: display 368x448 ready
...
I (14139) realtimeapi-sdk: DataChannel created
I (14139) realtimeapi-sdk: SESSION_UPDATE sent (169 bytes)
I (14139) realtimeapi-sdk: GREETING sent (116 bytes)
```

- `0x38` (FT3168) now appears on the bus and the touch probe succeeds on the
  first attempt.
- Display draws with 0 SPI errors; SH8601 driver active (the earlier
  "display blank" was the wrong panel driver + missing power-up).
- No panic, no reboot loop; Wi-Fi connects from saved config and the
  speech-to-speech backend completes the WebRTC handshake
  (`PeerConnectionState: connected` → `completed`, `SESSION_UPDATE` and
  `GREETING` sent). The earlier "can't connect to backend" was the invisible
  crash loops, not the backend.
- One benign log line: `E sh8601: swap_xy is not supported by this panel`
  (the driver rejects a rotation op the BSP issues; we don't rotate).
- Regression: AIPI-Lite build still compiles (`AIPI_LITE_BOARD=ON`);
  Freenove is unaffected (same shared changes).

Still open: audio (the `bsp_audio_init()`/`esp_codec_dev` step — ES8311 is
on the same I2C bus at 0x18 and the BSP provides the codec API); the user
should confirm the display now renders and the touchscreen responds.
