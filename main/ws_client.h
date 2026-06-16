/**
 * OpenAI Realtime API WebSocket Client
 *
 * Handles the WebSocket connection to OpenAI (or compatible) Realtime API.
 * Protocol: send PCM16/24kHz/mono audio as base64, receive audio stream back.
 *
 * Session lifecycle:
 *   1. ws_client_connect() — opens WebSocket, sends session.create
 *   2. ws_client_send_audio() — streams mic audio as input_audio_buffer.append
 *   3. Receives response.audio.delta events → decoded and queued for playback
 *   4. ws_client_disconnect() — closes session after silence timeout
 */

#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef enum {
    WS_STATE_DISCONNECTED = 0,
    WS_STATE_CONNECTING,
    WS_STATE_CONNECTED,
    WS_STATE_LISTENING,      // server VAD active, user speaking
    WS_STATE_RESPONDING,     // AI generating/streaming response
    WS_STATE_TOOL_RUNNING,   // local function tool is running
    WS_STATE_CLOSING,
    WS_STATE_ERROR,
} ws_state_t;

typedef void (*ws_audio_cb_t)(const int16_t *pcm, size_t samples);
typedef void (*ws_state_cb_t)(ws_state_t state);
typedef esp_err_t (*ws_device_control_cb_t)(const char *action, int value,
                                            char *result, size_t result_size);

typedef struct {
    const char *api_url;     // WebSocket URL
    const char *api_key;     // Bearer token
    const char *voice;       // Realtime output voice
    const char *conversation_style;
    const char *system_prompt;
    ws_audio_cb_t audio_cb;  // called when response audio arrives (24kHz/16bit/mono)
    ws_state_cb_t state_cb;  // called on state changes
    ws_device_control_cb_t device_control_cb;  // called for Realtime function tools
} ws_client_config_t;

typedef struct {
    bool has_usage;
    uint32_t response_count;
    uint32_t input_tokens;
    uint32_t output_tokens;
    uint32_t total_tokens;
    char raw_json[384];
} ws_usage_info_t;

esp_err_t ws_client_init(const ws_client_config_t *config);
esp_err_t ws_client_connect(void);
esp_err_t ws_client_disconnect(void);
esp_err_t ws_client_send_audio(const int16_t *pcm, size_t samples);
ws_state_t ws_client_get_state(void);
void ws_client_get_last_usage(ws_usage_info_t *out);

/**
 * Copy the assistant's most recent response transcript into `out`.
 * Updated live as the Realtime API streams transcript deltas alongside
 * the audio; reset to empty when a new response starts. Always
 * NUL-terminated. Pass at least 2048 bytes to capture a full turn.
 */
void ws_client_get_last_response_text(char *out, size_t out_size);
