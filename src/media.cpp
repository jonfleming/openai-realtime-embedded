#include <driver/i2s_std.h>
#include <driver/gpio.h>
#include <opus.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "main.h"

#if defined(WAVESHARE_AMOLED_2_06_BOARD) && WAVESHARE_AMOLED_2_06_BOARD
// 2.06 watch board: ES8311 codec for the speaker + ES7210 ADC for the dual
// digital mics, both on the BSP-managed I2C/I2S buses. Audio goes through
// bsp_audio_* + esp_codec_dev at 16 kHz stereo (see the Waveshare
// Spec_Analyzer example), not raw I2S channel handles.
#include "bsp/esp32_s3_touch_amoled_2_06.h"
#elif defined(WAVESHARE_AMOLED_1_8_BOARD) && WAVESHARE_AMOLED_1_8_BOARD
// The Waveshare BSP owns the ES8311 codec (I2C bus + full-duplex I2S on the
// Media Kit), so audio goes through bsp_audio_* + esp_codec_dev, not raw
// I2S channel handles.
#include "bsp/esp32_s3_touch_amoled_1_8.h"
#endif
#if defined(WAVESHARE_BSP_BOARD) && WAVESHARE_BSP_BOARD
#include "esp_codec_dev.h"
#endif

#if defined(AIPI_LITE_BOARD) && AIPI_LITE_BOARD
#define SPK_SAMPLE_RATE 16000
#define SPK_BUFFER_SAMPLES 320  // 20ms at 16kHz
#elif defined(WAVESHARE_BSP_BOARD) && WAVESHARE_BSP_BOARD
// Waveshare boards: one shared I2S bus with the codecs (ES8311 DAC and, on
// the 2.06, the ES7210 ADC), so both directions must run at one rate.
// 16 kHz matches the mic/Opus encoder and the proven AIPI-Lite setup.
#define SPK_SAMPLE_RATE 16000
#define SPK_BUFFER_SAMPLES 320  // 20ms at 16kHz
#else
#define SPK_SAMPLE_RATE 48000
#define SPK_BUFFER_SAMPLES 960  // 20ms at 48kHz
#endif
#define SPK_CHANNELS 2

#define MIC_OPUS_OUT_BUFFER_SIZE 1276

#if defined(AIPI_LITE_BOARD) && AIPI_LITE_BOARD
// AIPI-Lite: single ES8311 codec on one shared I2S bus.
// Mirrors the working Arduino sketch / stock firmware for this exact board:
// 16 kHz, 16-bit data in 32-bit slots (BCLK = 64 * fs = 1.024 MHz,
// MCLK/BCLK = 4), MCLK 256 * fs = 4.096 MHz on GPIO6.
#define MIC_SAMPLE_RATE 16000
#define MIC_BUFFER_SAMPLES 320  // 20ms at 16kHz per channel
#define AUDIO_I2S_PORT I2S_NUM_0
#define MCLK_PIN 6       // ES8311 MCLK (256 * fs = 4.096 MHz @ 16 kHz)
#define DAC_BCLK_PIN 14  // BCLK (shared)
#define DAC_LRCLK_PIN 12 // LRCLK (shared)
#define DAC_DATA_PIN 11  // DOUT (to speaker)
#define ADC_BCLK_PIN 14  // BCLK (shared)
#define ADC_LRCLK_PIN 12 // LRCLK (shared)
#define ADC_DATA_PIN 13  // DIN (from mic)
#define SPEAKER_AMP_ENABLE_PIN 9
#define MIC_BYTES_PER_SLOT 4   // 32-bit slots
#elif defined(WAVESHARE_AMOLED_2_06_BOARD) && WAVESHARE_AMOLED_2_06_BOARD
// 2.06: ES7210 dual-mic ADC delivers 16-bit stereo PCM (L/R interleaved) at
// 16 kHz on the shared I2S bus, same rate as the ES8311 speaker DAC.
#define MIC_SAMPLE_RATE 16000
#define MIC_BUFFER_SAMPLES 320  // 20ms at 16kHz per channel
#define MIC_BYTES_PER_SLOT 2    // 16-bit samples (stereo)
#else
// Freenove Media Kit: direct MEMS mic + NS4168 amp on separate I2S buses.
#define MIC_SAMPLE_RATE 16000
#define MIC_BUFFER_SAMPLES 320  // 20ms at 16kHz per channel
#define TX_I2S_PORT I2S_NUM_0
#define RX_I2S_PORT I2S_NUM_1
// speaker
#define MCLK_PIN -1
#define DAC_BCLK_PIN 42   // BCLK
#define DAC_LRCLK_PIN 41  // LRCLK
#define DAC_DATA_PIN 1    // SDATA
// microphone
#define ADC_BCLK_PIN 3    // SCK -- BCLK
#define ADC_LRCLK_PIN 14  // WS  -- LRCLK
#define ADC_DATA_PIN 46   // SD  -- DATA
#define MIC_BYTES_PER_SLOT 4   // 32-bit slots
#endif

