# Audio Pipeline Notes

Concise repo memory for confirmed audio fixes. Keep entries short: what changed,
why it mattered, what symptom it fixed.

## Display auto-off while Paused (battery saver)

- **What:** `status_display_task` (main.cpp) turns the backlight off after the
  device has been Paused for 10 s (`DISPLAY_AUTO_OFF_PAUSE_MS`) and back on
  the instant the button returns to "Listening". Backend: `lvgl_ui_set_backlight()`
  (lcd.cpp) — Waveshare `bsp_display_brightness_set(0|85)`, Freenove/AIPI
  `set_backlight_brightness(0|100)`.
- **Why:** while Paused the mic is muted and downlink audio is dropped, so the
  screen shows nothing useful — killing the backlight extends battery life,
  especially on the 2.06 watch.
- **Do not** pause the LVGL refresh timer or sleep the panel controller from
  here: the BSP owns the panel handle and a paused refresh timer strands the
  DMA mid-flush on the Waveshare boards.

## AIPI-Lite: ES8311 scratchy/static audio — CONFIRMED FIX (CLK2 pre_mult)

- **Symptom:** assistant audio sounds like static / missing bits. The codec was
  alive (regs read back, mic had energy, speaker made sound) — which is why an
  earlier attempt mislabeled it "working".
- **Root cause:** `src/es8311.c` had copied ESPHome's register constants, but
  ESPHome's `pre_mult << 3` write (CLK2=0x08, field 0b01 = ×2) is calibrated
  for a **128 × fs MCLK (2.048 MHz)**. This board's I2S feeds the codec
  **256 × fs (4.096 MHz)** MCLK, so the codec's internal clock ran 2× off and
  both DAC and ADC output garbage.
- **Fix:** CLK2 = **0x00** (pre_div=1, pre_mult=1), matching the espressif /
  ESP-ADF es8311 coefficient table for {mclk 4096000, rate 16000} — the same
  driver config the working Waveshare 1.8 board uses at 16 kHz. Everything else
  (32-bit slots, bclk_div=4, SDP 0x0C, OSR 16/32, RESET 0x80 last) was already
  correct.
- **Why it mattered:** a wrong pre_mult doubles the codec's internal clock;
  the ADC/DAC then run with a corrupted modulator clock → static, not silence,
  so it passed a "does it make sound" smoke test.
- **Measured result:** builds pass for Freenove, AIPI, Waveshare 1.8 and 2.06;
  AIPI assistant audio is clean. ESPHome's `es8311` values must not be
  copy-pasted blindly — they assume 128 × fs MCLK.

## AIPI-Lite display: 90° rotation, oversized fonts, unreadable WiFi text

- **Symptom:** text rotated 90° CCW; "Listening"/"Paused" wrapped; WiFi
  setup instructions unreadable (128×128 screen, no touch, no scroll).
- **Fix (`src/lcd.cpp`):** AIPI display flags now `swap_xy=true, mirror_x=true`
  (matching the xiaozhi-esp32 aipi-lite board config — all-false addressed the
  panel rotated 90°). AIPI UI constants: status font 16, message font 12,
  container pad-top 14, battery reserve 26, container inset 2 px each side,
  button padding 2 px. `src/wifi_config.cpp` messages shortened so the setup
  steps fit on one screen.
- **Message text on AIPI must be self-contained short lines (forced with
  `\n`), not one long wrapped string:** the WiFi instructions are e.g.
  `"Connect to WiFi\n\"OpenAi\"\nthen open\n192.168.4.1"` (~15 chars/line max).
  The container uses `LV_SCROLLBAR_MODE_OFF` (no touch input → nothing can
  scroll; a visible scrollbar also eats ~7 px of text width) and border width
  0. Don't reintroduce long single-line texts or the AUTO scrollbar — the top
  of the message gets cut off and can never be scrolled back.
