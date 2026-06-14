/**
 * HTTP internet radio streamer using Espressif ESP Audio Simple Decoder.
 *
 * Supports MP3, AAC/ADTS, M4A, TS and WAV streams/files. Decoded PCM is
 * converted to the board speaker format: 48 kHz, stereo, signed 32-bit I2S.
 */

#include "media_radio.h"
#include "app_config.h"
#include "aic3204.h"
#include "i2s_hal.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "decoder/impl/esp_aac_dec.h"
#include "simple_dec/impl/esp_m4a_dec.h"
#include "simple_dec/impl/esp_ts_dec.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "radio";

#define RADIO_HTTP_BUF_SIZE 2048
#define RADIO_PCM_BUF_SIZE  16384
#define RADIO_OUT_RATE      48000
#define RADIO_QUEUE_CHUNK_FRAMES 1024
#define RADIO_QUEUE_CHUNKS 24
#define RADIO_PREBUFFER_CHUNKS 6

typedef union {
    esp_m4a_dec_cfg_t m4a_cfg;
    esp_ts_dec_cfg_t ts_cfg;
    esp_aac_dec_cfg_t aac_cfg;
} radio_dec_cfg_t;

typedef struct {
    size_t bytes;
    int32_t data[RADIO_QUEUE_CHUNK_FRAMES * 2];
} radio_i2s_chunk_t;

typedef struct {
    uint32_t input_rate;
    uint32_t channels;
    uint64_t input_index;
    uint64_t next_out_q32;
    uint64_t step_q32;
    bool have_prev;
    int16_t prev_l;
    int16_t prev_r;
    radio_i2s_chunk_t *chunk;
    size_t chunk_frames;
} radio_resampler_t;

static TaskHandle_t radio_task_handle = NULL;
static TaskHandle_t radio_playback_task_handle = NULL;
static QueueHandle_t radio_free_q = NULL;
static QueueHandle_t radio_play_q = NULL;
static radio_i2s_chunk_t *radio_chunks[RADIO_QUEUE_CHUNKS];
static volatile bool stop_requested = false;
static volatile radio_state_t radio_state = RADIO_STATE_STOPPED;
static char current_url[256];
static char last_error[128];
static char pending_resume_url[256];   // non-empty when a session-paused stream wants to come back

static void radio_amp_enable(bool enable)
{
    // INPUT_OUTPUT (not plain OUTPUT): keeps the input sampler alive so the
    // settings UI can read the actual pin level via gpio_get_level().
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_SPEAKER_AMP),
        .mode = GPIO_MODE_INPUT_OUTPUT,
    };
    gpio_config(&io_conf);
    gpio_set_level(PIN_SPEAKER_AMP, enable ? 1 : 0);
}

static void set_error(const char *msg)
{
    strncpy(last_error, msg, sizeof(last_error) - 1);
    last_error[sizeof(last_error) - 1] = '\0';
    ESP_LOGE(TAG, "%s", last_error);
}

static esp_audio_simple_dec_type_t type_from_url(const char *url)
{
    const char *q = strchr(url, '?');
    size_t len = q ? (size_t)(q - url) : strlen(url);
    const char *dot = NULL;
    for (const char *p = url; p < url + len; p++) {
        if (*p == '.') dot = p;
    }
    if (!dot) return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;

    if (strncasecmp(dot, ".mp3", 4) == 0) return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    if (strncasecmp(dot, ".aac", 4) == 0) return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    if (strncasecmp(dot, ".m4a", 4) == 0 || strncasecmp(dot, ".mp4", 4) == 0) return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
    if (strncasecmp(dot, ".ts", 3) == 0) return ESP_AUDIO_SIMPLE_DEC_TYPE_TS;
    if (strncasecmp(dot, ".wav", 4) == 0) return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
    return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
}

