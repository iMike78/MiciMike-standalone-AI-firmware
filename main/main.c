/**
 * MiciMike AI Firmware - Main Application
 *
 * ESP-IDF based alternative firmware for MiciMike drop-in PCB.
 * Turns the device into a dedicated AI conversation partner
 * using OpenAI-compatible Realtime API.
 *
 * Architecture:
 *   XMOS XU316 (AEC + beamforming) â†’ I2S â†’ ESP32-S3 â†’ WebSocket â†’ Cloud LLM
 *                                                    â† WebSocket â† Cloud LLM
 *   ESP32-S3 â†’ I2S â†’ AIC3204 â†’ Speaker
 *
 * State machine:
 *   BOOT â†’ [provisioned?] â†’ IDLE (wake word listening)
 *                         â†’ SETUP (captive portal)
 *   IDLE â†’ WAKE_DETECTED â†’ SESSION_ACTIVE (streaming audio)
 *   SESSION_ACTIVE â†’ IDLE (after silence timeout)
 *
 * License: MIT (open source, community-maintained)
 */

#include "app_config.h"
#include "nvs_config.h"
#include "i2s_hal.h"
#include "aic3204.h"
#include "ws_client.h"
#include "wifi_provision.h"
#include "settings_server.h"
#include "led_control.h"
#include "touch_input.h"
#include "audio_resample.h"
#include "xmos_voice_kit.h"
#include "micro_wake_word.h"
#include "okay_nabu_model.h"
#include "hey_jarvis_model.h"
#include "hey_mycroft_model.h"
#include "alexa_model.h"
#include "stop_model.h"
#include "media_radio.h"
#include "sendspin_iface.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "mdns.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "main";

// ---------------------------------------------------------------------------
// Application state
// ---------------------------------------------------------------------------
typedef enum {
    APP_STATE_BOOT = 0,
    APP_STATE_SETUP,        // captive portal active
    APP_STATE_CONNECTING,   // connecting to WiFi
    APP_STATE_IDLE,         // listening for wake word
    APP_STATE_SESSION,      // active conversation session
} app_state_t;

static volatile app_state_t app_state = APP_STATE_BOOT;
static bool mww_available = false;
static micimike_config_t config;
static int64_t last_activity_ms = 0;
static int64_t session_started_ms = 0;
static volatile bool wake_session_pending = false;
static volatile bool session_error_cleanup_pending = false;
static volatile int64_t session_ready_ms = 0;
static volatile bool api_vad_seen = false;
static volatile uint32_t session_generation = 0;
static volatile bool speaker_amp_on = false;
static volatile bool end_session_after_response = false;
static volatile bool session_end_cleanup_pending = false;
static volatile int64_t mww_restart_not_before_ms = 0;
static volatile bool sendspin_radio_takeover_pending = false;
static TaskHandle_t sendspin_radio_takeover_task_handle = NULL;
static char active_wakeword[32] = DEFAULT_WAKEWORD;

// Audio playback queue (response audio from API → speaker).
// OpenAI's Realtime API ships output audio in bursts while the model
// is still encoding, so the queue must be deep enough that brief
// inter-burst pauses don't drain it below the 80 ms DMA buffer. 128
// chunks × 30 ms ≈ 3.8 s of headroom. on_ws_audio BLOCKS on xQueueSend
// when full — that propagates back to the WS socket as TCP recv
// backpressure and throttles the server to roughly realtime.
#define PLAYBACK_QUEUE_SIZE  128
#define PLAYBACK_CHUNK_SIZE  1440   // 30ms of 24kHz PCM16 mono = 720 samples
typedef struct {
    int16_t pcm[PLAYBACK_CHUNK_SIZE / sizeof(int16_t)];
    size_t samples;
} audio_chunk_t;

// Ring of recent mic chunks captured while the WS handshake is in flight.
// On WS_STATE_CONNECTED we flush MIC_PREBUFFER_FLUSH_MS worth into the
// stream so the wake word itself reaches the server. 240 ms covers
// "Okay Nabu" / "Hey Jarvis" without prepending a long silent lead that
// the Realtime API's server-side VAD would count as no_speech.
#define MIC_PREBUFFER_MS         600
#define MIC_PREBUFFER_FLUSH_MS   240
#define MIC_PREBUFFER_CHUNKS     (MIC_PREBUFFER_MS / AUDIO_BUF_SIZE_MS)
#define MIC_PREBUFFER_FLUSH_CHUNKS (MIC_PREBUFFER_FLUSH_MS / AUDIO_BUF_SIZE_MS)
#define MIC_PREBUFFER_MAX_SAMPLES (MIC_BUF_SAMPLES * 2)

static QueueHandle_t playback_queue = NULL;

static uint32_t session_idle_timeout_ms(void)
{
    uint16_t timeout_s = config.session_timeout_s;
    if (timeout_s < 3 || timeout_s > 300) {
        timeout_s = DEFAULT_SESSION_IDLE_TIMEOUT_S;
    }
    return (uint32_t)timeout_s * 1000U;
}

static void speaker_amp_enable(bool enable);
static void wake_session_task(void *arg);
static void session_end_cleanup_task(void *arg);
static void session_error_cleanup_task(void *arg);
static void on_settings_changed(uint32_t changed_mask);
static void on_stop_word_detected(const char *wake_word);
static void sendspin_apply_config(void);
static size_t sendspin_pcm_cb(const uint8_t *data, size_t length,
                              uint32_t timeout_ms, void *user);
static void sendspin_volume_cb(uint8_t volume, void *user);
static void sendspin_mute_cb(bool muted, void *user);
static void sendspin_radio_takeover_task(void *arg);
static void sendspin_request_radio_takeover(void);

static const uint8_t *wakeword_model_data(const char *wakeword, size_t *size)
{
    if (strcmp(wakeword, "hey_jarvis") == 0) {
        if (size) *size = hey_jarvis_model_data_len;
        return hey_jarvis_model_data;
    }
    if (strcmp(wakeword, "hey_mycroft") == 0) {
        if (size) *size = hey_mycroft_model_data_len;
        return hey_mycroft_model_data;
    }
    if (strcmp(wakeword, "alexa") == 0) {
        if (size) *size = alexa_model_data_len;
        return alexa_model_data;
    }
    if (size) *size = okay_nabu_model_data_len;
    return okay_nabu_model_data;
}