#define MIC_I2S_CHANNELS 2     // stereo capture avoids ESP32-S3 mono DMA padding artifact
#define MIC_CHANNELS 1         // Opus mono encode
#define OPUS_OUT_BUFFER_SIZE 1276  // 1276 bytes is recommended by opus_encode

#define OPUS_ENCODER_BITRATE 32000
#define OPUS_ENCODER_COMPLEXITY 2
#define MIC_GAIN 7  // linear gain applied before encode; 4 = +12 dB; increase if VAD still misses speech

#if defined(WAVESHARE_AMOLED_1_8_BOARD) && WAVESHARE_AMOLED_1_8_BOARD
// Linear gain applied to the averaged L+R downmix before the mono speaker.
// 2 restores the full-scale sum (+6 dB over the plain average); reduce to 1
// if loud content clips, or raise the codec volume instead.
#define SPK_GAIN 2
#endif

static i2s_chan_handle_t s_i2s_tx_chan = NULL;
static i2s_chan_handle_t s_i2s_rx_chan = NULL;

#if defined(WAVESHARE_BSP_BOARD) && WAVESHARE_BSP_BOARD
static esp_codec_dev_handle_t s_spk_codec_dev = NULL;
static esp_codec_dev_handle_t s_mic_codec_dev = NULL;
#endif

// ESP32-S3 has no APLL for I2S; PLL_160M gives MCLK within ~0.16% of
// 256*fs, which the ES8311 tolerates (same as the official i2s_es8311 example).
static constexpr i2s_clock_src_t MIC_CLK_SRC = I2S_CLK_SRC_DEFAULT;
static constexpr const char* MIC_CLK_SRC_NAME = "DEFAULT";

void oai_init_audio_capture() {
#if defined(AIPI_LITE_BOARD) && AIPI_LITE_BOARD
  // One full-duplex I2S controller for both TX (speaker) and RX (mic).
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_I2S_PORT, I2S_ROLE_MASTER);
  chan_cfg.auto_clear = true;  // don't repeat the last frame on TX underrun
  if (i2s_new_channel(&chan_cfg, &s_i2s_tx_chan, &s_i2s_rx_chan) != ESP_OK) {
    ESP_LOGE("Media", "Failed to create I2S channels");
    return;
  }

  // 32-bit slots: BCLK = 16k * 2 * 32 = 1.024 MHz, matching the ES8311
  // bclk_div=4 setup in es8311.c (MCLK/BCLK = 4). 16-bit data rides in the
  // upper 16 bits of each slot, exactly like the working Arduino sketch.
  i2s_std_config_t std_cfg = {
      .clk_cfg = {
          .sample_rate_hz = MIC_SAMPLE_RATE,
          .clk_src = MIC_CLK_SRC,
          .ext_clk_freq_hz = 0,
          .mclk_multiple = I2S_MCLK_MULTIPLE_256,
          .bclk_div = 0,
      },
      .slot_cfg = {
          .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
          .slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT,
          .slot_mode = I2S_SLOT_MODE_STEREO,
          .slot_mask = I2S_STD_SLOT_BOTH,
          .ws_width = I2S_DATA_BIT_WIDTH_32BIT,
          .ws_pol = false,
          .bit_shift = true,
          .left_align = true,
          .big_endian = false,
          .bit_order_lsb = false,
      },
      .gpio_cfg = {
          .mclk = static_cast<gpio_num_t>(MCLK_PIN),
          .bclk = static_cast<gpio_num_t>(DAC_BCLK_PIN),
          .ws = static_cast<gpio_num_t>(DAC_LRCLK_PIN),
          .dout = static_cast<gpio_num_t>(DAC_DATA_PIN),
          .din = static_cast<gpio_num_t>(ADC_DATA_PIN),
          .invert_flags = {
              .mclk_inv = false,
              .bclk_inv = false,
              .ws_inv = false,
          },
      },
  };
  if (i2s_channel_init_std_mode(s_i2s_tx_chan, &std_cfg) != ESP_OK) {
    ESP_LOGE("Media", "Failed to initialize I2S TX std mode");
    return;
  }
  if (i2s_channel_init_std_mode(s_i2s_rx_chan, &std_cfg) != ESP_OK) {
    ESP_LOGE("Media", "Failed to initialize I2S RX std mode");
    return;
  }
