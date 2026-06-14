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
#include "media_radio.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"

#include <math.h>
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
static volatile int64_t mww_restart_not_before_ms = 0;
static char active_wakeword[32] = DEFAULT_WAKEWORD;

// Audio playback queue (response audio from API â†’ speaker)
#define PLAYBACK_QUEUE_SIZE  160
#define PLAYBACK_CHUNK_SIZE  1440   // 30ms of 24kHz PCM16 mono = 720 samples
typedef struct {
    int16_t pcm[PLAYBACK_CHUNK_SIZE / sizeof(int16_t)];
    size_t samples;
} audio_chunk_t;

#define MIC_PREBUFFER_MS         2000
#define MIC_PREBUFFER_FLUSH_MS   1200
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
static void session_error_cleanup_task(void *arg);
static void on_settings_changed(uint32_t changed_mask);

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

    if (strcmp(wakeword, "hey_jarvis") == 0) {
        return slight ? 0.97f : (very ? 0.83f : 0.92f);
    }
    if (strcmp(wakeword, "hey_mycroft") == 0) {
        return slight ? 0.99f : (very ? 0.93f : 0.95f);
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
}

static void write_wake_tone(int freq_hz, int duration_ms, int32_t amplitude)
{
    const int sample_rate = I2S_OUT_SAMPLE_RATE;
    const int total_samples = sample_rate * duration_ms / 1000;
    const int frames_per_chunk = sample_rate * 10 / 1000;
    const float two_pi_f_over_sr = 2.0f * (float)M_PI * (float)freq_hz / (float)sample_rate;
    int32_t *buf = heap_caps_malloc(frames_per_chunk * 2 * sizeof(int32_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        return;
    }

    int idx = 0;
    while (idx < total_samples) {
        int n = total_samples - idx;
        if (n > frames_per_chunk) n = frames_per_chunk;

        for (int i = 0; i < n; i++) {
            int pos = idx + i;
            int fade = sample_rate * 8 / 1000;
            float env = 1.0f;
            if (fade > 0 && pos < fade) {
                env = (float)pos / (float)fade;
            } else if (fade > 0 && total_samples - pos < fade) {
                env = (float)(total_samples - pos) / (float)fade;
            }
            int32_t sample = (int32_t)(sinf(two_pi_f_over_sr * (float)pos) *
                                       (float)amplitude * env);
            buf[i * 2] = sample;
            buf[i * 2 + 1] = sample;
        }
        idx += n;

        size_t written = 0;
        if (mm_i2s_write(buf, n * 2 * sizeof(int32_t), &written, 100) != ESP_OK) {
            break;
        }
    }

    free(buf);
}

static void play_wake_chime(void)
{
    write_wake_tone(660, 55, 7000000);
    vTaskDelay(pdMS_TO_TICKS(20));
    write_wake_tone(990, 75, 6500000);
    vTaskDelay(pdMS_TO_TICKS(25));
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
    // RESPONDING event finally fires. Also exposes a "device heard you" cue
    // via the beep below — useful both as UX and as an end-to-end audio test.
    speaker_amp_enable(true);
    audio_chunk_t dummy;
    while (xQueueReceive(playback_queue, &dummy, 0) == pdTRUE);
    play_wake_chime();

    ws_client_connect();
}

static void mic_audio_levels(const int32_t *mic_buf, size_t frames,
                             int32_t *avg_abs, int32_t *peak_abs)
{
    int64_t sum_abs = 0;
    int32_t peak = 0;

    for (size_t i = 0; i < frames; i++) {
        int32_t sample = mic_buf[i * 2 + MWW_INPUT_CHANNEL] >> 16;
        int32_t abs_sample = sample < 0 ? -sample : sample;
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
// XMOS reset
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// XMOS pipeline stage configuration via I2C
// Mirrors ESPHome voice_kit component write_pipeline_stages().
// Must be called after XMOS boot (>3s after reset).
// XMOS I2C slave address: 0x2C (sln_voice default)
// ---------------------------------------------------------------------------
#if 0
#define XMOS_I2C_ADDR                   0x2C
#define XMOS_CONFIGURATION_SERVICER_RESID        241
#define XMOS_CHANNEL_0_PIPELINE_STAGE_REG        0x30
#define XMOS_CHANNEL_1_PIPELINE_STAGE_REG        0x40
#define XMOS_PIPELINE_STAGE_AGC                  4

static void xmos_configure_pipeline(void)
{
    // Channel 0: AGC (fully processed: AEC+IC+NS+AGC) â€” used by MWW and VA
    uint8_t ch0_cmd[] = {
        XMOS_CONFIGURATION_SERVICER_RESID,
        XMOS_CHANNEL_0_PIPELINE_STAGE_REG,
        1,
        XMOS_PIPELINE_STAGE_AGC
    };
    esp_err_t ret = i2c_master_write_to_device(0, XMOS_I2C_ADDR,  // I2C_NUM_0 = 0
                                               ch0_cmd, sizeof(ch0_cmd),
                                               pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "XMOS ch0 pipeline config failed (addr=0x%02x): %s",
                 XMOS_I2C_ADDR, esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "XMOS ch0 pipeline: AGC");
    }

    // Channel 1: AGC â€” same processing on both channels
    uint8_t ch1_cmd[] = {
        XMOS_CONFIGURATION_SERVICER_RESID,
        XMOS_CHANNEL_1_PIPELINE_STAGE_REG,
        1,
        XMOS_PIPELINE_STAGE_AGC
    };
    ret = i2c_master_write_to_device(0, XMOS_I2C_ADDR,  // I2C_NUM_0 = 0
                                     ch1_cmd, sizeof(ch1_cmd),
                                     pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "XMOS ch1 pipeline config failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "XMOS ch1 pipeline: AGC");
    }
}
#endif

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
    // Queue response audio for playback task
    // Split into chunks if needed
    size_t offset = 0;
    while (offset < samples) {
        audio_chunk_t chunk;
        chunk.samples = samples - offset;
        if (chunk.samples > PLAYBACK_CHUNK_SIZE / sizeof(int16_t)) {
            chunk.samples = PLAYBACK_CHUNK_SIZE / sizeof(int16_t);
        }
        memcpy(chunk.pcm, &pcm[offset], chunk.samples * sizeof(int16_t));
        offset += chunk.samples;

        if (xQueueSend(playback_queue, &chunk, pdMS_TO_TICKS(10)) != pdTRUE) {
            ESP_LOGW(TAG, "Playback queue full, dropping audio chunk");
        }
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
            ESP_LOGI(TAG, "Ending session after deferred device command response");
            end_session_after_response = false;
            ws_client_disconnect();
            speaker_amp_enable(false);
            app_state = APP_STATE_IDLE;
            led_control_set_state(LED_STATE_IDLE);
            mww_restart_not_before_ms = (esp_timer_get_time() / 1000) + 2000;
            media_radio_resume();
        }
        break;
    case WS_STATE_LISTENING:
        led_control_set_state(LED_STATE_SESSION_ACTIVE);
        api_vad_seen = true;
        last_activity_ms = esp_timer_get_time() / 1000;
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
    // Output buffer: 48kHz stereo 32bit — sized for one chunk after 2× upsample.
    // chunk.samples ≤ 720 (24 kHz, 30 ms) → out_frames ≤ 1440 stereo frames →
    // 1440 * 2 ch * 4 B = 11520 B. Allocate 16 KiB to be safe.
    const size_t spk_buf_bytes = 16 * 1024;
    int32_t *spk_buf = heap_caps_malloc(spk_buf_bytes, MALLOC_CAP_SPIRAM);
    audio_chunk_t chunk;
    const size_t silence_frames = I2S_OUT_SAMPLE_RATE * AUDIO_BUF_SIZE_MS / 1000;

    while (1) {
        if (xQueueReceive(playback_queue, &chunk, pdMS_TO_TICKS(100)) == pdTRUE) {
            // Resample 24kHz/16bit/mono → 48kHz/32bit/stereo
            size_t out_frames = audio_resample_to_speaker(
                chunk.pcm, chunk.samples,
                spk_buf, spk_buf_bytes);

            size_t bytes_to_write = out_frames * 2 * sizeof(int32_t);
            size_t written = 0;
            esp_err_t ret = mm_i2s_write(spk_buf, bytes_to_write, &written, 500);
            if (ret != ESP_OK || written != bytes_to_write) {
                ESP_LOGW(TAG, "Playback I2S write failed: wrote=%u/%u ret=%s",
                         (unsigned)written, (unsigned)bytes_to_write,
                         esp_err_to_name(ret));
            }
        } else if (app_state == APP_STATE_SESSION && speaker_amp_on) {
            memset(spk_buf, 0, silence_frames * 2 * sizeof(int32_t));
            size_t written = 0;
            mm_i2s_write(spk_buf, silence_frames * 2 * sizeof(int32_t),
                         &written, 100);
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
    int64_t barge_in_until_ms = 0;

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
        if (last_avg_abs >= SESSION_LOCAL_SPEECH_AVG_THRESHOLD) {
            last_activity_ms = level_now_ms;
        }
        if (ws_client_get_state() == WS_STATE_RESPONDING &&
            last_avg_abs >= SESSION_BARGE_IN_AVG_THRESHOLD) {
            barge_in_until_ms = level_now_ms + 1200;
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

            ws_state_t ws_state = ws_client_get_state();
            bool can_send = ws_state == WS_STATE_CONNECTED ||
                            ws_state == WS_STATE_LISTENING ||
                            ws_state == WS_STATE_TOOL_RUNNING ||
                            (ws_state == WS_STATE_RESPONDING &&
                             level_now_ms < barge_in_until_ms);

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
// Wake word detection placeholder
// ---------------------------------------------------------------------------
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
// Session timeout monitor
// ---------------------------------------------------------------------------
static void session_monitor_task(void *arg)
{
    while (1) {
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

    // Step 6: Start settings web server (accessible on local network)
    settings_server_start(&config, on_settings_changed);

    // Step 7: Init I2C codec driver, then bring up XMOS Voice Kit.
    // aic3204_init installs the shared I2C driver used by the XMOS control port.
    aic3204_init();

    xmos_firmware_version_t xmos_version = {0};
    esp_err_t xmos_ret = xmos_voice_kit_setup(XMOS_PIPELINE_STAGE_AGC,
                                              XMOS_PIPELINE_STAGE_AGC,
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
        .sliding_window_size = 5,
        .tensor_arena_size = 0,
        .gain_factor = 1,
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
