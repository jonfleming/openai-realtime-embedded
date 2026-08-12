# OpenAI RealtimeAPI Embedded SDK

This fork is specifically customized for Freenove Media Kit and can be directly used on it.

## Maintainer Notes

For contributor and agent guidance, see [AGENTS.md](AGENTS.md).

It captures the non-obvious integration details and regression guardrails for:
- Freenove Media Kit audio pipeline
- libpeer / WebRTC signaling and data channel lifecycle
- Opus encoding/decoding assumptions
- speech-to-speech interoperability settings

## Platform/Device Support

* [Freenove ESP32-S3-WROOM](https://www.amazon.com/gp/product/B0BMQ8F7FN)
* AIPI-Lite (ESP32-S3 + ES8311)
* Waveshare ESP32-S3 Touch AMOLED 1.8 (default)
* Waveshare ESP32-S3 Touch AMOLED 2.06 (watch form factor)

## Board Selection

This project supports four board profiles selected at configure/build time
(precedence Waveshare 2.06 > Waveshare 1.8 > AIPI-Lite > Freenove):

- Waveshare ESP32-S3 Touch AMOLED 1.8 (default): `WAVESHARE_AMOLED_1_8_BOARD=ON`
- Waveshare ESP32-S3 Touch AMOLED 2.06: `-DWAVESHARE_AMOLED_2_06_BOARD=ON`
- AIPI-Lite: `-DAIPI_LITE_BOARD=ON` (with `WAVESHARE_AMOLED_1_8_BOARD=OFF`)
- Freenove Media Kit: `-DAIPI_LITE_BOARD=OFF -DWAVESHARE_AMOLED_1_8_BOARD=OFF`

Examples:

- Configure/build for Waveshare 1.8 (default): plain `idf.py build`
- Configure/build for Waveshare 2.06: `idf.py -DWAVESHARE_AMOLED_2_06_BOARD=ON build`
- Configure/build for AIPI-Lite: `idf.py -DWAVESHARE_AMOLED_1_8_BOARD=OFF -DAIPI_LITE_BOARD=ON build`
- Configure/build for Freenove: `idf.py -DWAVESHARE_AMOLED_1_8_BOARD=OFF -DAIPI_LITE_BOARD=OFF build`

Notes for the Waveshare 2.06 board:

- 2.06" 410x502 QSPI AMOLED (CO5300), FT3168 touch, 32 MB flash, 8 MB PSRAM,
  ES8311 speaker DAC + ES7210 dual-mic ADC.
- Uses the official managed BSP `waveshare/esp32_s3_touch_amoled_2_06`; only
  one Waveshare BSP is linked per build (they export the same `bsp_*`
  symbols), selected by the `FREEBUFF_BOARD` env var set in `CMakeLists.txt`.
- Console is the default UART0 (the board's USB-C connects to a USB-UART
  bridge, not the ESP32-S3 native USB), so no console sdkconfig override is
  needed (unlike the 1.8 board).
- Flash size is 32 MB (QIO) via `sdkconfig.waveshare_amoled_2_06`.
- See `WAVESHARE-AMOLED-2.06-SUPPORT.md` for the full port/bring-up notes.

Notes for AIPI-Lite:

- GPIO10 power keep-alive is driven HIGH at startup.
- I2S pins use the AIPI-Lite mapping (MCLK=6, BCLK=14, LRCLK=12, DIN=13, DOUT=11).
- Speaker amplifier enable uses GPIO9 and is turned on during startup.
- Display/backlight pins differ from Freenove (see AIPI-Lite-GPIO-Pins.md).

## Installation & Usage

### ESP32-S3 & Usage

1. Install [IDF SDK](https://github.com/espressif/esp-idf) according to the [tutorial](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s3/get-started/index.html#get-started-how-to-get-esp-idf).

2. Clone code and submodules.

   `git clone --recurse-submodules https://github.com/Freenove/openai-realtime-embedded`

3. Set target platform (if necessary).

   `idf.py set-target esp32s3`

4. Build.

   `idf.py build`

5. Flash to the device.

   `idf.py flash`

6. Open monitor (optional).

   `idf.py monitor`

7. Connect your mobile phone or computer to a router named "OpenAi", which has no password.

8. After successful connection, use the browser to access "192.168.4.1".

9. Set your Wifi SSID, Password, and openai api key in browser. 

   If you don't know OpenAI API key, you need to [register and purchase](https://platform.openai.com/) it. Currently, OpenAI does not provide free services.

10. Done! Now you can have a conversation with OpenAI !

### Linux & Usage
1. Install [IDF SDK](https://github.com/espressif/esp-idf) according to the [tutorial](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s3/get-started/index.html#get-started-how-to-get-esp-idf).

2. Clone code and submodules.

   `git clone --recurse-submodules https://github.com/Freenove/openai-realtime-embedded`

3. Generate `privateConfig.json` file (File isolation is to protect your password and key).

   `cp privateConfig.common.json privateConfig.json`

4. Set your Wifi SSID, Password, and openai api key in file `privateConfig.json`. If you don't know OpenAI API key, you need to [register and purchase](https://platform.openai.com/) it. Currently, OpenAI does not provide free services.

   ```
     "wifi_ssid": "xxxxx",
     "wifi_password": "xxxxx",
     "openai_api_key": "xxxxx"
   ```

5. Set target platform (if necessary).

   `idf.py set-target esp32s3`

6. Build.

   `idf.py build`

7. Flash to the device.

   `idf.py flash`

8. Open monitor (optional).

   `idf.py monitor`

9. Done! Now you can have a conversation with OpenAI !