static esp_audio_simple_dec_type_t type_from_content_type(const char *content_type)
{
    if (!content_type) return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
    if (strstr(content_type, "mpeg") || strstr(content_type, "mp3")) return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    if (strstr(content_type, "aac") || strstr(content_type, "aacp")) return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    if (strstr(content_type, "mp4") || strstr(content_type, "m4a")) return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
    if (strstr(content_type, "mp2t")) return ESP_AUDIO_SIMPLE_DEC_TYPE_TS;
    if (strstr(content_type, "wav")) return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
    return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
}

static void fill_decoder_config(esp_audio_simple_dec_type_t type,
                                radio_dec_cfg_t *all_cfg,
                                esp_audio_simple_dec_cfg_t *dec_cfg)
{
    memset(all_cfg, 0, sizeof(*all_cfg));
    dec_cfg->dec_type = type;
    dec_cfg->use_frame_dec = false;

    switch (type) {
    case ESP_AUDIO_SIMPLE_DEC_TYPE_AAC:
        all_cfg->aac_cfg = (esp_aac_dec_cfg_t)ESP_AAC_DEC_CONFIG_DEFAULT();
        all_cfg->aac_cfg.aac_plus_enable = true;
        dec_cfg->dec_cfg = &all_cfg->aac_cfg;
        dec_cfg->cfg_size = sizeof(all_cfg->aac_cfg);
        break;
    case ESP_AUDIO_SIMPLE_DEC_TYPE_M4A:
        all_cfg->m4a_cfg.aac_plus_enable = true;
        dec_cfg->dec_cfg = &all_cfg->m4a_cfg;
        dec_cfg->cfg_size = sizeof(all_cfg->m4a_cfg);
        break;
    case ESP_AUDIO_SIMPLE_DEC_TYPE_TS:
        all_cfg->ts_cfg.aac_plus_enable = true;
        dec_cfg->dec_cfg = &all_cfg->ts_cfg;
        dec_cfg->cfg_size = sizeof(all_cfg->ts_cfg);
        break;
    default:
        dec_cfg->dec_cfg = NULL;
        dec_cfg->cfg_size = 0;
        break;
    }
}

static void radio_destroy_queues(void)
{
    if (radio_free_q) {
        vQueueDelete(radio_free_q);
        radio_free_q = NULL;
    }
    if (radio_play_q) {
        vQueueDelete(radio_play_q);
        radio_play_q = NULL;
    }
    for (size_t i = 0; i < RADIO_QUEUE_CHUNKS; i++) {
        free(radio_chunks[i]);
        radio_chunks[i] = NULL;
    }
}

static esp_err_t radio_create_queues(void)
{
    radio_destroy_queues();

    radio_free_q = xQueueCreate(RADIO_QUEUE_CHUNKS, sizeof(radio_i2s_chunk_t *));
    radio_play_q = xQueueCreate(RADIO_QUEUE_CHUNKS, sizeof(radio_i2s_chunk_t *));
    if (!radio_free_q || !radio_play_q) {
        radio_destroy_queues();
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < RADIO_QUEUE_CHUNKS; i++) {
        radio_chunks[i] = heap_caps_malloc(sizeof(radio_i2s_chunk_t),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!radio_chunks[i]) {
            radio_destroy_queues();
            return ESP_ERR_NO_MEM;
        }
        radio_i2s_chunk_t *chunk = radio_chunks[i];
        xQueueSend(radio_free_q, &chunk, 0);
    }
    return ESP_OK;
}

