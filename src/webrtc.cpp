#ifndef LINUX_BUILD
#include <opus.h>
#endif

#include <esp_event.h>
#include <esp_log.h>
#include <string.h>
#include <cJSON.h>

#include "main.h"

#ifndef LINUX_BUILD
#include "esp_lcd_panel_io.h"
#include "lcd.h"
#include "esp_lvgl_port.h"
#endif

#define TICK_INTERVAL 5

// TURN relay server for WebRTC/ICE media. Compile-time definitions come from
// CMakeLists.txt (TURN_USERNAME/TURN_PASSWORD are read from privateConfig.json
// when present). Signaling still goes to OPENAI_REALTIMEAPI; only the media
// path (ICE checks, DTLS, RTP, SCTP) is relayed through the TURN server.
#ifndef TURN_SERVER_URL
#define TURN_SERVER_URL "turn:turn.fleming.ai:3478"
#endif
#ifndef TURN_USERNAME
#define TURN_USERNAME ""
#endif
#ifndef TURN_PASSWORD
#define TURN_PASSWORD ""
#endif
#define GREETING                                                    \
  "{\"type\": \"response.create\", \"response\": {\"modalities\": " \
  "[\"audio\", \"text\"], \"instructions\": \"Say 'How can I help?.'\"}}"

#define SESSION_UPDATE                                              \
  "{\"type\": \"session.update\", \"session\": {"              \
  "\"type\": \"realtime\", \"audio\": {"                     \
  "\"input\": {\"turn_detection\": {\"type\": \"server_vad\", " \
  "\"threshold\": 0.3, "                                      \
  "\"interrupt_response\": true}}}}}"

PeerConnection *peer_connection = NULL;

void parse_response(const char* json_str) {
  cJSON *root = cJSON_Parse(json_str);
  if (root == NULL) {
      // printf("JSON parse failed\n");
      return;
  }
  
  cJSON *transcript = cJSON_GetObjectItem(root, "transcript");
  if (transcript != NULL && cJSON_IsString(transcript)) {
      printf("msg: %s\n", transcript->valuestring);
#ifndef LINUX_BUILD
      char buf[1000];
      lv_snprintf(buf, sizeof(buf), "msg: %s", transcript->valuestring);
      lvgl_ui_label_set_text(buf);
#endif
  }
  
  cJSON_Delete(root);
}
#ifndef LINUX_BUILD
StaticTask_t task_buffer;
static bool audio_task_started = false;
void oai_send_audio_task(void *user_data) {
  oai_init_audio_encoder();

  // STATE_CHANGED updates pc->state after invoking the callback, so wait here
  // to avoid racing the transition on task startup.
  while (peer_connection_get_state(peer_connection) != PEER_CONNECTION_COMPLETED) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  while (1) {
    // Check if interrupted - skip sending audio if so
    if (oai_is_interrupted()) {
      vTaskDelay(pdMS_TO_TICKS(50)); // Poll every 50ms when interrupted
      continue;
    }
    oai_send_audio(peer_connection);
  }
}
#endif

static void oai_ondatachannel_onmessage_task(char *msg, size_t len,
                                             void *userdata, uint16_t sid) {
  ESP_LOGI(LOG_TAG, "Received datachannel message: %s", msg);
  parse_response(msg);
}

static void oai_ondatachannel_onopen_task(void *userdata) {
  // SCTP is now associated; create the named channel so the server sees DATA_CHANNEL_OPEN.
  if (peer_connection_create_datachannel(peer_connection, DATA_CHANNEL_RELIABLE,
                                         0, 0, (char *)"oai-events",
                                         (char *)"") == -1) {
    ESP_LOGE(LOG_TAG, "Failed to create DataChannel");
    return;
  }
  ESP_LOGI(LOG_TAG, "DataChannel created");

  int session_ret = peer_connection_datachannel_send(peer_connection,
                                                     (char *)SESSION_UPDATE,
                                                     strlen(SESSION_UPDATE));
  ESP_LOGI(LOG_TAG, "SESSION_UPDATE %s (%d bytes)", session_ret >= 0 ? "sent" : "failed", session_ret);

  int greeting_ret = peer_connection_datachannel_send(peer_connection,
                                                      (char *)GREETING,
                                                      strlen(GREETING));
  ESP_LOGI(LOG_TAG, "GREETING %s (%d bytes)", greeting_ret >= 0 ? "sent" : "failed", greeting_ret);
}

static void oai_onconnectionstatechange_task(PeerConnectionState state,
                                             void *user_data) {
  ESP_LOGI(LOG_TAG, "PeerConnectionState: %s",
           peer_connection_state_to_string(state));

  if (state == PEER_CONNECTION_DISCONNECTED ||
      state == PEER_CONNECTION_CLOSED) {
#ifndef LINUX_BUILD
    esp_restart();
#endif
  } else if (state == PEER_CONNECTION_COMPLETED) {
#ifndef LINUX_BUILD
    if (!audio_task_started) {
      audio_task_started = true;
      StackType_t *stack_memory = (StackType_t *)heap_caps_malloc(
        40000 * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
      xTaskCreateStaticPinnedToCore(oai_send_audio_task, "audio_publisher", 40000,
                                    NULL, 7, stack_memory, &task_buffer, 1);
    }
#endif
  }
}

static void oai_on_icecandidate_task(char *description, void *user_data) {
  char local_buffer[MAX_HTTP_OUTPUT_BUFFER + 1] = {0};
  oai_http_request(description, local_buffer);
  if (strlen(local_buffer) > 0) {
    peer_connection_set_remote_description(peer_connection, local_buffer);
  }
}

void oai_webrtc() {
  PeerConfiguration peer_connection_config = {
      .ice_servers = {},
      .audio_codec = CODEC_OPUS,
      .video_codec = CODEC_NONE,
      .datachannel = DATA_CHANNEL_STRING,
      .onaudiotrack = [](uint8_t *data, size_t size, void *userdata) -> void {
#ifndef LINUX_BUILD
        oai_audio_decode(data, size);
#endif
      },
      .onvideotrack = NULL,
      .on_request_keyframe = NULL,
      .user_data = NULL,
  };

  // libpeer needs a username/password for a turn: server (long-term
  // credentials). Without credentials the allocation is skipped gracefully and
  // only host/STUN candidates are used.
  if (TURN_USERNAME[0] != '\0') {
    peer_connection_config.ice_servers[0] = (IceServer){
        .urls = TURN_SERVER_URL,
        .username = TURN_USERNAME,
        .credential = TURN_PASSWORD,
    };
    ESP_LOGI(LOG_TAG, "ICE will use TURN server %s", TURN_SERVER_URL);
  }

  peer_connection = peer_connection_create(&peer_connection_config);
  if (peer_connection == NULL) {
    ESP_LOGE(LOG_TAG, "Failed to create peer connection");
#ifndef LINUX_BUILD
    esp_restart();
#endif
  }

  peer_connection_oniceconnectionstatechange(peer_connection,
                                             oai_onconnectionstatechange_task);
  peer_connection_onicecandidate(peer_connection, oai_on_icecandidate_task);
  peer_connection_ondatachannel(peer_connection,
                                oai_ondatachannel_onmessage_task,
                                oai_ondatachannel_onopen_task, NULL);

  peer_connection_create_offer(peer_connection);

  while (1) {
    peer_connection_loop(peer_connection);
    vTaskDelay(pdMS_TO_TICKS(TICK_INTERVAL));
  }
}