#elif defined(WAVESHARE_BSP_BOARD) && WAVESHARE_BSP_BOARD
  // Waveshare boards: codecs on the BSP-managed I2C/I2S buses.
  //  - 1.8 ("Media Kit"): one ES8311 codec on the full-duplex I2S bus
  //    (SCLK 9 / MCLK 16 / LCLK 45 / DOUT 8 / DIN 10).
  //  - 2.06 (watch): ES8311 speaker DAC + ES7210 dual-mic ADC on one shared
  //    I2S bus (SCLK 41 / MCLK 16 / LCLK 45 / DOUT 40 / DSIN 42).
  // The BSP owns the I2C bus (SCL 14 / SDA 15) the codecs sit on, so do NOT
  // claim those pins with raw I2S here (GPIO 14 is the BSP's I2C SCL and
  // breaks touch detection / display init).
#if defined(WAVESHARE_AMOLED_2_06_BOARD) && WAVESHARE_AMOLED_2_06_BOARD
  ESP_LOGI("Media", "Waveshare board: initializing BSP audio (ES8311 spk + ES7210 mic, 16 kHz stereo)");
#else
  ESP_LOGI("Media", "Waveshare board: initializing BSP audio (ES8311, 16 kHz mono)");
#endif
  if (bsp_audio_init(NULL) != ESP_OK) {
    ESP_LOGE("Media", "Failed to init BSP audio (I2S)");
    return;
  }
  // The BSP's codec init functions only bring up the I2C bus when
  // i2s_data_if is still NULL; since bsp_audio_init() ran first they skip
  // it, leaving i2c_handle NULL and the codec control interfaces unusable
  // (es8311_codec_new -> "Wrong codec config", codec never configured ->
  // all-zero mic / silent DAC). Create the bus here explicitly.
  esp_err_t i2c_ret = bsp_i2c_init();
  if (i2c_ret != ESP_OK) {
    ESP_LOGE("Media", "bsp_i2c_init failed: %s", esp_err_to_name(i2c_ret));
    return;
  }
  s_spk_codec_dev = bsp_audio_codec_speaker_init();
  s_mic_codec_dev = bsp_audio_codec_microphone_init();
  if (s_spk_codec_dev == NULL || s_mic_codec_dev == NULL) {
    ESP_LOGE("Media", "BSP codec device init failed (spk=%p, mic=%p)",
             s_spk_codec_dev, s_mic_codec_dev);
    return;
  }
  esp_codec_dev_sample_info_t fs = {
      .bits_per_sample = 16,
#if defined(WAVESHARE_AMOLED_2_06_BOARD) && WAVESHARE_AMOLED_2_06_BOARD
      // 2.06: ES7210 outputs L/R from the dual mics; the ES8311 DAC plays
      // stereo back (Waveshare Spec_Analyzer recipe: 16 kHz / 16-bit / 2ch).
      .channel = 2,
#else
      .channel = 1,
#endif
      .channel_mask = 0,
      .sample_rate = MIC_SAMPLE_RATE,
      .mclk_multiple = 0,
  };
  if (esp_codec_dev_open(s_spk_codec_dev, &fs) != ESP_CODEC_DEV_OK ||
      esp_codec_dev_open(s_mic_codec_dev, &fs) != ESP_CODEC_DEV_OK) {
    ESP_LOGE("Media", "Failed to open BSP codec devices");
    return;
  }
  // Codec volume: the codec-dev default curve maps 0-100 to -50..0 dB, and
  // the uninitialized dev->volume starts at 0 (-50 dB) until set here.
#if defined(WAVESHARE_AMOLED_2_06_BOARD) && WAVESHARE_AMOLED_2_06_BOARD
  // 2.06: volume 60 on the codec-dev curve is -20 dB (≈ -16 dB effective at
  // the DAC after the BSP's PA compensation), which is far too quiet on the
  // watch speaker. Use 100 = 0 dB (≈ +3.6 dB effective), same as the 1.8
  // board. Also set the ES7210 ADC gain (24 dB, the Waveshare example's
  // CODEC_DEFAULT_ADC_VOLUME).
  esp_codec_dev_set_out_vol(s_spk_codec_dev, 100);
  esp_codec_dev_set_in_gain(s_mic_codec_dev, 24.0);
  ESP_LOGI("Media", "OAI Audio Capture Initialized (BSP codec, 16 kHz stereo in/out, vol=100)");