- **Why it mattered:** AIPI reused the big-screen (480×320) fonts and layout on
  a 128×128 panel with no touch input — nothing could be read or scrolled.
- **Fonts:** `CONFIG_LV_FONT_MONTSERRAT_12/16` added to `sdkconfig.defaults`.

## Freenove/AIPI speaker buzz on interrupt button — CONFIRMED FIX

- **Symptom:** pressing the interrupt button during playback sometimes left
  the speaker buzzing with a "repeating last frame" noise that only stopped
  when the button was pressed again.
- **Root cause:** `oai_audio_decode()` stops writing to the I2S TX channel
  while interrupted; the DMA underruns and re-transmits the last 20 ms frame.
  `auto_clear = true` (alias of `auto_clear_after_cb`) is NOT sufficient on
  the ESP32-S3: it only zeroes buffers that pass through the TX_EOF callback,
  and a mid-write underrun can leave the GDMA re-sending a live buffer that
  never gets zeroed.
- **Fix:** `oai_silence_pump_task` in `src/media.cpp` (started via
  `oai_start_silence_pump()` from `oai_init_interrupt_button()`, non-BSP
  boards only): while interrupted it writes one zero 20 ms frame per 20 ms to
  the TX channel so the DMA never underruns. It is the sole writer while
  interrupted (decode path returns early); `i2s_channel_write` serializes on
  the channel's binary semaphore so enter/exit overlap is safe.
- **Why it mattered:** prevents the underrun instead of relying on auto_clear's
  unreliable underrun zeroing; no channel enable/disable (which rebooted the
  board when it raced the blocked `i2s_channel_write`).
- **Verified:** builds pass for Freenove, AIPI, Waveshare 1.8 and 2.06.

## WiFi connect-failure reboot loop — CONFIRMED FIX

- **Symptom:** Waveshare 1.8 "reboots after entering WiFi info" when the
  stored network is unavailable (trace showed clean `rst:0xc RTC_SW_CPU_RST`
  from `esp_restart()`, not a crash).
- **Fix (`src/wifi_config.cpp`):** on connect failure, stop STA, destroy the
  STA netif, and return to the AP portal in-place instead of
  `clear_nvs_config(); esp_restart();`. Saved config is kept, so a later boot
  with an available network still connects first try.
- **Why it mattered:** the old fail-fast wipe+reboot locked the user out of
  their credentials and reboot-looped on an unreachable network.

## Battery indicator (bottom bar) — all boards

- **What changed:** `src/battery.cpp` + `lvgl_ui_battery_set_percent()` in
  `src/lcd.cpp`; green horizontal bar with % text at the bottom, polled every
  5 s by `battery_display_task` in `main.cpp`. -1 (no battery) hides it.
- **Backends:** Freenove = GPIO20 ADC2_CH9, `battery_mV = adc_mV*2.5 - 3300`
  (shared LCD_RST pin, released to ADC after `init_lvgl()`); AIPI-Lite =
  GPIO2 ADC1_CH1 with the xiaozhi-esp32 raw table
  `{1480,0},{1581,20},{1663,40},{1750,60},{1840,80},{1980,100}`; Waveshare =
  AXP2101 at 0x34 on the BSP I2C bus, percent reg 0xA4 (fuel gauge),
  voltage fallback reg 0x34/0x35. Presence is detected from the battery
  voltage ADC (reg 0x34/0x35, ~2.0-4.5 V), NOT status1 reg 0x00 bit 3
  (unreliable on the 2.06).
- **Why it mattered:** all four boards have battery hardware but nothing
  displayed charge; Freenove's GPIO20 is the tricky one (LCD reset + divider
  on one pin, per the official FNK0102 docs).
- **Verified:** builds pass for Freenove, AIPI, Waveshare 1.8 and 2.06.
- **Validate on hardware:** battery % sanity per board; Freenove display
  still renders after GPIO20 floats to the divider voltage.
