#include <peer.h>

#define LOG_TAG "realtimeapi-sdk"
#define MAX_HTTP_OUTPUT_BUFFER 2048

void oai_wifi(void);
void oai_init_audio_capture(void);
void oai_init_audio_decoder(void);
void oai_init_audio_encoder();
void oai_send_audio(PeerConnection *peer_connection);
void oai_audio_decode(uint8_t *data, size_t size);
void oai_webrtc();
void oai_http_request(char *offer, char *answer);

// Interrupt conversation functionality
void oai_init_interrupt_button(void);
bool oai_is_interrupted(void);
void oai_set_interrupted(bool interrupted);
void oai_stop_audio_playback(void);