static float wakeword_probability_cutoff(const char *wakeword, const char *sensitivity)
{
    bool slight = strcmp(sensitivity, "slight") == 0;
    bool very = strcmp(sensitivity, "very") == 0;

    // ESPHome publishes 0.83 for hey_jarvis "very", but with the
    // MiciMike's PCB mic distance the NS-stage signal stays quiet
    // enough that the 5-frame averaged probability rarely crosses
    // that line — the user literally never wakes. We trade some
    // false-positives for actually-triggers-when-spoken at "very";
    // "moderate" and "slight" stay closer to the published values.
    if (strcmp(wakeword, "hey_jarvis") == 0) {
        return slight ? 0.92f : (very ? 0.60f : 0.78f);
    }
    if (strcmp(wakeword, "hey_mycroft") == 0) {
        return slight ? 0.97f : (very ? 0.72f : 0.86f);
    }
    return slight ? 0.85f : (very ? 0.56f : 0.69f);
}

static void on_settings_changed(uint32_t changed_mask)
{
    if (changed_mask & SETTINGS_CHANGED_WAKEWORD) {
        bool reboot_required = strcmp(config.wakeword, active_wakeword) != 0;
        settings_server_set_runtime_wakeword(active_wakeword, reboot_required);
        if (reboot_required) {
            ESP_LOGW(TAG,
                     "Wake word saved to NVS: %s, active model is still %s until restart",
                     config.wakeword, active_wakeword);
        }
    }

    if (changed_mask & SETTINGS_CHANGED_WW_SENS) {
        if (strcmp(config.wakeword, active_wakeword) == 0) {
            float cutoff = wakeword_probability_cutoff(active_wakeword,
                                                       config.wakeword_sensitivity);
            esp_err_t ret = mww_set_probability_cutoff(cutoff);
            ESP_LOGI(TAG, "Wake word sensitivity applied live: word=%s sens=%s cutoff=%.2f ret=%s",
                     active_wakeword, config.wakeword_sensitivity, cutoff,
                     esp_err_to_name(ret));
        } else {
            ESP_LOGW(TAG,
                     "Wake word sensitivity saved for pending model %s; active model %s unchanged until restart",
                     config.wakeword, active_wakeword);
        }
    }

    if (changed_mask & (SETTINGS_CHANGED_VOICE | SETTINGS_CHANGED_STYLE |
                        SETTINGS_CHANGED_PROMPT | SETTINGS_CHANGED_API)) {
        ESP_LOGI(TAG, "API/voice prompt settings saved; they apply on the next API session");
    }

    if (changed_mask & SETTINGS_CHANGED_SNAPCAST) {
        // The sendspin-cpp library destructor doesn't survive a live
        // start/stop cycle (tlsf_free assert in 0.6.1), so we defer
        // applying the change until the next boot.
        ESP_LOGI(TAG, "Sendspin settings saved; reboot required to apply");
    }
}

// Apply the current NVS-backed sendspin settings: stop a running player,
// then start a new one if enabled. The sendspin-cpp library brings its own
// WS server + mDNS advertisement; we only feed it identity + the PCM sink.
static void sendspin_apply_config(void)
{
    sendspin_iface_stop();

    if (!config.snapcast_enable) {
        ESP_LOGI(TAG, "Sendspin disabled — player not started");
        return;
    }

    sendspin_iface_config_t ss = {0};
    const char *cname = config.snapcast_name[0] ? config.snapcast_name : config.device_name;
    strncpy(ss.name, cname, sizeof(ss.name) - 1);

    // Prefix the MAC-derived id so Music Assistant treats this as a fresh
    // Sendspin player if an older cached MAC-only entry missed device_info.
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(ss.client_id, sizeof(ss.client_id),
             "mrd-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // Reuse the (formerly Snapcast) host NVS slot as an optional Sendspin
    // server URL fallback for environments where mDNS doesn't propagate.
    strncpy(ss.server_url, config.snapcast_host, sizeof(ss.server_url) - 1);
    ss.initial_volume = config.volume;

    ESP_LOGI(TAG, "Starting Sendspin player: name='%s' id=%s url='%s'",
             ss.name, ss.client_id, ss.server_url[0] ? ss.server_url : "(mDNS only)");
    sendspin_iface_set_pcm_cb(sendspin_pcm_cb, NULL);
    sendspin_iface_set_volume_cb(sendspin_volume_cb, NULL);
    sendspin_iface_set_mute_cb(sendspin_mute_cb, NULL);
    esp_err_t ret = sendspin_iface_start(&ss);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sendspin_iface_start failed: %s", esp_err_to_name(ret));
    }
}

static void sendspin_volume_cb(uint8_t volume, void *user)
{
    (void)user;
    if (volume > 100) volume = 100;

    config.volume = volume;
    esp_err_t ret = aic3204_set_volume(config.volume);
    if (ret == ESP_OK) {
        led_control_show_volume(config.volume);
        ESP_LOGI(TAG, "Sendspin server volume applied: %u%%",
                 (unsigned)config.volume);
    } else {
        ESP_LOGW(TAG, "Sendspin server volume failed: %s", esp_err_to_name(ret));
    }
}

static void sendspin_mute_cb(bool muted, void *user)
{
    (void)user;
    uint8_t effective_volume = muted ? 0 : config.volume;
    esp_err_t ret = aic3204_set_volume(effective_volume);
    if (ret == ESP_OK) {
        if (muted) {
            led_control_show_volume(0);
        } else {
            led_control_show_volume(config.volume);
        }
        ESP_LOGI(TAG, "Sendspin server mute applied: %s", muted ? "true" : "false");
    } else {
        ESP_LOGW(TAG, "Sendspin server mute failed: %s", esp_err_to_name(ret));
    }
}

static void sendspin_radio_takeover_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Sendspin takeover: stopping web radio");
    media_radio_stop();
    if (!media_radio_wait_stopped(1000)) {
        ESP_LOGW(TAG, "Radio task still running after Sendspin takeover grace period");
    }
    sendspin_radio_takeover_pending = false;
    sendspin_radio_takeover_task_handle = NULL;
    vTaskDelete(NULL);
}

static void sendspin_request_radio_takeover(void)
{
    if (sendspin_radio_takeover_pending) return;

    sendspin_radio_takeover_pending = true;
    BaseType_t ok = xTaskCreatePinnedToCore(sendspin_radio_takeover_task,
                                            "ss_takeover", 4096, NULL, 7,
                                            &sendspin_radio_takeover_task_handle, 0);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "Sendspin takeover task launch failed; stopping radio inline");
        media_radio_stop();
        media_radio_wait_stopped(250);
        sendspin_radio_takeover_pending = false;
        sendspin_radio_takeover_task_handle = NULL;
    }
}