#else
  esp_codec_dev_set_out_vol(s_spk_codec_dev, 100);
  ESP_LOGI("Media", "OAI Audio Capture Initialized (BSP codec, 16 kHz mono in/out, vol=100)");
#endif
  return;
#else
  i2s_chan_config_t tx_chan_cfg =
      I2S_CHANNEL_DEFAULT_CONFIG(TX_I2S_PORT, I2S_ROLE_MASTER);
  if (i2s_new_channel(&tx_chan_cfg, &s_i2s_tx_chan, NULL) != ESP_OK) {
    ESP_LOGE("Media", "Failed to create I2S TX channel");
    return;
  }

  i2s_std_config_t tx_std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SPK_SAMPLE_RATE),
      .slot_cfg =
          I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {
          .mclk = static_cast<gpio_num_t>(MCLK_PIN),
          .bclk = static_cast<gpio_num_t>(DAC_BCLK_PIN),
          .ws = static_cast<gpio_num_t>(DAC_LRCLK_PIN),
          .dout = static_cast<gpio_num_t>(DAC_DATA_PIN),
          .din = I2S_GPIO_UNUSED,
          .invert_flags = {
              .mclk_inv = false,
              .bclk_inv = false,
              .ws_inv = false,
          },
      },
  };
  if (i2s_channel_init_std_mode(s_i2s_tx_chan, &tx_std_cfg) != ESP_OK) {
    ESP_LOGE("Media", "Failed to initialize I2S TX std mode");
    return;
  }

  i2s_chan_config_t rx_chan_cfg =
      I2S_CHANNEL_DEFAULT_CONFIG(RX_I2S_PORT, I2S_ROLE_MASTER);
  if (i2s_new_channel(&rx_chan_cfg, NULL, &s_i2s_rx_chan) != ESP_OK) {
    ESP_LOGE("Media", "Failed to create I2S RX channel");
    return;
  }

  i2s_std_config_t rx_std_cfg = {
      // 32-bit slot gives BCLK=1.024 MHz at 16 kHz (mics need ≥1 MHz)
      .clk_cfg = {
          .sample_rate_hz = MIC_SAMPLE_RATE,
          .clk_src = MIC_CLK_SRC,
          .ext_clk_freq_hz = 0,
          .mclk_multiple = I2S_MCLK_MULTIPLE_256,
          .bclk_div = 0,
      },
      .slot_cfg = {
          .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
          .slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT,
          .slot_mode = I2S_SLOT_MODE_STEREO,
          .slot_mask = I2S_STD_SLOT_BOTH,
          .ws_width = 32,
          .ws_pol = false,
          .bit_shift = true,
          .left_align = true,
          .big_endian = false,
          .bit_order_lsb = false,
      },
      .gpio_cfg = {
          .mclk = static_cast<gpio_num_t>(MCLK_PIN),
          .bclk = static_cast<gpio_num_t>(ADC_BCLK_PIN),
          .ws = static_cast<gpio_num_t>(ADC_LRCLK_PIN),
          .dout = I2S_GPIO_UNUSED,
          .din = static_cast<gpio_num_t>(ADC_DATA_PIN),
          .invert_flags = {
              .mclk_inv = false,
              .bclk_inv = false,
              .ws_inv = false,
          },
      },
  };
  if (i2s_channel_init_std_mode(s_i2s_rx_chan, &rx_std_cfg) != ESP_OK) {
    ESP_LOGE("Media", "Failed to initialize I2S RX std mode");
    return;
  }
#endif

  if (i2s_channel_enable(s_i2s_tx_chan) != ESP_OK) {
    ESP_LOGE("Media", "Failed to enable I2S TX channel");
    return;
  }

  if (i2s_channel_enable(s_i2s_rx_chan) != ESP_OK) {
    ESP_LOGE("Media", "Failed to enable I2S RX channel");
    return;
  }

#if defined(AIPI_LITE_BOARD) && AIPI_LITE_BOARD
  // Enable the AIPI-Lite speaker amplifier.
  gpio_config_t amp_conf = {};
  amp_conf.intr_type = GPIO_INTR_DISABLE;
  amp_conf.mode = GPIO_MODE_OUTPUT;
  amp_conf.pin_bit_mask = (1ULL << SPEAKER_AMP_ENABLE_PIN);
  amp_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  amp_conf.pull_up_en = GPIO_PULLUP_DISABLE;
  if (gpio_config(&amp_conf) == ESP_OK) {
    gpio_set_level(static_cast<gpio_num_t>(SPEAKER_AMP_ENABLE_PIN), 1);
    ESP_LOGI("Media", "Speaker amplifier enabled (pin %d)", SPEAKER_AMP_ENABLE_PIN);
  } else {
    ESP_LOGE("Media", "Failed to configure speaker amplifier pin");
  }
