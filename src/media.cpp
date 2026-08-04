#include <driver/i2s.h>
#include <opus.h>
#include <esp_log.h>

#include "main.h"

#define SPK_SAMPLE_RATE 48000
#define SPK_BUFFER_SAMPLES 960  // 20ms at 48kHz
#define SPK_CHANNELS 2

#define MIC_OPUS_OUT_BUFFER_SIZE  2552// 1276  // 1276 bytes is recommended by opus_encode
#define MIC_SAMPLE_RATE 48000
#define MIC_BUFFER_SAMPLES 960  // 20ms at 48kHz
#define MIC_CHANNELS 2

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

void oai_init_audio_capture() {
  i2s_config_t i2s_config_out = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
      .sample_rate = SPK_SAMPLE_RATE,
      .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
      .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
      .communication_format = I2S_COMM_FORMAT_I2S_MSB,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = 8,
      .dma_buf_len = SPK_BUFFER_SAMPLES,
      .use_apll = 1,
      .tx_desc_auto_clear = true,
  };
  if (i2s_driver_install(I2S_NUM_0, &i2s_config_out, 0, NULL) != ESP_OK) {
    ESP_LOGE("Media", "Failed to configure I2S driver for audio output");
    return;
  }

  i2s_pin_config_t pin_config_out = {
      .mck_io_num = MCLK_PIN,
      .bck_io_num = DAC_BCLK_PIN,
      .ws_io_num = DAC_LRCLK_PIN,
      .data_out_num = DAC_DATA_PIN,
      .data_in_num = I2S_PIN_NO_CHANGE,
  };
  if (i2s_set_pin(I2S_NUM_0, &pin_config_out) != ESP_OK) {
    ESP_LOGE("Media", "Failed to set I2S pins for audio output");
    return;
  }
  i2s_zero_dma_buffer(I2S_NUM_0);

  i2s_config_t i2s_config_in = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
      .sample_rate = MIC_SAMPLE_RATE,
      .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
      .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
      .communication_format = I2S_COMM_FORMAT_I2S_MSB,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = 8,
      .dma_buf_len = MIC_BUFFER_SAMPLES,
      .use_apll = 1,
  };
  if (i2s_driver_install(I2S_NUM_1, &i2s_config_in, 0, NULL) != ESP_OK) {
    ESP_LOGE("Media", "Failed to configure I2S driver for audio input");
    return;
  }

  i2s_pin_config_t pin_config_in = {
      .mck_io_num = MCLK_PIN,
      .bck_io_num = ADC_BCLK_PIN,
      .ws_io_num = ADC_LRCLK_PIN,
      .data_out_num = I2S_PIN_NO_CHANGE,
      .data_in_num = ADC_DATA_PIN,
  };
  if (i2s_set_pin(I2S_NUM_1, &pin_config_in) != ESP_OK) {
    ESP_LOGE("Media", "Failed to set I2S pins for audio input");
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
    i2s_write(I2S_NUM_0,
              output_buffer,
              decoded_size * SPK_CHANNELS * sizeof(opus_int16),
              &bytes_written, portMAX_DELAY);
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

  opus_encoder_ctl(opus_encoder, OPUS_SET_BITRATE(64000));
  opus_encoder_ctl(opus_encoder, OPUS_SET_COMPLEXITY(OPUS_ENCODER_COMPLEXITY));
  opus_encoder_ctl(opus_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
  encoder_input_buffer =
      (opus_int16 *)malloc(MIC_BUFFER_SAMPLES * MIC_CHANNELS * sizeof(opus_int16));
  encoder_capture_buffer =
      (opus_int16 *)malloc(MIC_BUFFER_SAMPLES * MIC_CHANNELS * sizeof(opus_int16));
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

  i2s_read(I2S_NUM_1,
           encoder_capture_buffer,
           MIC_BUFFER_SAMPLES * MIC_CHANNELS * sizeof(opus_int16),
           &bytes_read,
           portMAX_DELAY);

  int samples_read = bytes_read / (MIC_CHANNELS * sizeof(opus_int16));
  if (samples_read <= 0) {
    return;
  }

  int64_t left_energy = 0;
  int64_t right_energy = 0;
  for (int i = 0; i < samples_read; ++i) {
    int16_t left = encoder_capture_buffer[i * 2];
    int16_t right = encoder_capture_buffer[i * 2 + 1];
    left_energy += (int32_t)left * left;
    right_energy += (int32_t)right * right;
  }

  bool use_left_channel = left_energy >= right_energy;
  for (int i = 0; i < samples_read; ++i) {
    int16_t sample = use_left_channel ? encoder_capture_buffer[i * 2]
                                      : encoder_capture_buffer[i * 2 + 1];
    encoder_input_buffer[i * 2] = sample;
    encoder_input_buffer[i * 2 + 1] = sample;
  }

  regulator++;
  if (regulator % 100 == 0) {
    ESP_LOGI("Media", "Bytes read from mic: %d, channel=%s, L=%lld R=%lld",
             bytes_read,
             use_left_channel ? "left" : "right",
             (long long)left_energy,
             (long long)right_energy);
    // Print first 8 samples for debugging
    char sample_log[128] = {0};
    int n = snprintf(sample_log, sizeof(sample_log), "Mic samples: ");
    for (int i = 0; i < 8 && i < MIC_BUFFER_SAMPLES * MIC_CHANNELS; ++i) {
      n += snprintf(sample_log + n, sizeof(sample_log) - n, "%d ", encoder_input_buffer[i]);
    }
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