static void radio_playback_task(void *arg)
{
    bool prebuffered = false;
    uint32_t underruns = 0;

    while (!stop_requested) {
        if (!prebuffered) {
            while (!stop_requested &&
                   uxQueueMessagesWaiting(radio_play_q) < RADIO_PREBUFFER_CHUNKS) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            if (stop_requested) break;
            prebuffered = true;
            ESP_LOGI(TAG, "Radio prebuffer ready: %u chunks",
                     (unsigned)uxQueueMessagesWaiting(radio_play_q));
        }

        radio_i2s_chunk_t *chunk = NULL;
        if (xQueueReceive(radio_play_q, &chunk, pdMS_TO_TICKS(100)) != pdTRUE) {
            underruns++;
            if (underruns == 1 || (underruns % 10) == 0) {
                ESP_LOGW(TAG, "Radio playback underrun count=%lu free=%u play=%u",
                         (unsigned long)underruns,
                         (unsigned)uxQueueMessagesWaiting(radio_free_q),
                         (unsigned)uxQueueMessagesWaiting(radio_play_q));
            }
            prebuffered = false;
            continue;
        }

        size_t written = 0;
        esp_err_t ret = mm_i2s_write(chunk->data, chunk->bytes, &written, 500);
        if (ret != ESP_OK || written != chunk->bytes) {
            ESP_LOGW(TAG, "Radio I2S write: wrote=%u/%u ret=%s",
                     (unsigned)written, (unsigned)chunk->bytes, esp_err_to_name(ret));
        }

        xQueueSend(radio_free_q, &chunk, portMAX_DELAY);
    }

    radio_i2s_chunk_t *chunk = NULL;
    while (radio_play_q && xQueueReceive(radio_play_q, &chunk, 0) == pdTRUE) {
        xQueueSend(radio_free_q, &chunk, 0);
    }

    radio_playback_task_handle = NULL;
    vTaskDelete(NULL);
}