#endif

  ESP_LOGI("Media", "Mic clock source=%s, sample_rate=%d, slot_bits=%d, channels=%d, expected_BCLK=%d Hz",
           MIC_CLK_SRC_NAME,
           MIC_SAMPLE_RATE,
           MIC_BYTES_PER_SLOT * 8,
           MIC_I2S_CHANNELS,
           MIC_SAMPLE_RATE * MIC_I2S_CHANNELS * MIC_BYTES_PER_SLOT * 8);
  ESP_LOGI("Media","OAI Audio Capture Initialized");
}

opus_int16 *output_buffer = NULL;
opus_int32 *output_buffer_32 = NULL;
opus_int16 *output_buffer_mono = NULL;
OpusDecoder *opus_decoder = NULL;

void oai_init_audio_decoder() {
  int decoder_error = 0;
  opus_decoder = opus_decoder_create(SPK_SAMPLE_RATE, SPK_CHANNELS, &decoder_error);
  if (decoder_error != OPUS_OK) {
    ESP_LOGE("Media", "Failed to create OPUS decoder");
    return;
  }

  output_buffer =
      (opus_int16 *)malloc(SPK_BUFFER_SAMPLES * SPK_CHANNELS * sizeof(opus_int16));
#if defined(AIPI_LITE_BOARD) && AIPI_LITE_BOARD
  // AIPI I2S uses 32-bit slots; 16-bit audio is left-aligned (<< 16).
  output_buffer_32 =
      (opus_int32 *)malloc(SPK_BUFFER_SAMPLES * SPK_CHANNELS * sizeof(opus_int32));
#elif defined(WAVESHARE_AMOLED_1_8_BOARD) && WAVESHARE_AMOLED_1_8_BOARD
  // Mono speaker path through the BSP codec device (no L/R slots on the wire).
  output_buffer_mono =
      (opus_int16 *)malloc(SPK_BUFFER_SAMPLES * sizeof(opus_int16));
#endif
}

void oai_audio_decode(uint8_t *data, size_t size) {
  int decoded_size =
      opus_decode(opus_decoder, data, size, output_buffer, SPK_BUFFER_SAMPLES, 0);

  if (decoded_size > 0) {
    size_t bytes_written = 0;
    if (s_i2s_tx_chan != NULL) {
#if defined(AIPI_LITE_BOARD) && AIPI_LITE_BOARD
      // Left-align the 16-bit samples in the 32-bit slots (the same wire
      // format the codec ADC produces and the reference sketch uses).
      for (int i = 0; i < decoded_size * SPK_CHANNELS; ++i) {
        output_buffer_32[i] = ((opus_int32)output_buffer[i]) << 16;
      }
      i2s_channel_write(s_i2s_tx_chan,
                        output_buffer_32,
                        decoded_size * SPK_CHANNELS * sizeof(opus_int32),
                        &bytes_written,
                        portMAX_DELAY);
#else
      i2s_channel_write(s_i2s_tx_chan,
                        output_buffer,
                        decoded_size * SPK_CHANNELS * sizeof(opus_int16),
                        &bytes_written,
                        portMAX_DELAY);
#endif
    }
#if defined(WAVESHARE_AMOLED_2_06_BOARD) && WAVESHARE_AMOLED_2_06_BOARD
    if (s_spk_codec_dev != NULL) {
      // Stereo passthrough: the ES8311 DAC is opened at 16 kHz stereo, so
      // the decoded stereo frame goes straight to the codec device.
      esp_codec_dev_write(s_spk_codec_dev, output_buffer,
                          decoded_size * SPK_CHANNELS * sizeof(opus_int16));
    }
#elif defined(WAVESHARE_AMOLED_1_8_BOARD) && WAVESHARE_AMOLED_1_8_BOARD
    if (s_spk_codec_dev != NULL && output_buffer_mono != NULL) {
      // Mono speaker path: downmix the decoded stereo frame and push it
      // through the BSP codec device (opened at 16 kHz mono).
      for (int i = 0; i < decoded_size; ++i) {
        int32_t m = (((int32_t)output_buffer[i * 2] + (int32_t)output_buffer[i * 2 + 1]) >> 1) * SPK_GAIN;
        output_buffer_mono[i] = m > 32767 ? 32767 : (m < -32768 ? -32768 : (opus_int16)m);
      }
      esp_codec_dev_write(s_spk_codec_dev, output_buffer_mono,
                          decoded_size * sizeof(opus_int16));
    }
#endif
  }
}

