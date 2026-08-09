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