static esp_err_t radio_send_resampled_chunk(radio_resampler_t *rs)
{
    if (!rs->chunk || rs->chunk_frames == 0) {
        return ESP_OK;
    }

    rs->chunk->bytes = rs->chunk_frames * 2 * sizeof(int32_t);
    radio_i2s_chunk_t *ready = rs->chunk;
    rs->chunk = NULL;
    rs->chunk_frames = 0;

    if (xQueueSend(radio_play_q, &ready, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGW(TAG, "Radio play queue full, dropping chunk");
        xQueueSend(radio_free_q, &ready, 0);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static esp_err_t radio_enqueue_frame(radio_resampler_t *rs, int32_t left, int32_t right)
{
    if (!rs->chunk) {
        if (xQueueReceive(radio_free_q, &rs->chunk, pdMS_TO_TICKS(500)) != pdTRUE) {
            ESP_LOGW(TAG, "Radio free queue empty");
            return ESP_ERR_TIMEOUT;
        }
        rs->chunk_frames = 0;
    }

    rs->chunk->data[rs->chunk_frames * 2] = left;
    rs->chunk->data[rs->chunk_frames * 2 + 1] = right;
    rs->chunk_frames++;

    if (rs->chunk_frames >= RADIO_QUEUE_CHUNK_FRAMES) {
        return radio_send_resampled_chunk(rs);
    }
    return ESP_OK;
}

static void radio_get_frame(const int16_t *in, size_t in_frames,
                            const radio_resampler_t *rs, uint64_t abs_index,
                            int16_t *left, int16_t *right)
{
    if (rs->have_prev && abs_index + 1 == rs->input_index) {
        *left = rs->prev_l;
        *right = rs->prev_r;
        return;
    }

    size_t rel = (size_t)(abs_index - rs->input_index);
    if (rs->channels == 1) {
        *left = *right = in[rel];
    } else {
        *left = in[rel * 2];
        *right = in[rel * 2 + 1];
    }
}

static int32_t lerp_16_to_32(int16_t a, int16_t b, uint32_t frac_q32)
{
    int64_t delta = (int64_t)b - (int64_t)a;
    int64_t sample = (int64_t)a + ((delta * (int64_t)frac_q32) >> 32);
    return (int32_t)sample << 16;
}

static esp_err_t queue_decoded_pcm(const uint8_t *pcm, size_t pcm_bytes,
                                   const esp_audio_simple_dec_info_t *info,
                                   radio_resampler_t *rs)
{
    if (info->bits_per_sample != 16 || info->sample_rate == 0 ||
        info->channel == 0 || info->channel > 2) {
        return ESP_ERR_INVALID_ARG;
    }

    if (rs->input_rate != info->sample_rate || rs->channels != info->channel) {
        memset(rs, 0, sizeof(*rs));
        rs->input_rate = info->sample_rate;
        rs->channels = info->channel;
        rs->step_q32 = ((uint64_t)info->sample_rate << 32) / RADIO_OUT_RATE;
    }

    const int16_t *in = (const int16_t *)pcm;
    size_t bytes_per_frame = (size_t)info->channel * sizeof(int16_t);
    size_t in_frames = pcm_bytes / bytes_per_frame;
    if (in_frames == 0) return ESP_OK;

    uint64_t chunk_end = rs->input_index + in_frames;
    while (!stop_requested) {
        uint64_t src_abs = rs->next_out_q32 >> 32;
        uint32_t frac = (uint32_t)(rs->next_out_q32 & 0xFFFFFFFFULL);

        if (src_abs + 1 >= chunk_end) {
            break;
        }
        if (src_abs < rs->input_index && !(rs->have_prev && src_abs + 1 == rs->input_index)) {
            rs->next_out_q32 = rs->input_index << 32;
            continue;
        }

        int16_t l0, r0, l1, r1;
        radio_get_frame(in, in_frames, rs, src_abs, &l0, &r0);
        radio_get_frame(in, in_frames, rs, src_abs + 1, &l1, &r1);

        esp_err_t ret = radio_enqueue_frame(rs, lerp_16_to_32(l0, l1, frac),
                                            lerp_16_to_32(r0, r1, frac));
        if (ret != ESP_OK) return ret;
        rs->next_out_q32 += rs->step_q32;
    }

    if (info->channel == 1) {
        rs->prev_l = rs->prev_r = in[in_frames - 1];
    } else {
        rs->prev_l = in[(in_frames - 1) * 2];
        rs->prev_r = in[(in_frames - 1) * 2 + 1];
    }
    rs->have_prev = true;
    rs->input_index = chunk_end;
    return ESP_OK;
}

static void radio_task(void *arg)
{
    uint8_t *in_buf = heap_caps_malloc(RADIO_HTTP_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *pcm_buf = heap_caps_malloc(RADIO_PCM_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    esp_http_client_handle_t client = NULL;
    esp_audio_simple_dec_handle_t decoder = NULL;
    esp_audio_simple_dec_info_t dec_info = {0};
    radio_resampler_t resampler = {0};
    bool have_info = false;
    uint32_t decoded_chunks = 0;

    if (!in_buf || !pcm_buf) {
        set_error("Radio buffer allocation failed");
        radio_state = RADIO_STATE_ERROR;
        goto done;
    }

    esp_audio_dec_register_default();
    esp_audio_simple_dec_register_default();

    radio_state = RADIO_STATE_CONNECTING;
    last_error[0] = '\0';

    esp_http_client_config_t http_cfg = {
        .url = current_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
        .buffer_size = RADIO_HTTP_BUF_SIZE,
        .user_agent = "micimike-ai-fw/1.0",
    };

    client = esp_http_client_init(&http_cfg);
    if (!client) {
        set_error("HTTP client init failed");
        radio_state = RADIO_STATE_ERROR;
        goto done;
    }

    esp_http_client_set_header(client, "Icy-MetaData", "0");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        snprintf(last_error, sizeof(last_error), "HTTP open failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", last_error);
        radio_state = RADIO_STATE_ERROR;
        goto done;
    }

    int64_t content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    char *content_type = NULL;
    esp_http_client_get_header(client, "Content-Type", &content_type);

    esp_audio_simple_dec_type_t dec_type = type_from_content_type(content_type);
    if (dec_type == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE) {
        dec_type = type_from_url(current_url);
    }
    if (dec_type == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE) {
        dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
        ESP_LOGW(TAG, "Unknown radio format, trying MP3 decoder");
    }

    ESP_LOGI(TAG, "Radio connected: status=%d len=%lld type=%s decoder=%s",
             status, content_len, content_type ? content_type : "unknown",
             esp_audio_simple_dec_get_name(dec_type));

    radio_dec_cfg_t all_cfg = {0};
    esp_audio_simple_dec_cfg_t dec_cfg = {0};
    fill_decoder_config(dec_type, &all_cfg, &dec_cfg);

    esp_audio_err_t dec_ret = esp_audio_simple_dec_open(&dec_cfg, &decoder);
    if (dec_ret != ESP_AUDIO_ERR_OK) {
        snprintf(last_error, sizeof(last_error), "Decoder open failed: %d", dec_ret);
        ESP_LOGE(TAG, "%s", last_error);
        radio_state = RADIO_STATE_ERROR;
        goto done;
    }

    aic3204_mute(false);
    radio_amp_enable(true);

    if (radio_create_queues() != ESP_OK) {
        set_error("Radio queue allocation failed");
        radio_state = RADIO_STATE_ERROR;
        goto done;
    }
    BaseType_t play_ok = xTaskCreatePinnedToCore(radio_playback_task, "radio_play",
                                                 4096, NULL, 12,
                                                 &radio_playback_task_handle, 1);
    if (play_ok != pdPASS) {
        set_error("Failed to create radio playback task");
        radio_state = RADIO_STATE_ERROR;
        goto done;
    }

    radio_state = RADIO_STATE_PLAYING;

    while (!stop_requested) {
        int r = esp_http_client_read(client, (char *)in_buf, RADIO_HTTP_BUF_SIZE);
        if (r < 0) {
            set_error("HTTP read failed");
            radio_state = RADIO_STATE_ERROR;
            break;
        }
        if (r == 0) {
            ESP_LOGI(TAG, "Radio stream ended");
            break;
        }

        esp_audio_simple_dec_raw_t raw = {
            .buffer = in_buf,
            .len = (uint32_t)r,
            .eos = false,
        };

        while (raw.len > 0 && !stop_requested) {
            esp_audio_simple_dec_out_t out = {
                .buffer = pcm_buf,
                .len = RADIO_PCM_BUF_SIZE,
            };

            dec_ret = esp_audio_simple_dec_process(decoder, &raw, &out);
            if (dec_ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                snprintf(last_error, sizeof(last_error), "PCM buffer too small, need %u", (unsigned)out.needed_size);
                ESP_LOGE(TAG, "%s", last_error);
                radio_state = RADIO_STATE_ERROR;
                stop_requested = true;
                break;
            }
            if (dec_ret != ESP_AUDIO_ERR_OK) {
                ESP_LOGW(TAG, "Decode returned %d, skipping chunk", dec_ret);
                break;
            }

            if (out.decoded_size > 0) {
                if (!have_info) {
                    if (esp_audio_simple_dec_get_info(decoder, &dec_info) == ESP_AUDIO_ERR_OK) {
                        have_info = true;
                        ESP_LOGI(TAG, "Radio audio: %lu Hz, %u ch, %u bits, bitrate=%lu",
                                 (unsigned long)dec_info.sample_rate, dec_info.channel,
                                 dec_info.bits_per_sample, (unsigned long)dec_info.bitrate);
                    }
                }

                if (have_info) {
                    queue_decoded_pcm(out.buffer, out.decoded_size, &dec_info, &resampler);
                    if ((decoded_chunks++ % 100) == 0) {
                        ESP_LOGI(TAG, "Radio decoded: chunk=%lu pcm=%u free=%u play=%u",
                                 (unsigned long)decoded_chunks, (unsigned)out.decoded_size,
                                 (unsigned)uxQueueMessagesWaiting(radio_free_q),
                                 (unsigned)uxQueueMessagesWaiting(radio_play_q));
                    }
                }
            }

            if (raw.consumed == 0) {
                break;
            }
            raw.len -= raw.consumed;
            raw.buffer += raw.consumed;
        }
    }

done:
    radio_send_resampled_chunk(&resampler);
    stop_requested = true;
    if (radio_playback_task_handle) {
        for (int i = 0; i < 20 && radio_playback_task_handle; i++) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    radio_amp_enable(false);
    if (decoder) esp_audio_simple_dec_close(decoder);
    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    free(in_buf);
    free(pcm_buf);
    radio_destroy_queues();
    if (radio_state != RADIO_STATE_ERROR) {
        radio_state = RADIO_STATE_STOPPED;
    }
    radio_task_handle = NULL;
    stop_requested = false;
    vTaskDelete(NULL);
}

esp_err_t media_radio_start(const char *url)
{
    if (!url || url[0] == '\0') {
        set_error("Radio URL is empty");
        return ESP_ERR_INVALID_ARG;
    }

    if (radio_task_handle) {
        media_radio_stop();
        for (int i = 0; i < 40 && radio_task_handle; i++) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (radio_task_handle) {
            set_error("Previous radio task did not stop");
            return ESP_ERR_INVALID_STATE;
        }
    }

    strncpy(current_url, url, sizeof(current_url) - 1);
    current_url[sizeof(current_url) - 1] = '\0';
    stop_requested = false;

    BaseType_t ok = xTaskCreatePinnedToCore(radio_task, "radio", 24576, NULL, 8,
                                            &radio_task_handle, 1);
    if (ok != pdPASS) {
        set_error("Failed to create radio task");
        radio_task_handle = NULL;
        radio_state = RADIO_STATE_ERROR;
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t media_radio_stop(void)
{
    stop_requested = true;
    radio_amp_enable(false);
    radio_state = RADIO_STATE_STOPPED;
    return ESP_OK;
}

// Internal hook so callers from the HTTP API (explicit user stop) can clear
// the pending resume slot; pause_for_session() should NOT clear it.
// Note: media_radio_stop() above is reused by pause_for_session, so we don't
// touch pending_resume_url there.

radio_state_t media_radio_get_state(void)
{
    return radio_state;
}

const char *media_radio_get_error(void)
{
    return last_error;
}

void media_radio_pause_for_session(void)
{
    // Snapshot the URL so we can resume after the session ends.
    radio_state_t st = radio_state;
    if ((st == RADIO_STATE_PLAYING || st == RADIO_STATE_CONNECTING)
        && current_url[0] != '\0') {
        strncpy(pending_resume_url, current_url, sizeof(pending_resume_url) - 1);
        pending_resume_url[sizeof(pending_resume_url) - 1] = '\0';
        ESP_LOGI(TAG, "Pausing radio for session (will resume): %s", pending_resume_url);
        media_radio_stop();
    } else {
        // Nothing playing — make sure no stale resume is pending.
        pending_resume_url[0] = '\0';
    }
}

bool media_radio_resume(void)
{
    if (pending_resume_url[0] == '\0') {
        return false;
    }
    char url[sizeof(pending_resume_url)];
    strncpy(url, pending_resume_url, sizeof(url) - 1);
    url[sizeof(url) - 1] = '\0';
    pending_resume_url[0] = '\0';

    ESP_LOGI(TAG, "Resuming radio after session: %s", url);
    return media_radio_start(url) == ESP_OK;
}

void media_radio_schedule_resume(const char *url)
{
    if (!url || url[0] == '\0') {
        pending_resume_url[0] = '\0';
        return;
    }
    strncpy(pending_resume_url, url, sizeof(pending_resume_url) - 1);
    pending_resume_url[sizeof(pending_resume_url) - 1] = '\0';
    ESP_LOGI(TAG, "Radio scheduled after session: %s", pending_resume_url);
}

void media_radio_clear_pending(void)
{
    if (pending_resume_url[0] != '\0') {
        ESP_LOGI(TAG, "Clearing pending radio resume");
        pending_resume_url[0] = '\0';
    }
}