// PCM sink for the sendspin player. The library hands us 16-bit signed
// interleaved stereo PCM at 48 kHz (matching what we declare in
// PlayerRoleConfig::audio_formats). The AIC3204 path is 32-bit interleaved
// stereo at the same rate, so we left-shift each sample by 16 bits and
// pass it through mm_i2s_write.
//
// Voice-barge-in mediation: while a voice session is live, the Sendspin
// stream stays silent and we consume its bytes so the WS does not backpressure.
// For media playback, Sendspin and internet radio are last-command-wins:
// if Sendspin audio arrives over radio, it stops the radio and takes over.
static size_t sendspin_pcm_cb(const uint8_t *data, size_t length,
                              uint32_t timeout_ms, void *user)
{
    (void)user;
    if (length == 0) return 0;

    if (app_state == APP_STATE_SESSION) {
        size_t frames = length / (sizeof(int16_t) * 2);
        uint32_t duration_ms = (uint32_t)((frames * 1000U) / 48000U);
        if (duration_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(duration_ms));
        }
        return length;
    }

    radio_state_t radio_state = media_radio_get_state();
    if (radio_state == RADIO_STATE_PLAYING || radio_state == RADIO_STATE_CONNECTING) {
        sendspin_request_radio_takeover();
        return length;
    }
    if (sendspin_radio_takeover_pending) {
        return length;
    }

    static int32_t *scratch = NULL;
    static size_t   scratch_cap = 0;

    size_t in_samples = length / sizeof(int16_t);
    size_t out_bytes  = in_samples * sizeof(int32_t);
    if (out_bytes > scratch_cap) {
        size_t new_cap = out_bytes * 2;
        int32_t *p = heap_caps_realloc(scratch, new_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p) {
            ESP_LOGW(TAG, "Sendspin PCM scratch realloc(%u) failed", (unsigned)new_cap);
            return 0;
        }
        scratch = p;
        scratch_cap = new_cap;
    }

    const int16_t *in = (const int16_t *)data;
    for (size_t i = 0; i < in_samples; i++) {
        scratch[i] = ((int32_t)in[i]) << 16;
    }

    if (!speaker_amp_on || gpio_get_level(PIN_SPEAKER_AMP) == 0) {
        speaker_amp_enable(true);
    }

    // The library passes AUDIO_WRITE_TIMEOUT_MS = 20 ms, but the AIC3204 I2S
    // path is XMOS-clocked at 48 kHz/32-bit/stereo = 192 kB/s — 20 ms only
    // fits ~3.8 kB while the sync task hands us up to ~32 kB chunks. Bump
    // the I2S timeout so the write completes in one go and the encoded
    // ring buffer can drain instead of overflowing.
    uint32_t i2s_timeout = timeout_ms < 1000 ? 1000 : timeout_ms;

    size_t written = 0;
    esp_err_t ret = mm_i2s_write(scratch, out_bytes, &written, i2s_timeout);

    if (ret != ESP_OK || written == 0) {
        return 0;
    }
    // Report bytes consumed from the input buffer (16-bit samples).
    return (written / sizeof(int32_t)) * sizeof(int16_t);
}

static void start_session(const char *reason)
{
    int64_t now = esp_timer_get_time() / 1000;
    ESP_LOGI(TAG, "Starting session (%s), idle_timeout=%us", reason, config.session_timeout_s);

    // Tear down any leftover WS handle BEFORE flipping app_state to SESSION,
    // so the DISCONNECTED event it generates doesn't run through the
    // session-end cleanup path (which would prematurely resume radio).
    ws_client_disconnect();

    app_state = APP_STATE_SESSION;
    session_generation++;
    session_started_ms = now;
    last_activity_ms = now;
    session_ready_ms = 0;
    api_vad_seen = false;
    end_session_after_response = false;
    led_control_set_state(LED_STATE_PROCESSING);

    // Pause any internet radio playback so the voice session has the speaker
    // to itself. media_radio_pause_for_session() remembers the URL; the
    // session-end path below resumes it.
    media_radio_pause_for_session();

    // Pre-warm the PA so the first audio chunk isn't clipped/lost when the
    // RESPONDING event finally fires. Don't play a chime here: the chime
    // is a tone burst on the AEC reference path and forces the XU316
    // adaptive filter to re-converge, which costs ~200 ms at the start of
    // every utterance and frequently eats the first syllable. The wake LED
    // is the user cue.
    speaker_amp_enable(true);
    audio_chunk_t dummy;
    while (xQueueReceive(playback_queue, &dummy, 0) == pdTRUE);

    ws_client_connect();
}

static void mic_audio_levels(const int32_t *mic_buf, size_t frames,
                             int32_t *avg_abs, int32_t *peak_abs)
{
    int64_t sum_abs = 0;
    int32_t peak = 0;

    for (size_t i = 0; i < frames; i++) {
        int32_t left = mic_buf[i * 2] >> 16;
        int32_t right = mic_buf[i * 2 + 1] >> 16;
        int32_t abs_left = left < 0 ? -left : left;
        int32_t abs_right = right < 0 ? -right : right;
        int32_t abs_sample = abs_left > abs_right ? abs_left : abs_right;
        sum_abs += abs_sample;
        if (abs_sample > peak) {
            peak = abs_sample;
        }
    }

    if (avg_abs) {
        *avg_abs = frames > 0 ? (int32_t)(sum_abs / frames) : 0;
    }
    if (peak_abs) {
        *peak_abs = peak;
    }
}

// ---------------------------------------------------------------------------
// Speaker amplifier control
// ---------------------------------------------------------------------------
static void speaker_amp_enable(bool enable)
{
    // INPUT_OUTPUT (not plain OUTPUT) so the settings UI's gpio_get_level()
    // read-back reflects the real pin level. Plain OUTPUT disables the input
    // sampler on S3 and the diagnostic LED stays red even when the amp is on.
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_SPEAKER_AMP),
        .mode = GPIO_MODE_INPUT_OUTPUT,
    };
    gpio_config(&io_conf);
    gpio_set_level(PIN_SPEAKER_AMP, enable ? 1 : 0);
    speaker_amp_on = enable;
}

// ---------------------------------------------------------------------------
// Mute switch
// ---------------------------------------------------------------------------
static bool is_muted(void)
{
    // GPIO1 mute is active-high: high = muted, low/GND = microphone enabled.
    return gpio_get_level(PIN_MUTE_SWITCH) == 1;
}

static int mute_gpio_level(void)
{
    return gpio_get_level(PIN_MUTE_SWITCH);
}

