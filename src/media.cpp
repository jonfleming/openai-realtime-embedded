#include <driver/i2s_std.h>
#include <opus.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "main.h"

#define SPK_SAMPLE_RATE 48000
#define SPK_BUFFER_SAMPLES 960  // 20ms at 48kHz
#define SPK_CHANNELS 2

#define MIC_OPUS_OUT_BUFFER_SIZE 1276
#define MIC_SAMPLE_RATE 16000
#define MIC_BUFFER_SAMPLES 320  // 20ms at 16kHz per channel
#define MIC_I2S_CHANNELS 2     // stereo capture avoids ESP32-S3 mono DMA padding artifact
#define MIC_CHANNELS 1         // Opus mono encode

// speaker
#define MCLK_PIN -1
#define DAC_BCLK_PIN 42   // BCLK
#define DAC_LRCLK_PIN 41  // LRCLK
#define DAC_DATA_PIN 1    // SDATA

// microphone
#define ADC_BCLK_PIN 3    // SCK -- BCLK
#define ADC_LRCLK_PIN 14  // WS  -- LRCLK
#define ADC_DATA_PIN 46   // SD  -- DATA
#define OPUS_OUT_BUFFER_SIZE 1276  // 1276 bytes is recommended by opus_encode

#define OPUS_ENCODER_BITRATE 32000
#define OPUS_ENCODER_COMPLEXITY 2
#define MIC_GAIN 4  // linear gain applied before encode; 4 = +12 dB; increase if VAD still misses speech

static i2s_chan_handle_t s_i2s_tx_chan = NULL;
static i2s_chan_handle_t s_i2s_rx_chan = NULL;

void oai_init_audio_capture() {
  i2s_chan_config_t tx_chan_cfg =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
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
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  if (i2s_new_channel(&rx_chan_cfg, NULL, &s_i2s_rx_chan) != ESP_OK) {
    ESP_LOGE("Media", "Failed to create I2S RX channel");
    return;
  }

  i2s_std_config_t rx_std_cfg = {
      // 32-bit slot gives BCLK=1.024 MHz at 16 kHz (mics need ≥1 MHz)
      .clk_cfg = {
          .sample_rate_hz = MIC_SAMPLE_RATE,
          .clk_src = I2S_CLK_SRC_DEFAULT,
          .mclk_multiple = I2S_MCLK_MULTIPLE_256,
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

  if (i2s_channel_enable(s_i2s_tx_chan) != ESP_OK) {
    ESP_LOGE("Media", "Failed to enable I2S TX channel");
    return;
  }

  if (i2s_channel_enable(s_i2s_rx_chan) != ESP_OK) {
    ESP_LOGE("Media", "Failed to enable I2S RX channel");
    return;
  }

  ESP_LOGI("Media","OAI Audio Capture Initialized");
}

opus_int16 *output_buffer = NULL;
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
}

void oai_audio_decode(uint8_t *data, size_t size) {
  int decoded_size =
      opus_decode(opus_decoder, data, size, output_buffer, SPK_BUFFER_SAMPLES, 0);

  if (decoded_size > 0) {
    size_t bytes_written = 0;
    if (s_i2s_tx_chan != NULL) {
      i2s_channel_write(s_i2s_tx_chan,
                        output_buffer,
                        decoded_size * SPK_CHANNELS * sizeof(opus_int16),
                        &bytes_written,
                        portMAX_DELAY);
    }
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

  opus_encoder_ctl(opus_encoder, OPUS_SET_BITRATE(32000));
  opus_encoder_ctl(opus_encoder, OPUS_SET_COMPLEXITY(OPUS_ENCODER_COMPLEXITY));
  opus_encoder_ctl(opus_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
  encoder_input_buffer =
      (opus_int16 *)malloc(MIC_BUFFER_SAMPLES * sizeof(opus_int16));
  encoder_capture_buffer =
      (opus_int16 *)malloc(MIC_BUFFER_SAMPLES * MIC_I2S_CHANNELS * 4);  // 32-bit per slot = 4 bytes each
  encoder_output_buffer = (uint8_t *)malloc(MIC_OPUS_OUT_BUFFER_SIZE);
  if (encoder_input_buffer == NULL || encoder_capture_buffer == NULL ||
      encoder_output_buffer == NULL) {
    ESP_LOGE("Media", "Failed to allocate mic encoder buffers");
    return;
  }
  ESP_LOGI("Media","Initialized OPUS encoder");
}

void oai_send_audio(PeerConnection *peer_connection) {
  size_t bytes_read = 0;
  static int regulator = 0;

  if (s_i2s_rx_chan == NULL) {
    return;
  }

  i2s_channel_read(s_i2s_rx_chan,
                   encoder_capture_buffer,
                   MIC_BUFFER_SAMPLES * MIC_I2S_CHANNELS * 4,
                   &bytes_read,
                   portMAX_DELAY);

  int samples_read = (int)(bytes_read / (MIC_I2S_CHANNELS * 4));
  if (samples_read <= 0) {
    return;
  }

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
  for (int i = 0; i < samples_read; ++i) {
    int32_t s = (int32_t)((int32_t)stereo32[i * 2 + (use_left ? 0 : 1)] >> 16) * MIC_GAIN;
    encoder_input_buffer[i] = s > 32767 ? 32767 : (s < -32768 ? -32768 : (int16_t)s);
  }

  regulator++;
  if (regulator % 100 == 0) {
    ESP_LOGI("Media", "Bytes read: %d, ch=%s, L=%lld R=%lld",
             bytes_read, use_left ? "L" : "R",
             (long long)left_energy, (long long)right_energy);
    char sample_log[128] = {0};
    int n = snprintf(sample_log, sizeof(sample_log), "Mic samples: ");
    for (int i = 0; i < 8 && i < samples_read; ++i)
      n += snprintf(sample_log + n, sizeof(sample_log) - n, "%d ", encoder_input_buffer[i]);
    ESP_LOGI("Media", "%s", sample_log);
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
    ESP_LOGI("Media", "Sending audio: encoded_size=%d, first 8 bytes: %02x %02x %02x %02x %02x %02x %02x %02x",
      (int)encoded_size,
      encoder_output_buffer[0], encoder_output_buffer[1], encoder_output_buffer[2], encoder_output_buffer[3],
      encoder_output_buffer[4], encoder_output_buffer[5], encoder_output_buffer[6], encoder_output_buffer[7]);
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
