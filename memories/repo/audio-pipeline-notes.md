# Audio Pipeline Notes

Concise repo memory for confirmed audio fixes. Keep entries short: what changed,
why it mattered, what symptom it fixed.

## AIPI-Lite: ES8311 audio dead (all-zero mic + silent speaker) — CONFIRMED FIX

- **Symptom:** Mic samples all zero, no speaker output, while I2C/registers read
  back fine (`RST=0x00` in the regs dump was the giveaway).
- **Fix:** Mirror the working Arduino sketch / ESPHome `es8311` driver for this
  exact board, in `src/es8311.c` + `src/media.cpp`:
  - 16 kHz both directions (not 24 kHz), 16-bit data in **32-bit slots**
    (BCLK = 64 × fs = 1.024 MHz, MCLK/BCLK = 4 → codec `bclk_div=4`).
  - Register 0x00 must end at **0x80 (power-on) as the last write**
    (0x1F → 0x00 → config → 0x80). Ending at 0x00 powers the codec down.
  - 16 kHz clock coefficients: CLK2=0x08 (pre_mult=1), CLK3=0x10, CLK4=0x20,
    CLK6=0x03. SDP 0x0C/0x0C. ADC vol 0xC8 (REG17), DAC vol 0xBF (REG32),
    mic gain 0x00 (REG16), REG37=0x08, REG31=0x00.
  - Mic capture: upper 16 bits of each 32-bit slot, L+R summed, ×12 gain
    (mirrors sketch `convert_input_to_backend_pcm`).
  - Playback: 16-bit samples left-aligned (`<< 16`) into 32-bit slots.
- **Why it mattered:** wrong PLL clock multiplier + wrong slot width + extra
  post-init register writes (`REG37=0x48`, `REG16=0x40`) all diverged from the
  proven reference and left the codec silent in both directions.
- **Measured result:** after the fix, the ES8311 logs
  `initialized (16 kHz, I2S slave, 16-bit in 32-bit slots)` with
  `RST=0x80 CLK2=0x08 CLK4=0x20`, mic shows non-zero energy, and assistant
  audio plays through the speaker.

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