// ---------------------------------------------------------------------------
// WebSocket callbacks
// ---------------------------------------------------------------------------
static void on_ws_audio(const int16_t *pcm, size_t samples)
{
    // Queue response audio for the playback task. Block briefly when the
    // queue is full: the WS receive task pauses, the TCP recv window
    // closes, and the OpenAI server throttles itself to roughly the
    // 24 kHz playback rate. Without that backpressure the API bursts at
    // multiple-× realtime and we'd have to drop most of the response.
    // 250 ms is well below the WS ping interval, so we don't risk a
    // keepalive timeout, and long enough that the playback task can
    // drain ~8 chunks of queue.
    size_t offset = 0;
    size_t dropped = 0;
    while (offset < samples) {
        audio_chunk_t chunk;
        chunk.samples = samples - offset;
        if (chunk.samples > PLAYBACK_CHUNK_SIZE / sizeof(int16_t)) {
            chunk.samples = PLAYBACK_CHUNK_SIZE / sizeof(int16_t);
        }
        memcpy(chunk.pcm, &pcm[offset], chunk.samples * sizeof(int16_t));
        offset += chunk.samples;

        // 200 ms wait stays comfortably under esp_websocket_client's
        // 250 ms internal lock timeout (which printed
        // "Could not lock ws-client within 250 timeout" in the previous
        // run when we sat at the edge).
        if (xQueueSend(playback_queue, &chunk, pdMS_TO_TICKS(200)) != pdTRUE) {
            dropped++;
        }
    }
    if (dropped > 0) {
        ESP_LOGW(TAG, "Playback queue still full after backpressure, dropped %u new chunk(s)",
                 (unsigned)dropped);
    }
    last_activity_ms = esp_timer_get_time() / 1000;
}

static void on_ws_state(ws_state_t state)
{
    switch (state) {
    case WS_STATE_CONNECTED:
        led_control_set_state(LED_STATE_SESSION_ACTIVE);
        if (session_ready_ms == 0) {
            session_ready_ms = esp_timer_get_time() / 1000;
            api_vad_seen = false;
        }
        last_activity_ms = esp_timer_get_time() / 1000;
        ESP_LOGI(TAG, "Session ready, listening...");
        if (end_session_after_response && app_state == APP_STATE_SESSION) {
            ESP_LOGI(TAG, "Scheduling session end after deferred device command response");
            end_session_after_response = false;
            if (!session_end_cleanup_pending) {
                session_end_cleanup_pending = true;
                BaseType_t created = xTaskCreatePinnedToCore(session_end_cleanup_task,
                                                             "sess_end_clean",
                                                             4096,
                                                             NULL,
                                                             6,
                                                             NULL,
                                                             1);
                if (created != pdPASS) {
                    session_end_cleanup_pending = false;
                    ESP_LOGE(TAG, "Failed to create session end cleanup task");
                }
            }
        }
        break;
    case WS_STATE_LISTENING:
        led_control_set_state(LED_STATE_SESSION_ACTIVE);
        api_vad_seen = true;
        last_activity_ms = esp_timer_get_time() / 1000;
        // If we just entered LISTENING during an active response, the
        // server detected user speech and cancelled the response — drop
        // anything still queued so the cancelled tail doesn't play over
        // the user's interruption. (When LISTENING fires at the start
        // of a normal turn the queue is already empty, so it's a no-op.)
        if (playback_queue) {
            audio_chunk_t dropped;
            size_t flushed = 0;
            while (xQueueReceive(playback_queue, &dropped, 0) == pdTRUE) {
                flushed++;
            }
            if (flushed > 0) {
                ESP_LOGI(TAG, "Barge-in: flushed %u queued playback chunks",
                         (unsigned)flushed);
            }
        }
        break;
    case WS_STATE_RESPONDING:
        led_control_set_state(LED_STATE_SPEAKING);
        speaker_amp_enable(true);
        last_activity_ms = esp_timer_get_time() / 1000;
        break;
    case WS_STATE_TOOL_RUNNING:
        led_control_set_state(LED_STATE_PROCESSING);
        last_activity_ms = esp_timer_get_time() / 1000;
        break;
    case WS_STATE_DISCONNECTED:
        led_control_set_state(LED_STATE_IDLE);
        speaker_amp_enable(false);
        if (app_state == APP_STATE_SESSION) {
            ESP_LOGW(TAG, "Session ended by WebSocket disconnect");
            app_state = APP_STATE_IDLE;
            mww_restart_not_before_ms = (esp_timer_get_time() / 1000) + 2000;
            // Resume radio if it was playing when we took the speaker.
            media_radio_resume();
        }
        break;
    case WS_STATE_ERROR:
        led_control_set_state(LED_STATE_ERROR);
        if (app_state == APP_STATE_SESSION) {
            ESP_LOGW(TAG, "Session ended by WebSocket error");
            speaker_amp_enable(false);
            app_state = APP_STATE_IDLE;
            mww_restart_not_before_ms = (esp_timer_get_time() / 1000) + 2000;
        }
        if (!session_error_cleanup_pending) {
            session_error_cleanup_pending = true;
            BaseType_t created = xTaskCreatePinnedToCore(session_error_cleanup_task,
                                                         "sess_err_clean",
                                                         4096,
                                                         NULL,
                                                         6,
                                                         NULL,
                                                         1);
            if (created != pdPASS) {
                session_error_cleanup_pending = false;
                led_control_set_state(LED_STATE_IDLE);
            }
        }
        break;
    default:
        break;
    }
}

static esp_err_t on_device_control(const char *action, int value,
                                   char *result, size_t result_size)
{
    if (!action || !result || result_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strcmp(action, "radio_stop") == 0) {
        media_radio_stop();
        media_radio_clear_pending();
        snprintf(result, result_size, "Radio stopped.");
        return ESP_OK;
    }

    if (strcmp(action, "radio_play") == 0) {
        if (value != 1) {
            snprintf(result, result_size,
                     "Radio play rejected: explicit radio_play confirmation value is required.");
            return ESP_ERR_INVALID_ARG;
        }

        int idx = config.radio_current_index;
        if ((idx < 0 || idx >= (int)config.radio_station_count) &&
            config.radio_station_count > 0) {
            idx = 0;
            config.radio_current_index = 0;
            nvs_config_save(&config);
        }

        if (idx < 0 || idx >= (int)config.radio_station_count ||
            config.radio_stations[idx].url[0] == '\0') {
            snprintf(result, result_size, "No radio station is configured.");
            return ESP_ERR_NOT_FOUND;
        }

        media_radio_clear_pending();
        if (app_state == APP_STATE_SESSION) {
            media_radio_schedule_resume(config.radio_stations[idx].url);
            end_session_after_response = true;
            snprintf(result, result_size, "Radio will start after this reply: %s.",
                     config.radio_stations[idx].name[0]
                         ? config.radio_stations[idx].name
                         : "selected station");
            return ESP_OK;
        }

        esp_err_t ret = media_radio_start(config.radio_stations[idx].url);
        if (ret == ESP_OK) {
            snprintf(result, result_size, "Radio started: %s.",
                     config.radio_stations[idx].name[0]
                         ? config.radio_stations[idx].name
                         : "selected station");
        } else {
            snprintf(result, result_size, "Radio start failed: %s.",
                     media_radio_get_error());
        }
        return ret;
    }

    if (strcmp(action, "volume_set") == 0 || strcmp(action, "volume_delta") == 0) {
        int volume = config.volume;
        if (strcmp(action, "volume_set") == 0) {
            volume = value;
        } else {
            volume += value;
        }

        if (volume < 0) volume = 0;
        if (volume > 100) volume = 100;

        config.volume = (uint8_t)volume;
        esp_err_t ret = aic3204_set_volume(config.volume);
        if (ret == ESP_OK) {
            nvs_config_save(&config);
            led_control_show_volume(config.volume);
            snprintf(result, result_size, "Volume set to %u percent.",
                     (unsigned)config.volume);
        } else {
            snprintf(result, result_size, "Volume change failed: %s.",
                     esp_err_to_name(ret));
        }
        return ret;
    }

    snprintf(result, result_size, "Unsupported action: %s.", action);
    return ESP_ERR_INVALID_ARG;
}

