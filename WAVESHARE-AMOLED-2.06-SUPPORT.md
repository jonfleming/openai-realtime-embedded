# Plan: Add support for Waveshare ESP32-S3 Touch AMOLED 2.06 (watch)

Status: **Implementation done, build-verified** for all four board profiles
(2.06, 1.8, AIPI-Lite, Freenove) with ESP-IDF 5.5.5. This document is the
design record + hardware bring-up checklist for the 2.06 watch board profile
in `openai-realtime-embedded`. **Audio has NOT yet been validated on physical
hardware** — see §10.

---

## 1. Board facts (verified against Waveshare wiki, first-party repo + BSP source)

| Feature | Detail |
| --- | --- |
| MCU | ESP32-S3, 8 MB octal PSRAM, **32 MB flash (QIO)** |
| Display | 2.06" **410×502** QSPI AMOLED — **CO5300** controller (driven by the official BSP via the `esp_lcd_sh8601` QSPI driver with the exact 410×502 window; this is what Waveshare's factory firmware, examples and third-party projects use) |
| Touch | FT3168 (I2C), reset wired to **GPIO9** directly (no TCA9554 power gate like the 1.8 V1.0) |
| Audio | **ES8311 speaker DAC + ES7210 dual-mic ADC** on one shared I2S bus — stereo in/out |
| Console | **USB-UART bridge** → default UART0 console works (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` NOT needed, unlike the 1.8) |
| Form factor | Watch (comes with strap); no RST button (PWR + BOOT side buttons) |

Official managed BSP: **`waveshare/esp32_s3_touch_amoled_2_06` `^2.0.0`**.
Waveshare's own current example (`02_lvgl_demo_v9`) pins `^2.0.0` with the
comment *"v2 is the current board BSP line"*. BSP 1.x vs 2.x is a board-line
difference; all 2.06 BSP versions use the SH8601 driver for this panel.

## 2. Key design decision: one managed BSP per board, selected at resolve time

Both Waveshare BSPs export the **same `bsp_*` symbols** (`bsp_i2c_init`,
`bsp_display_new`, `bsp_audio_init`, ...), so linking both fails with
duplicate-symbol errors. They cannot both be in one build — the active board's
BSP must be the only one resolved.

Mechanism (the tricky part — plain CMake options can't gate an
`idf_component.yml` dependency; `rules:` only know `target`/`idf_version`):

1. `CMakeLists.txt` sets the **`FREEBUFF_BOARD` environment variable** (to
   `waveshare_amoled_1_8` or `waveshare_amoled_2_06`) *before* `project()`
   runs the component manager.
2. `src/idf_component.yml` uses `rules:` with an **`if:` expression on the env
   var** (supported by the component manager bundled with IDF 5.5.5 —
   verified in its `if_parser.py`; unset env vars error, so CMake always sets
   it):
   ```yaml
   waveshare/esp32_s3_touch_amoled_1_8:
     version: ^1.1.4
     require: public
     rules:
       - if: "$FREEBUFF_BOARD != waveshare_amoled_2_06"
   waveshare/esp32_s3_touch_amoled_2_06:
     version: ^2.0.0
     require: public
     rules:
       - if: "$FREEBUFF_BOARD == waveshare_amoled_2_06"
   ```
3. `src/CMakeLists.txt` does **NOT** list the BSP in `REQUIRES` (and not the
   1.8-only `esp_io_expander*` — those flow from the 1.8 BSP's public deps).
   The manifest's `require: public` pulls the selected BSP into the build.

Verified both directions: the 2.06 build graph contains only the 2.06 BSP
(+ its `esp_lcd_sh8601`, `esp_codec_dev`, `esp_lcd_touch_ft5x06` deps), and
the default 1.8 build contains only the 1.8 BSP (the manager even deletes the
stale component from `managed_components/`).

## 3. Board selection

Precedence: **Waveshare 2.06 > Waveshare 1.8 > AIPI-Lite > Freenove**.
- `-DWAVESHARE_AMOLED_2_06_BOARD=ON` → 2.06 watch, overlays
  `sdkconfig.waveshare_amoled_2_06`
- `-DWAVESHARE_AMOLED_1_8_BOARD=ON` (**default**, no flag needed) → 1.8
- `-DWAVESHARE_AMOLED_1_8_BOARD=OFF -DAIPI_LITE_BOARD=ON` → AIPI-Lite
- both `OFF` → Freenove

`WAVESHARE_BSP_BOARD=1` is defined for **either** Waveshare board and guards
all code that is identical across the two BSPs (BSP includes, display init
sequence, codec-dev audio plumbing, `lcd_disp_lock` routing); the
`WAVESHARE_AMOLED_1_8_BOARD` / `WAVESHARE_AMOLED_2_06_BOARD` macros guard the
per-board differences (resolution, TCA9554 pulse, stereo vs mono).

Build on this machine (PowerShell, per AGENTS.md):

    idf.py -DWAVESHARE_AMOLED_2_06_BOARD=ON build
    idf.py -p COM4 flash        # USB-UART bridge → normal COM port
    idf.py -p COM4 monitor

When switching boards, delete the generated `sdkconfig` (or `idf.py
fullclean`) so the per-board overlay regenerates.

## 4. Display / LVGL (`src/lcd.cpp`)

- Pin block: 410×502 for `WAVESHARE_AMOLED_2_06_BOARD`.
- `init_lvgl()` replicates `bsp_display_start()` with public BSP APIs (same as
  the 1.8 path — see `WAVESHARE-AMOLED-1.8-SUPPORT.md` §12.7 for why the
  direct call is unsafe with this project's `NDEBUG` build): `lvgl_port_init`
  → `bsp_display_new` → `lvgl_port_add_disp` → touch (probe retried 5×,
  **never fatal**) → brightness.

  > **Why `lvgl_port_add_disp` and not `lvgl_port_add_disp_rgb`?** The panel is
  > QSPI/SPI, not RGB. The `_rgb` variant marks the display as RGB, which makes
  > esp_lvgl_port call `lv_disp_flush_ready()` immediately after queueing the
  > async SPI transaction; LVGL then reuses the single 20-row draw buffer while
  > the SPI DMA is still reading it, leaving a corrupted horizontal strip at
  > each buffer boundary (seen as a white line through text/buttons). The plain
  > variant signals flush-ready only after the SPI transfer completes (see
  > `WAVESHARE-AMOLED-1.8-SUPPORT.md` §12.7 issue B2).
- **`max_transfer_sz` must be nonzero** — the 2.06 BSP asserts it in
  `bsp_display_new()` (the 1.8 BSP doesn't check). Set to
  `DISPLAY_WIDTH * DISPLAY_HEIGHT * 2`.
- LVGL buffer follows this project's proven convention (DMA buffer,
  `sw_rotate=false`, 20 rows, `swap_bytes=true`) plus the even-coordinate
  invalidate rounder (LVGL ≥9) for the 16-bit-aligned QSPI panels.
- **No TCA9554 pulse on 2.06** (touch reset = GPIO9; the expander pulse stays
  `#if WAVESHARE_AMOLED_1_8_BOARD`).
- No LEDC backlight on either Waveshare board (brightness is a panel command
  via `bsp_display_brightness_set`).

## 5. Audio (`src/media.cpp`)

The 2.06 topology is different from the 1.8: **ES7210 dual-mic ADC + ES8311
speaker DAC, one shared I2S bus, stereo**. It mirrors the Waveshare
Spec_Analyzer example recipe:

- `bsp_audio_init(NULL)` → then **`bsp_i2c_init()` explicitly** (the BSP's
  codec-init helpers only bring up I2C when `i2s_data_if` is NULL, and
  `bsp_audio_init` runs first, so the codec control interface would be dead
  without it) → `bsp_audio_codec_speaker_init()` +
  `bsp_audio_codec_microphone_init()`.
- Open **both** `esp_codec_dev` handles at **16 kHz / 16-bit / 2 channels**
  (the BSP's own I2S default is mono 22.05 kHz, but `esp_codec_dev` reconfigures
  the shared I2S slot/clock on open — verified in its source).
- Speaker volume **100** (= 0 dB on the codec-dev default curve; the
  Waveshare example's `CODEC_DEFAULT_VOLUME=60` is **-20 dB** — roughly
  -16 dB effective at the DAC after the BSP's PA compensation — which is far
  too quiet on the watch speaker). Mic input gain **24.0 dB**
  (`CODEC_DEFAULT_ADC_VOLUME` in the Waveshare example).
- Decode: `esp_codec_dev_write(speaker, output_buffer, decoded*2*2 bytes)` —
  straight stereo passthrough (no mono downmix; the 1.8 path keeps its
  `SPK_GAIN` mono downmix).
- Mic read: `esp_codec_dev_read(mic, ...)` for `320 * 2 * sizeof(int16_t)`
  bytes (one 20 ms stereo frame), then **average L+R** → mono before Opus
  encode, with L/R energy diagnostics (same pattern as the other boards).
- 1.8 keeps mono 16 kHz; AIPI/Freenove raw-I2S paths untouched.

## 6. sdkconfig overlay (`sdkconfig.waveshare_amoled_2_06`)

Loaded on top of `sdkconfig.defaults` when 2.06 is selected
(`SDKCONFIG_DEFAULTS` wiring in `CMakeLists.txt`):

```
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y      # Waveshare examples mandate QIO
CONFIG_ESPTOOLPY_FLASHSIZE_32MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="32MB"
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y
CONFIG_LV_FONT_MONTSERRAT_24=y
```

No console override: the 2.06's USB-UART bridge keeps the default UART0
console (only the 1.8 needs `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`).
Note: esptool's flash-args string always says "dio" in IDF ≥5; the real mode
is `CONFIG_ESPTOOLPY_FLASHMODE_QIO=y` in the image.

## 7. Build verification (done)

All four profiles build cleanly with ESP-IDF 5.5.5 (fresh per-board build
dirs, `-D SDKCONFIG=<build_dir>/sdkconfig` so each board gets a clean config):

| Profile | Flag | Result |
| --- | --- | --- |
| 2.06 watch | `-DWAVESHARE_AMOLED_2_06_BOARD=ON` | ✅ 32 MB / QIO / 80M PSRAM / 64B cache line confirmed in `config/sdkconfig`; only the 2.06 BSP in the build graph |
| 1.8 | default (no flag) | ✅ 16 MB, only the 1.8 BSP in the graph |
| AIPI-Lite | `-DWAVESHARE_AMOLED_1_8_BOARD=OFF -DAIPI_LITE_BOARD=ON` | ✅ |
| Freenove | both `OFF` | ✅ |

## 8. Flashing / monitoring on hardware

- The 2.06's USB-C is a **USB-UART bridge**, so it enumerates as a normal
  COM port and `idf.py -p COMx flash` / `monitor` just work (no
  USB-Serial-JTAG, no strap/download-mode dance like the 1.8).
- **No RST button** (PWR + BOOT only) — if the chip ever sticks in ROM
  download mode, recover with `python/s3_recover.py COMx` (esptool's
  `--after watchdog-reset`, tracked in the repo; a PWR double-press is the
  physical power-cycle equivalent). Same as the 1.8.

## 9. Validation checklist (hardware bring-up, in order)

1. `idf.py -DWAVESHARE_AMOLED_2_06_BOARD=ON build` succeeds (✅ done).
2. Flash + monitor: boots, BSP logs panel/touch init; **no panic in
   `bsp_display_new`** (confirms `max_transfer_sz` handling) and no
   "Touch not found"-style NULL deref (touch failure must be non-fatal).
3. Display: LVGL UI renders 410×502; `bsp_display_brightness_set()` works.
   Watch for the `swap_xy is not supported` warning (benign, same as 1.8).
4. Touch: taps register (I2C scan should show `0x38` = FT3168; the 2.06 has
   no TCA9554 power gate, so it should answer immediately).
5. Mic: `esp_codec_dev_read` returns non-zero L+R energy at 16 kHz stereo —
   confirms the ES7210 is clocked correctly and the L/R interleave assumption
   is right (adjust the extractor if energy is one-sided).
6. Speaker: assistant audio audible at vol 100; if distorted, the ES8311 DAC
   at stereo 16 kHz is the first thing to re-check.
7. End-to-end: Wi-Fi → SDP → `PEER_CONNECTION_COMPLETED` → data channel →
   `SESSION_UPDATE` → conversational turn (reuse AGENTS.md debug markers).

## 10. Risks / open questions

- **Audio not yet hardware-validated** (no physical 2.06 board attached in
  this session). The recipe is copied from Waveshare's Spec_Analyzer example,
  but channel count, gain and volume are worth confirming on real hardware
  (checklist steps 5-7).
- **Display correctness on the actual panel**: the BSP drives the CO5300
  through the SH8601 QSPI driver — that is the official Waveshare path (their
  factory firmware + examples + third-party watch projects use it), but if the
  panel ever shows wrong colors/orientation, revisit `invert_color` /
  `swap_xy` / `rgb_order` in the BSP's panel config before touching the driver.
- **Shared-I2S constraint**: ES7210 ADC and ES8311 DAC share one bus/clock, so
  mic and speaker rates are locked together (16 kHz both ways here) — do not
  diverge them.
- BSP version pin: `^2.0.0` matches Waveshare's current example line and the
  CO5300 board; if a hardware revision split ever appears for the 2.06
  (like the 1.8's V1/V2), re-check the BSP README's HW-version table.
