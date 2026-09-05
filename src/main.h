#include <peer.h>

#define LOG_TAG "realtimeapi-sdk"
#define MAX_HTTP_OUTPUT_BUFFER 2048

#ifdef __cplusplus
extern "C" {
#endif

// Voice Assistant app entry. Never returns. Used by the standalone
// firmware and by watch-os after a start-menu selection.
void voice_assistant_run(void);

#ifdef __cplusplus
}
#endif

void oai_wifi(void);
void oai_init_audio_capture(void);
void oai_init_audio_decoder(void);
void oai_init_audio_encoder();
// Apply speaker volume (0..100) and digital mic gain (1..16) immediately.
// Safe to call before or after codec init; board-specific writes no-op until ready.
void oai_apply_audio_settings(int speaker_vol, int mic_gain);
void oai_apply_audio_settings_from_nvs(void);
void oai_send_audio(PeerConnection *peer_connection);
void oai_audio_decode(uint8_t *data, size_t size);
void oai_webrtc();
void oai_http_request(char *offer, char *answer);

// Interrupt conversation functionality
void oai_init_interrupt_button(void);
bool oai_is_interrupted(void);
void oai_set_interrupted(bool interrupted);
void oai_stop_audio_playback(void);
void oai_resume_audio_playback(void);
void oai_send_interrupt(void);
void oai_start_silence_pump(void);  // Freenove/AIPI: silence the TX DMA while interrupted
// True once ICE+DTLS+SCTP have finished and the mic uplink task is running.
bool oai_is_voice_ready(void);