// ---------------------------------------------------------------------------
// Playback task â€” dequeues audio chunks, resamples, sends to I2S
// ---------------------------------------------------------------------------
static void playback_task(void *arg)
{
    // Output buffer: 48 kHz stereo 32-bit, sized for one chunk after the
    // 2× upsample. chunk.samples ≤ 720 (24 kHz, 30 ms) → 1440 stereo
    // frames × 2 ch × 4 B = 11520 B. 16 KiB gives safe headroom.
    const size_t spk_buf_bytes = 16 * 1024;
    int32_t *spk_buf = heap_caps_malloc(spk_buf_bytes, MALLOC_CAP_SPIRAM);
    audio_chunk_t chunk;

    while (1) {
        // Wait for the next chunk. While the queue is empty the I2S TX
        // DMA outputs zeros automatically (auto_clear_after_cb), so we
        // don't need to write a manual silence frame — doing so would
        // just block this task for another 30 ms inside i2s_channel_write
        // and slow our response to real audio when it does arrive.
        if (xQueueReceive(playback_queue, &chunk, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }

        // Resample 24kHz/16bit/mono → 48kHz/32bit/stereo
        size_t out_frames = audio_resample_to_speaker(
            chunk.pcm, chunk.samples,
            spk_buf, spk_buf_bytes);

        size_t bytes_to_write = out_frames * 2 * sizeof(int32_t);
        size_t written = 0;
        esp_err_t ret = mm_i2s_write(spk_buf, bytes_to_write, &written, 100);
        if (ret != ESP_OK || written != bytes_to_write) {
            ESP_LOGW(TAG, "Playback I2S write failed: wrote=%u/%u ret=%s",
                     (unsigned)written, (unsigned)bytes_to_write,
                     esp_err_to_name(ret));
        }
    }
}

// ---------------------------------------------------------------------------
// Mic capture task â€” reads I2S, resamples, sends to WebSocket
// ---------------------------------------------------------------------------
static void mic_capture_task(void *arg)
{
    // Input buffer: 16kHz stereo 32bit
    const size_t read_size = MIC_BUF_SAMPLES * 2 * sizeof(int32_t);  // stereo
    int32_t *mic_buf = heap_caps_malloc(read_size, MALLOC_CAP_SPIRAM);

    // Output buffer: 24kHz mono 16bit (after resample)
    // 30ms at 24kHz = 720 samples, but resample ratio 3:2 means output is 1.5x input
    int16_t *api_buf = heap_caps_malloc(MIC_BUF_SAMPLES * 2 * sizeof(int16_t),
                                         MALLOC_CAP_SPIRAM);
    int16_t *prebuf = heap_caps_malloc(MIC_PREBUFFER_CHUNKS *
                                       MIC_PREBUFFER_MAX_SAMPLES *
                                       sizeof(int16_t),
                                       MALLOC_CAP_SPIRAM);
    uint16_t *prebuf_samples = heap_caps_malloc(MIC_PREBUFFER_CHUNKS *
                                                sizeof(uint16_t),
                                                MALLOC_CAP_SPIRAM);
    if (!mic_buf || !api_buf || !prebuf || !prebuf_samples) {
        ESP_LOGE(TAG, "Failed to allocate mic capture buffers");
        vTaskDelete(NULL);
        return;
    }

    size_t prebuf_head = 0;
    size_t prebuf_count = 0;
    uint32_t seen_generation = 0;
    bool prebuffer_flushed = false;
    uint32_t sent_chunks = 0;
    uint32_t sent_samples = 0;
    uint32_t send_errors = 0;
    uint32_t read_errors = 0;
    int32_t last_avg_abs = 0;
    int32_t last_peak_abs = 0;
    int64_t last_stats_ms = 0;

    while (1) {
        if (app_state != APP_STATE_SESSION) {
            prebuf_head = 0;
            prebuf_count = 0;
            prebuffer_flushed = false;
            seen_generation = session_generation;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        uint32_t current_generation = session_generation;
        if (current_generation != seen_generation) {
            prebuf_head = 0;
            prebuf_count = 0;
            prebuffer_flushed = false;
            seen_generation = current_generation;
        }

        if (is_muted()) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        size_t bytes_read;
        esp_err_t ret = mm_i2s_read(mic_buf, read_size, &bytes_read, 100);
        if (ret != ESP_OK || bytes_read == 0) {
            read_errors++;
            continue;
        }

        size_t in_frames = bytes_read / (2 * sizeof(int32_t));  // stereo frames
        mic_audio_levels(mic_buf, in_frames, &last_avg_abs, &last_peak_abs);
        int64_t level_now_ms = esp_timer_get_time() / 1000;
        ws_state_t ws_state = ws_client_get_state();

        if (last_avg_abs >= SESSION_LOCAL_SPEECH_AVG_THRESHOLD) {
            last_activity_ms = level_now_ms;
        }

        // Resample 16kHz/32bit/stereo â†’ 24kHz/16bit/mono
        size_t out_samples = audio_resample_to_api(
            mic_buf, in_frames,
            api_buf, MIC_BUF_SAMPLES * 2 * sizeof(int16_t));

        if (out_samples > 0) {
            if (out_samples > MIC_PREBUFFER_MAX_SAMPLES) {
                out_samples = MIC_PREBUFFER_MAX_SAMPLES;
            }

            memcpy(&prebuf[prebuf_head * MIC_PREBUFFER_MAX_SAMPLES],
                   api_buf,
                   out_samples * sizeof(int16_t));
            prebuf_samples[prebuf_head] = (uint16_t)out_samples;
            prebuf_head = (prebuf_head + 1) % MIC_PREBUFFER_CHUNKS;
            if (prebuf_count < MIC_PREBUFFER_CHUNKS) {
                prebuf_count++;
            }

            // Keep streaming the mic to the API even while it's actively
            // RESPONDING — that's what makes barge-in work. The audio we
            // forward (channel 0, AGC stage) is the fully echo-cancelled
            // XMOS output, so the assistant's own voice doesn't trigger
            // the server's semantic VAD. interrupt_response=true in the
            // session config makes the API cancel the current response
            // as soon as the VAD fires; we react locally by flushing
            // the playback queue in the LISTENING handler.
            bool can_send = ws_state == WS_STATE_CONNECTED ||
                            ws_state == WS_STATE_LISTENING ||
                            ws_state == WS_STATE_RESPONDING ||
                            ws_state == WS_STATE_TOOL_RUNNING;

            if (can_send && !prebuffer_flushed) {
                size_t flushed_chunks = 0;
                size_t flushed_samples = 0;
                size_t flush_count = prebuf_count > MIC_PREBUFFER_FLUSH_CHUNKS
                                   ? MIC_PREBUFFER_FLUSH_CHUNKS
                                   : prebuf_count;
                size_t start = (prebuf_head + MIC_PREBUFFER_CHUNKS - flush_count) %
                               MIC_PREBUFFER_CHUNKS;
                for (size_t i = 0; i < flush_count; i++) {
                    size_t idx = (start + i) % MIC_PREBUFFER_CHUNKS;
                    size_t samples = prebuf_samples[idx];
                    if (samples == 0) {
                        continue;
                    }
                    ret = ws_client_send_audio(&prebuf[idx * MIC_PREBUFFER_MAX_SAMPLES],
                                               samples);
                    if (ret == ESP_OK) {
                        sent_chunks++;
                        sent_samples += samples;
                        flushed_chunks++;
                        flushed_samples += samples;
                    } else {
                        send_errors++;
                        break;
                    }
                }
                if (flushed_chunks > 0) {
                    ESP_LOGI(TAG, "Mic prebuffer flushed: chunks=%u samples=%u",
                             (unsigned)flushed_chunks, (unsigned)flushed_samples);
                }
                prebuffer_flushed = true;
            } else if (can_send) {
                ret = ws_client_send_audio(api_buf, out_samples);
                if (ret == ESP_OK) {
                    sent_chunks++;
                    sent_samples += out_samples;
                } else {
                    send_errors++;
                }
            }
        }

        int64_t now = esp_timer_get_time() / 1000;
        if (now - last_stats_ms >= 5000) {
            ws_state_t ws_state = ws_client_get_state();
            ESP_LOGI(TAG,
                     "Mic->API stats: ws=%d chunks=%lu samples=%lu avg=%ld peak=%ld send_err=%lu read_err=%lu",
                     ws_state,
                     (unsigned long)sent_chunks,
                     (unsigned long)sent_samples,
                     (long)last_avg_abs,
                     (long)last_peak_abs,
                     (unsigned long)send_errors,
                     (unsigned long)read_errors);
            sent_chunks = 0;
            sent_samples = 0;
            send_errors = 0;
            read_errors = 0;
            last_stats_ms = now;
        }
    }
}

// ---------------------------------------------------------------------------
// Touch event handler
// ---------------------------------------------------------------------------
static void on_touch_event(touch_event_t event)
{
    switch (event) {
    case TOUCH_EVENT_VOL_UP:
        if (config.volume < 100) {
            config.volume += 5;
            if (config.volume > 100) config.volume = 100;
        }
        aic3204_set_volume(config.volume);
        nvs_config_save(&config);
        led_control_show_volume(config.volume);
        ESP_LOGI(TAG, "Volume up: %d%%", config.volume);
        // Return to previous LED state after 1.5s (handled by timeout in main loop)
        break;

    case TOUCH_EVENT_VOL_DOWN:
        if (config.volume >= 5) {
            config.volume -= 5;
        } else {
            config.volume = 0;
        }
        aic3204_set_volume(config.volume);
        nvs_config_save(&config);
        led_control_show_volume(config.volume);
        ESP_LOGI(TAG, "Volume down: %d%%", config.volume);
        break;

    case TOUCH_EVENT_CENTER_SHORT:
        if (app_state == APP_STATE_IDLE) {
            // Manual wake â€” skip wake word, start session immediately
            ESP_LOGI(TAG, "Manual wake (center tap)");
            start_session("manual");
        }
        break;

    case TOUCH_EVENT_CENTER_LONG:
        // Enter setup mode
        ESP_LOGI(TAG, "Entering setup mode (long press)");
        ws_client_disconnect();
        speaker_amp_enable(false);
        nvs_config_clear();
        esp_restart();  // reboot into provisioning
        break;

    case TOUCH_EVENT_CENTER_DOUBLE:
        if (app_state == APP_STATE_SESSION) {
            // Cancel current session
            ESP_LOGI(TAG, "Session cancelled (double tap)");
            ws_client_disconnect();
            speaker_amp_enable(false);
            app_state = APP_STATE_IDLE;
            led_control_set_state(LED_STATE_IDLE);
            // Clear playback queue
            audio_chunk_t dummy;
            while (xQueueReceive(playback_queue, &dummy, 0) == pdTRUE);
            media_radio_resume();
        }
        break;

    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Wake word detection callback
// ---------------------------------------------------------------------------
static void on_wake_word_detected(const char *wake_word)
{
    bool muted = is_muted();
    ESP_LOGI(TAG, "Wake callback: word=%s state=%d mute_gpio=%d muted=%d",
             wake_word, app_state, mute_gpio_level(), muted);

    if (app_state != APP_STATE_IDLE) {
        ESP_LOGW(TAG, "Wake ignored: app_state=%d", app_state);
        return;
    }

    if (muted) {
        ESP_LOGW(TAG, "Wake ignored: muted (gpio=%d)", mute_gpio_level());
        return;
    }

    ESP_LOGI(TAG, "Wake word '%s' detected! Starting session...", wake_word);
    int64_t now = esp_timer_get_time() / 1000;
    wake_session_pending = true;
    app_state = APP_STATE_SESSION;
    session_started_ms = now;
    last_activity_ms = now;

    BaseType_t created = xTaskCreatePinnedToCore(wake_session_task,
                                                 "wake_session",
                                                 8192,
                                                 (void *)wake_word,
                                                 9,
                                                 NULL,
                                                 1);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create wake_session task");
        wake_session_pending = false;
        app_state = APP_STATE_IDLE;
    }
}

static void on_stop_word_detected(const char *wake_word)
{
    ESP_LOGI(TAG, "Stop word '%s' detected: stopping local audio/session", wake_word);

    ws_client_disconnect();
    media_radio_stop();
    media_radio_clear_pending();
    speaker_amp_enable(false);
    end_session_after_response = false;
    wake_session_pending = false;

    if (playback_queue) {
        audio_chunk_t dummy;
        while (xQueueReceive(playback_queue, &dummy, 0) == pdTRUE);
    }

    app_state = APP_STATE_IDLE;
    last_activity_ms = esp_timer_get_time() / 1000;
    mww_restart_not_before_ms = last_activity_ms + 1000;
    led_control_set_state(LED_STATE_IDLE);
}

static void wake_session_task(void *arg)
{
    const char *wake_word = (const char *)arg;

    ESP_LOGI(TAG, "Wake session task: stopping MWW before API session");
    if (mww_available && mww_is_running()) {
        mww_stop();
    }

    start_session(wake_word ? wake_word : "wake_word");
    wake_session_pending = false;
    vTaskDelete(NULL);
}

static void session_end_cleanup_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    ws_client_disconnect();
    speaker_amp_enable(false);

    if (playback_queue) {
        audio_chunk_t dummy;
        while (xQueueReceive(playback_queue, &dummy, 0) == pdTRUE);
    }

    app_state = APP_STATE_IDLE;
    led_control_set_state(LED_STATE_IDLE);
    mww_restart_not_before_ms = (esp_timer_get_time() / 1000) + 2000;

    // Restore deferred radio playback only after the websocket task is out of
    // its own event callback. Calling ws_client_disconnect() inside that
    // callback can assert inside esp_websocket_client.
    media_radio_resume();

    session_end_cleanup_pending = false;
    vTaskDelete(NULL);
}

static void session_error_cleanup_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1200));
    ws_client_disconnect();
    speaker_amp_enable(false);

    if (app_state == APP_STATE_IDLE && !is_muted()) {
        led_control_set_state(LED_STATE_IDLE);
    }

    // Restore any pre-session radio playback; no-op if nothing was pending.
    media_radio_resume();

    session_error_cleanup_pending = false;
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Session timeout monitor + XMOS VNR heartbeat
// ---------------------------------------------------------------------------
// Reading the VNR register every few seconds is a cheap liveness check on
// the XU316: if it stops returning a sane (1..254) value the pipeline has
// stalled, even if the mic I2S looks fine on the ESP32 side.
#define VNR_LOG_INTERVAL_MS 5000