OpusEncoder *opus_encoder = NULL;
opus_int16 *encoder_input_buffer = NULL;
opus_int16 *encoder_capture_buffer = NULL;
uint8_t *encoder_output_buffer = NULL;

void oai_init_audio_encoder() {
  int encoder_error;
  opus_encoder = opus_encoder_create(MIC_SAMPLE_RATE, MIC_CHANNELS, OPUS_APPLICATION_VOIP,
                                     &encoder_error);
  if (encoder_error != OPUS_OK) {
    ESP_LOGE("Media", "Failed to create OPUS encoder");
    return;
  }

  if (opus_encoder_init(opus_encoder, MIC_SAMPLE_RATE, MIC_CHANNELS, OPUS_APPLICATION_VOIP) !=
      OPUS_OK) {
    ESP_LOGE("Media", "Failed to initialize OPUS encoder");
    return;
  }

  opus_encoder_ctl(opus_encoder, OPUS_SET_BITRATE(OPUS_ENCODER_BITRATE));
  opus_encoder_ctl(opus_encoder, OPUS_SET_COMPLEXITY(OPUS_ENCODER_COMPLEXITY));
  opus_encoder_ctl(opus_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
  encoder_input_buffer =
      (opus_int16 *)malloc(MIC_BUFFER_SAMPLES * sizeof(opus_int16));
  encoder_capture_buffer =
      (opus_int16 *)malloc(MIC_BUFFER_SAMPLES * MIC_I2S_CHANNELS * MIC_BYTES_PER_SLOT);
  encoder_output_buffer = (uint8_t *)malloc(MIC_OPUS_OUT_BUFFER_SIZE);
  if (encoder_input_buffer == NULL || encoder_capture_buffer == NULL ||
      encoder_output_buffer == NULL) {
    ESP_LOGE("Media", "Failed to allocate mic encoder buffers");
    return;
  }
  ESP_LOGI("Media","Initialized OPUS encoder");
}

void oai_send_audio(PeerConnection *peer_connection) {
  // If interrupted, skip audio capture and encoding entirely
  if (oai_is_interrupted()) {
    return;
  }
  
  size_t bytes_read = 0;
  static int regulator = 0;

#if defined(WAVESHARE_BSP_BOARD) && WAVESHARE_BSP_BOARD
  if (s_mic_codec_dev == NULL) {
    return;
  }
  // BSP codec path: one blocking 20 ms frame (1 s internal timeout; returns
  // the full request on success). 1.8 = mono 16-bit; 2.06 = stereo 16-bit
  // (L/R interleaved) from the ES7210 dual mics.
  int codec_ret = esp_codec_dev_read(s_mic_codec_dev, encoder_capture_buffer,
#if defined(WAVESHARE_AMOLED_2_06_BOARD) && WAVESHARE_AMOLED_2_06_BOARD
                                     MIC_BUFFER_SAMPLES * 2 * sizeof(int16_t));
#else
                                     MIC_BUFFER_SAMPLES * sizeof(int16_t));
#endif
  if (codec_ret != ESP_CODEC_DEV_OK) {
    if (regulator % 100 == 0) {
      ESP_LOGW("Media", "esp_codec_dev_read failed: %d", codec_ret);
    }
    return;
  }
#if defined(WAVESHARE_AMOLED_2_06_BOARD) && WAVESHARE_AMOLED_2_06_BOARD
  bytes_read = MIC_BUFFER_SAMPLES * 2 * sizeof(int16_t);
#else
  bytes_read = MIC_BUFFER_SAMPLES * sizeof(int16_t);
#endif
#else
  if (s_i2s_rx_chan == NULL) {
    return;
  }

  i2s_channel_read(s_i2s_rx_chan,
                   encoder_capture_buffer,
                   MIC_BUFFER_SAMPLES * MIC_I2S_CHANNELS * MIC_BYTES_PER_SLOT,
                   &bytes_read,
                   portMAX_DELAY);
#endif

#if defined(WAVESHARE_AMOLED_2_06_BOARD) && WAVESHARE_AMOLED_2_06_BOARD
  int samples_read = (int)(bytes_read / (2 * sizeof(int16_t)));  // stereo frames
#elif defined(WAVESHARE_AMOLED_1_8_BOARD) && WAVESHARE_AMOLED_1_8_BOARD
  int samples_read = (int)(bytes_read / sizeof(int16_t));  // mono 16-bit
#else
  int samples_read = (int)(bytes_read / (MIC_I2S_CHANNELS * MIC_BYTES_PER_SLOT));
#endif
  if (samples_read <= 0) {
    return;
  }

#if defined(WAVESHARE_AMOLED_2_06_BOARD) && WAVESHARE_AMOLED_2_06_BOARD
  // ES7210 dual digital mics: 16-bit stereo PCM (L/R interleaved) at 16 kHz.
  // Average L+R (same as the Waveshare Spec_Analyzer example), then apply
  // the MIC_GAIN headroom like the other boards.
  int16_t* s16 = (int16_t*)(void*)encoder_capture_buffer;
  const char* ch_str = "L+R";
  int64_t left_energy = 0, right_energy = 0;
  for (int i = 0; i < samples_read; ++i) {
    int16_t l = s16[i * 2];
    int16_t r = s16[i * 2 + 1];
    left_energy  += (int64_t)l * l;
    right_energy += (int64_t)r * r;
  }
  for (int i = 0; i < samples_read; ++i) {
    int32_t s = (((int32_t)s16[i * 2] + (int32_t)s16[i * 2 + 1]) >> 1) * MIC_GAIN;
    encoder_input_buffer[i] = s > 32767 ? 32767 : (s < -32768 ? -32768 : (int16_t)s);
  }
#elif defined(WAVESHARE_AMOLED_1_8_BOARD) && WAVESHARE_AMOLED_1_8_BOARD
  // Plain 16-bit mono PCM straight from the codec (channel 0 of the shared
  // bus). Apply the same MIC_GAIN headroom as the other boards.
  int16_t* s16 = (int16_t*)(void*)encoder_capture_buffer;
  const char* ch_str = "M";
  int64_t left_energy = 0, right_energy = 0;
  for (int i = 0; i < samples_read; ++i) {
    left_energy += (int64_t)s16[i] * s16[i];
    int32_t s = (int32_t)s16[i] * MIC_GAIN;
    encoder_input_buffer[i] = s > 32767 ? 32767 : (s < -32768 ? -32768 : (int16_t)s);
  }
#elif defined(AIPI_LITE_BOARD) && AIPI_LITE_BOARD
  // 32-bit slots with audio in the upper 16 bits (ES8311 16-bit
  // left-aligned). Mirror the proven Arduino sketch: sum L+R (the codec
  // sends the mono mic on both slots), then scale to 16-bit with 12x gain.
  int32_t* s32 = (int32_t*)(void*)encoder_capture_buffer;
  const char* ch_str = "L+R";
  int64_t left_energy = 0, right_energy = 0;
  for (int i = 0; i < samples_read; ++i) {
    int16_t l = (int16_t)(s32[i * 2]     >> 16);
    int16_t r = (int16_t)(s32[i * 2 + 1] >> 16);
    left_energy  += (int64_t)l * l;
    right_energy += (int64_t)r * r;
  }
  for (int i = 0; i < samples_read; ++i) {
    int32_t s = ((int32_t)(s32[i * 2] >> 16) + (int32_t)(s32[i * 2 + 1] >> 16)) * 12;
    encoder_input_buffer[i] = s > 32767 ? 32767 : (s < -32768 ? -32768 : (int16_t)s);
  }
#else
  // Audio data is in the upper 16 bits of each 32-bit I2S slot (left-aligned per I2S standard)
  int32_t* stereo32 = (int32_t*)(void*)encoder_capture_buffer;
  int64_t left_energy = 0, right_energy = 0;
  for (int i = 0; i < samples_read; ++i) {
    int16_t l = (int16_t)((int32_t)stereo32[i * 2]     >> 16);
    int16_t r = (int16_t)((int32_t)stereo32[i * 2 + 1] >> 16);
    left_energy  += (int32_t)l * l;
    right_energy += (int32_t)r * r;
  }
  bool use_left = (left_energy >= right_energy);
  const char* ch_str = use_left ? "L" : "R";
  for (int i = 0; i < samples_read; ++i) {
    int32_t s = (int32_t)((int32_t)stereo32[i * 2 + (use_left ? 0 : 1)] >> 16) * MIC_GAIN;
    encoder_input_buffer[i] = s > 32767 ? 32767 : (s < -32768 ? -32768 : (int16_t)s);
  }
#endif

  regulator++;
  if (regulator % 100 == 0) {
    ESP_LOGI("Media", "Bytes read: %d, ch=%s, L=%lld R=%lld",
             bytes_read, ch_str,
             (long long)left_energy, (long long)right_energy);
    char sample_log[128] = {0};
    int n = snprintf(sample_log, sizeof(sample_log), "Mic samples: ");
    for (int i = 0; i < 8 && i < samples_read; ++i)
      n += snprintf(sample_log + n, sizeof(sample_log) - n, "%d ", encoder_input_buffer[i]);
    ESP_LOGI("Media", "%s", sample_log);

#if defined(WAVESHARE_AMOLED_2_06_BOARD) && WAVESHARE_AMOLED_2_06_BOARD
    if (left_energy == 0 && right_energy == 0) {
      ESP_LOGW("Media", "ZERO mic: all stereo samples are zero (ES7210 ADC not producing data)");
    }
#elif defined(WAVESHARE_AMOLED_1_8_BOARD) && WAVESHARE_AMOLED_1_8_BOARD
    if (left_energy == 0) {
      ESP_LOGW("Media", "ZERO mic: all 16-bit samples are zero (codec ADC not producing data)");
    }
#elif defined(AIPI_LITE_BOARD) && AIPI_LITE_BOARD
    if (left_energy == 0 && right_energy == 0) {
      // All-zero capture: the codec is not running (powered off / not
      // clocked), or DIN is not connected.
      int nz_words = 0;
      for (int i = 0; i < samples_read * MIC_I2S_CHANNELS; ++i) {
        if (s32[i] != 0) nz_words++;
      }
      ESP_LOGW("Media", "ZERO mic: nz_words=%d/%d, first=0x%08x 0x%08x 0x%08x 0x%08x",
               nz_words, samples_read * MIC_I2S_CHANNELS,
               (unsigned)s32[0], (unsigned)s32[1],
               (unsigned)s32[2], (unsigned)s32[3]);
    }
#else
    if (left_energy == 0 && right_energy == 0) {
      // All-zero capture. Distinguish a silent codec from a bit-position
      // mismatch: also check the lower 16 bits of each slot and count
      // non-zero 32-bit words in the raw buffer.
      int64_t l_low = 0, r_low = 0;
      int nz_words = 0;
      for (int i = 0; i < samples_read; ++i) {
        uint32_t wl = (uint32_t)stereo32[i * 2];
        uint32_t wr = (uint32_t)stereo32[i * 2 + 1];
        int16_t ll = (int16_t)(wl & 0xFFFF);
        int16_t rl = (int16_t)(wr & 0xFFFF);
        l_low += (int32_t)ll * ll;
        r_low += (int32_t)rl * rl;
        if (wl != 0 || wr != 0) nz_words++;
      }
      ESP_LOGW("Media", "ZERO mic: lower16 L=%lld R=%lld, nz_words=%d/%d, first=0x%08x 0x%08x 0x%08x 0x%08x",
               (long long)l_low, (long long)r_low, nz_words, samples_read,
               (unsigned)stereo32[0], (unsigned)stereo32[1],
               (unsigned)stereo32[2], (unsigned)stereo32[3]);
    }
#endif
  }

  auto encoded_size =
      opus_encode(opus_encoder, encoder_input_buffer, samples_read,
                  encoder_output_buffer, OPUS_OUT_BUFFER_SIZE);

  if (regulator % 100 == 0) {
    ESP_LOGI("Media", "Encoded size: %d", (int)encoded_size);
  }

  if (encoded_size <= 0) {
    if (regulator % 100 == 0) {
      ESP_LOGW("Media", "opus_encode failed: %d", (int)encoded_size);
    }
    return;
  }

  if (regulator % 100 == 0) {
    int opus_samples_48k = opus_packet_get_nb_samples(encoder_output_buffer, encoded_size, 48000);
    int opus_ms = opus_samples_48k > 0 ? (opus_samples_48k * 1000 / 48000) : -1;
    ESP_LOGI("Media", "Sending audio: encoded_size=%d, first 8 bytes: %02x %02x %02x %02x %02x %02x %02x %02x",
      (int)encoded_size,
      encoder_output_buffer[0], encoder_output_buffer[1], encoder_output_buffer[2], encoder_output_buffer[3],
      encoder_output_buffer[4], encoder_output_buffer[5], encoder_output_buffer[6], encoder_output_buffer[7]);
    ESP_LOGI("Media", "Opus packet duration (RTP/48k): samples=%d (~%d ms)", opus_samples_48k, opus_ms);
  }
  int send_ret = peer_connection_send_audio(peer_connection, encoder_output_buffer,
                                            encoded_size);
  if (send_ret < 0 && regulator % 100 == 0) {
    PeerConnectionState state = peer_connection_get_state(peer_connection);
    ESP_LOGW("Media", "peer_connection_send_audio failed: %d (state=%s)",
             send_ret, peer_connection_state_to_string(state));
  }

  vTaskDelay(pdMS_TO_TICKS(1));
}