static void session_monitor_task(void *arg)
{
    int64_t last_vnr_log_ms = 0;

    while (1) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms - last_vnr_log_ms >= VNR_LOG_INTERVAL_MS) {
            uint8_t vnr = 0;
            if (xmos_voice_kit_read_vnr(&vnr) == ESP_OK) {
                ESP_LOGI(TAG, "XMOS VNR=%u state=%d", vnr, app_state);
            } else {
                ESP_LOGW(TAG, "XMOS VNR read failed");
            }
            last_vnr_log_ms = now_ms;
        }

        if (app_state == APP_STATE_SESSION && !wake_session_pending) {
            int64_t now = esp_timer_get_time() / 1000;
            int64_t session_ms = now - session_started_ms;
            ws_state_t ws_state = ws_client_get_state();

            // Idle timer only applies once the session is actually live.
            // During CONNECTING the WS handshake can legitimately take 5–20s
            // (TLS + OpenAI accept), and we must not abort it as "idle".
            bool session_live = (ws_state == WS_STATE_CONNECTED
                              || ws_state == WS_STATE_LISTENING
                              || ws_state == WS_STATE_RESPONDING
                              || ws_state == WS_STATE_TOOL_RUNNING);

            if (session_live) {
                if (ws_state == WS_STATE_TOOL_RUNNING) {
                    vTaskDelay(pdMS_TO_TICKS(500));
                    continue;
                }
                int64_t idle_ms = now - last_activity_ms;
                int64_t no_vad_ms = session_ready_ms > 0 ? now - session_ready_ms : 0;
                if (!api_vad_seen && session_ready_ms > 0 &&
                    no_vad_ms > session_idle_timeout_ms()) {
                    ESP_LOGI(TAG, "Session timeout (%lld ms without API VAD, limit=%u ms), closing",
                             no_vad_ms, (unsigned)session_idle_timeout_ms());
                    ws_client_disconnect();
                    speaker_amp_enable(false);
                    app_state = APP_STATE_IDLE;
                    led_control_set_state(LED_STATE_IDLE);
                    mww_restart_not_before_ms = (esp_timer_get_time() / 1000) + 2000;
                    media_radio_resume();
                    continue;
                }
                if (idle_ms > session_idle_timeout_ms()) {
                    ESP_LOGI(TAG, "Session timeout (%lld ms idle, limit=%u ms), closing",
                             idle_ms, (unsigned)session_idle_timeout_ms());
                    ws_client_disconnect();
                    speaker_amp_enable(false);
                    app_state = APP_STATE_IDLE;
                    led_control_set_state(LED_STATE_IDLE);
                    mww_restart_not_before_ms = (esp_timer_get_time() / 1000) + 2000;
                    media_radio_resume();
                }
            }

            if (session_ms > SESSION_MAX_DURATION_MS) {
                ESP_LOGW(TAG, "Session max duration reached, closing");
                ws_client_disconnect();
                speaker_amp_enable(false);
                app_state = APP_STATE_IDLE;
                led_control_set_state(LED_STATE_IDLE);
                mww_restart_not_before_ms = (esp_timer_get_time() / 1000) + 2000;
                media_radio_resume();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// ---------------------------------------------------------------------------
// Main entry point
// ---------------------------------------------------------------------------
void app_main(void)
{
    ESP_LOGI(TAG, "=== MiciMike AI Firmware ===");
    ESP_LOGI(TAG, "Build: %s %s", __DATE__, __TIME__);
    ESP_LOGI(TAG, "Reset reason: %d", esp_reset_reason());

    // Step 1: NVS init
    nvs_config_init();

    // Step 2: LED init (immediate visual feedback)
    led_control_init();
    led_control_set_state(LED_STATE_SETUP);  // orange while booting

    // Step 3: Check provisioning
    if (!nvs_config_is_provisioned()) {
        ESP_LOGI(TAG, "Not provisioned â†’ starting captive portal");
        app_state = APP_STATE_SETUP;
        wifi_provision_start();
        // wifi_provision_start blocks until config saved + reboot
        return;
    }

    // Step 4: Load config
    nvs_config_load(&config);
    ESP_LOGI(TAG, "Config loaded: wakeword=%s sensitivity=%s",
             config.wakeword, config.wakeword_sensitivity);

    // Step 5: Connect WiFi
    app_state = APP_STATE_CONNECTING;
    if (wifi_station_connect(&config) != ESP_OK) {
        ESP_LOGE(TAG, "WiFi connection failed!");
        led_control_set_state(LED_STATE_ERROR);
        vTaskDelay(pdMS_TO_TICKS(5000));
        // Fall back to setup mode
        nvs_config_clear();
        esp_restart();
        return;
    }

    // Step 5b: Sync clock via SNTP — TLS cert validity checks need it.
    // Without a real wall clock OpenAI's edge silently drops our WS upgrade.
    if (wifi_sntp_sync(30000) != ESP_OK) {
        ESP_LOGE(TAG, "SNTP sync failed — TLS handshake to OpenAI will be unreliable. "
                     "Check that UDP/123 is reachable from this network.");
    }

    // Step 5c: Initialise mDNS so Snapcast discovery + name advertising work.
    if (mdns_init() == ESP_OK) {
        char host[33];
        size_t pos = 0;
        for (size_t i = 0; config.device_name[i] && pos + 1 < sizeof(host); i++) {
            char c = config.device_name[i];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
                host[pos++] = c;
            } else if (pos > 0 && host[pos - 1] != '-') {
                host[pos++] = '-';
            }
        }
        while (pos > 0 && host[pos - 1] == '-') pos--;
        if (pos == 0) {
            strncpy(host, "micimike", sizeof(host) - 1);
            pos = strlen(host);
        }
        host[pos] = '\0';
        mdns_hostname_set(host);
        mdns_instance_name_set(config.device_name);
    } else {
        ESP_LOGW(TAG, "mDNS init failed — Snapcast auto-discovery will not work");
    }

    // Step 6: Start settings web server (accessible on local network)
    settings_server_start(&config, on_settings_changed);

    // Step 7: Init I2C codec driver, then bring up XMOS Voice Kit.
    // aic3204_init installs the shared I2C driver used by the XMOS control port.
    aic3204_init();

    // Pipeline-stage assignment mirrors ESPHome voice_kit defaults:
    //   ch0 = AGC  -> fully processed, sent to the Realtime API
    //   ch1 = NS   -> noise-suppressed, used by microWakeWord
    // The XU316 firmware (ffva v1.3.1) accepts these over I2C and routes
    // the corresponding pipeline taps to the I2S2 output that the ESP32
    // reads as the mic input.
    xmos_firmware_version_t xmos_version = {0};
    esp_err_t xmos_ret = xmos_voice_kit_setup(XMOS_PIPELINE_STAGE_AGC,
                                              XMOS_PIPELINE_STAGE_NS,
                                              &xmos_version);
    if (xmos_ret != ESP_OK) {
        ESP_LOGE(TAG, "XMOS Voice Kit setup failed: %s", esp_err_to_name(xmos_ret));
        led_control_set_state(LED_STATE_ERROR);
    }

    aic3204_set_volume(config.volume);
    led_control_set_volume(config.volume);
    aic3204_set_eq_profile(config.eq_profile);
    mm_i2s_init();

    // Step 8: Init WebSocket client
    ws_client_config_t ws_cfg = {
        .api_url = config.api_url,
        .api_key = config.api_key,
        .voice = config.realtime_voice,
        .conversation_style = config.conversation_style,
        .system_prompt = config.system_prompt,
        .audio_cb = on_ws_audio,
        .state_cb = on_ws_state,
        .device_control_cb = on_device_control,
    };
    ws_client_init(&ws_cfg);

    // Step 9: Init touch input
    //touch_input_init(on_touch_event);

    // Step 10: Init mute switch GPIO
    gpio_config_t mute_cfg = {
        .pin_bit_mask = (1ULL << PIN_MUTE_SWITCH),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&mute_cfg);
    led_control_set_muted(is_muted());

    // Step 11: Create audio pipeline tasks
    playback_queue = xQueueCreateWithCaps(PLAYBACK_QUEUE_SIZE, sizeof(audio_chunk_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!playback_queue) {
        ESP_LOGE(TAG, "Failed to create playback queue");
        led_control_set_state(LED_STATE_ERROR);
        return;
    }

    xTaskCreatePinnedToCore(mic_capture_task, "mic_cap", 8192, NULL, 10, NULL, 0);
    xTaskCreatePinnedToCore(playback_task, "playback", 6144, NULL, 12, NULL, 1);
    xTaskCreatePinnedToCore(session_monitor_task, "sess_mon", 2048, NULL, 5, NULL, 1);

    // Step 12: Initialize wake word detection
    size_t wakeword_model_size = 0;
    const uint8_t *wakeword_model = wakeword_model_data(config.wakeword,
                                                        &wakeword_model_size);
    float wakeword_cutoff = wakeword_probability_cutoff(config.wakeword,
                                                        config.wakeword_sensitivity);
    mww_config_t mww_cfg = {
        .wake_word_label = config.wakeword,
        .model_data = wakeword_model,
        .model_size = wakeword_model_size,
        .probability_cutoff = wakeword_cutoff,
        .stop_model_data = NULL,
        .stop_model_size = 0,
        .stop_probability_cutoff = 0.0f,
        .on_stop_detected = NULL,
        .sliding_window_size = 5,
        .tensor_arena_size = 0,
        // ESPHome HA Voice PE uses gain_factor=4 with the NS-stage XMOS
        // output, but the MiciMike PCB's MEMS path is measurably quieter
        // than the HA Voice PE — the previous log showed NS peaks of
        // ~75-130 in silence (i.e. raw NS peak ~20-30 before the 4×
        // multiplier). Bumping to 8 lifts that to ~150-260 and brings
        // real speech well above the model's int8 noise floor without
        // ever approaching clipping in normal use.
        .gain_factor = 8,
        .input_channel = MWW_INPUT_CHANNEL,
        .on_detected = on_wake_word_detected,
    };
    if (mww_init(&mww_cfg) == ESP_OK) {
        mww_available = true;
        strncpy(active_wakeword, config.wakeword, sizeof(active_wakeword) - 1);
        active_wakeword[sizeof(active_wakeword) - 1] = '\0';
        settings_server_set_runtime_wakeword(active_wakeword, false);
        mww_start();
        ESP_LOGI(TAG, "Wake word detection active: %s cutoff=%.2f",
                 config.wakeword, wakeword_cutoff);
    } else {
        ESP_LOGW(TAG, "Wake word init failed â€” manual wake only (center button still works)");
    }

    // Step 12b: Start the Sendspin player if the user enabled it. The
    // library spawns its own WS server and mDNS advertisement; PCM
    // routing to I2S is wired up via sendspin_iface_set_pcm_cb later.
    sendspin_apply_config();

    // Step 13: Ready!
    app_state = APP_STATE_IDLE;
    led_control_set_state(LED_STATE_IDLE);
    speaker_amp_enable(false);
    ESP_LOGI(TAG, "System ready. Say '%s' or use center button to wake.", config.wakeword);

    // Main loop â€” session state management
    while (1) {
        led_control_set_volume(config.volume);
        led_control_set_muted(is_muted());

        // When session ends (disconnected by timeout), restart MWW
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (mww_available && app_state == APP_STATE_IDLE && !mww_is_running() &&
            !is_muted() && now_ms >= mww_restart_not_before_ms) {
            mww_start();
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
