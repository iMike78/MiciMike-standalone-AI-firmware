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
#include "cJSON.h"
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
// Each chunk is 1024 stereo 32-bit frames ≈ 21 ms of audio at 48 kHz.
// 6 chunks = 128 ms of buffer was right on the edge of what variable
// home WiFi could feed — a single 130 ms hiccup triggered an underrun.
// 12-chunk prebuffer (≈256 ms) and 36-slot queue (≈760 ms) absorb the
// usual jitter; the ~250 KiB SPIRAM cost is negligible on this board.
#define RADIO_QUEUE_CHUNKS 36
#define RADIO_PREBUFFER_CHUNKS 12

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
static TaskHandle_t radio_meta_task_handle = NULL;
static volatile bool radio_meta_stop_requested = false;
static QueueHandle_t radio_free_q = NULL;
static QueueHandle_t radio_play_q = NULL;
static radio_i2s_chunk_t *radio_chunks[RADIO_QUEUE_CHUNKS];
static volatile bool stop_requested = false;
static volatile radio_state_t radio_state = RADIO_STATE_STOPPED;
static char current_url[256];
static char last_error[128];
static char pending_resume_url[256];   // non-empty when a session-paused stream wants to come back

// Shoutcast/Icecast (ICY) stream metadata: most internet radio servers
// embed the currently-playing track inline between audio bytes, every
// icy-metaint bytes. We demultiplex it in the read loop and store the
// parsed "StreamTitle" here so the web UI can show what's playing.
// Empty string = no metadata yet (also the case for plain MP3/AAC files).
#define RADIO_TRACK_TITLE_SIZE 192
static char current_track_title[RADIO_TRACK_TITLE_SIZE];

static void radio_clear_track_title(void)
{
    current_track_title[0] = '\0';
}

// Shared "now playing" setter. Used by every metadata source we support
// (HTTP poller, ID3v2 sniff, ICY demux) so we get one canonical log
// message and one no-op-on-unchanged check, regardless of which channel
// the title came in on.
static void radio_set_track_title(const char *title, const char *source)
{
    if (!title) return;
    while (*title == ' ' || *title == '\t' || *title == '\r' || *title == '\n') title++;
    size_t len = strlen(title);
    while (len > 0 && (title[len - 1] == ' ' || title[len - 1] == '\t' ||
                       title[len - 1] == '\r' || title[len - 1] == '\n')) len--;
    if (len == 0) return;
    if (len >= RADIO_TRACK_TITLE_SIZE) len = RADIO_TRACK_TITLE_SIZE - 1;
    if (len == strnlen(current_track_title, RADIO_TRACK_TITLE_SIZE) &&
        memcmp(current_track_title, title, len) == 0) {
        return;
    }
    memcpy(current_track_title, title, len);
    current_track_title[len] = '\0';
    ESP_LOGI(TAG, "Now playing (%s): %s", source ? source : "?", current_track_title);
}

// Extract StreamTitle='...'; from an ICY metadata block. Currently only
// reachable if a future patch turns ICY back on safely; kept so it stays
// next to the other metadata parsers.
static bool radio_parse_icy_metadata(const char *meta)
{
    if (!meta || !*meta) return false;
    const char *p = strstr(meta, "StreamTitle='");
    if (!p) return false;
    p += strlen("StreamTitle='");
    const char *end = strstr(p, "';");
    if (!end) end = p + strlen(p);
    size_t len = (size_t)(end - p);
    if (len == 0) return false;
    char tmp[RADIO_TRACK_TITLE_SIZE];
    if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
    memcpy(tmp, p, len);
    tmp[len] = '\0';
    radio_set_track_title(tmp, "icy");
    return true;
}

// -----------------------------------------------------------------------
// ID3v2 sniff: parsed from the FIRST HTTP read of the audio stream. Most
// internet radio streams don't carry ID3 (they're pure MP3 frames), but
// direct-file URLs like https://example.com/song.mp3 usually do. The
// decoder itself skips the tag internally — we just sneak a peek before
// handing the buffer over so we can pull TIT2 / TPE1 out for the UI.
// We do NOT modify the buffer.
// -----------------------------------------------------------------------
static uint32_t id3_synchsafe(const uint8_t *p)
{
    return ((uint32_t)(p[0] & 0x7f) << 21) |
           ((uint32_t)(p[1] & 0x7f) << 14) |
           ((uint32_t)(p[2] & 0x7f) << 7) |
           ((uint32_t)(p[3] & 0x7f));
}

static void radio_sniff_id3v2(const uint8_t *buf, size_t len)
{
    if (len < 10) return;
    if (memcmp(buf, "ID3", 3) != 0) return;

    uint8_t ver_major = buf[3];     // 3 or 4 in practice
    uint32_t tag_size = id3_synchsafe(buf + 6);
    if (tag_size == 0 || tag_size > len - 10) {
        // Tag spans past what we read in one HTTP buffer — skip; we'd
        // rather miss the title than risk anything else.
        return;
    }

    char title[RADIO_TRACK_TITLE_SIZE] = {0};
    char artist[RADIO_TRACK_TITLE_SIZE] = {0};
    size_t pos = 10;

    while (pos + 10 <= 10 + tag_size && pos + 10 <= len) {
        const uint8_t *frame = buf + pos;
        if (frame[0] == 0) break;  // padding
        // ID3v2.3 uses raw big-endian size, v2.4 uses synchsafe. Both
        // sit at frame[4..7].
        uint32_t fsize = (ver_major >= 4)
            ? id3_synchsafe(frame + 4)
            : (((uint32_t)frame[4] << 24) | ((uint32_t)frame[5] << 16) |
               ((uint32_t)frame[6] << 8)  | (uint32_t)frame[7]);
        if (fsize == 0 || pos + 10 + fsize > len) break;

        const uint8_t *fdata = frame + 10;
        bool is_title  = (memcmp(frame, "TIT2", 4) == 0);
        bool is_artist = (memcmp(frame, "TPE1", 4) == 0);
        if ((is_title || is_artist) && fsize >= 2) {
            uint8_t enc = fdata[0];
            const char *text = (const char *)(fdata + 1);
            size_t tlen = fsize - 1;
            // Only handle ISO-8859-1 (0) and UTF-8 (3). UTF-16 (1/2)
            // would need conversion and is rare on radio streams.
            if (enc == 0 || enc == 3) {
                char *dst = is_title ? title : artist;
                size_t copy = tlen < RADIO_TRACK_TITLE_SIZE - 1
                              ? tlen
                              : RADIO_TRACK_TITLE_SIZE - 1;
                memcpy(dst, text, copy);
                dst[copy] = '\0';
            }
        }
        pos += 10 + fsize;
    }

    if (title[0] && artist[0]) {
        // Big enough to hold both halves plus the " - " separator
        // without -Wformat-truncation tripping.
        char combined[RADIO_TRACK_TITLE_SIZE * 2 + 4];
        snprintf(combined, sizeof(combined), "%s - %s", artist, title);
        radio_set_track_title(combined, "id3v2");
    } else if (title[0]) {
        radio_set_track_title(title, "id3v2");
    } else if (artist[0]) {
        radio_set_track_title(artist, "id3v2");
    }
}

// -----------------------------------------------------------------------
// Out-of-band metadata poller. We open a SEPARATE short-lived HTTP
// request to a stats endpoint every ~10 s and read the currently-
// playing track from there. The audio stream HTTP socket and decoder
// are not touched, so even a flaky stats server cannot corrupt audio.
//
// Stations served by Icecast typically expose /status-json.xsl;
// Shoutcast v1 boxes expose /7.html. We try them in order, accept the
// first one that yields a title, and remember which one worked for the
// rest of the playback session.
// -----------------------------------------------------------------------

// Strip the path off a stream URL, leaving just scheme + host + port.
// "https://radio.example.com:8000/stream128?abc" → "https://radio.example.com:8000"
static bool radio_extract_host_base(const char *url, char *out, size_t out_size)
{
    const char *scheme_end = strstr(url, "://");
    if (!scheme_end) return false;
    const char *path_start = strchr(scheme_end + 3, '/');
    size_t len = path_start ? (size_t)(path_start - url) : strlen(url);
    if (len == 0 || len >= out_size) return false;
    memcpy(out, url, len);
    out[len] = '\0';
    return true;
}

// One-shot blocking GET into a flat buffer. Returns ESP_OK and the
// number of bytes read on success, or ESP_FAIL on any error / non-200.
static esp_err_t radio_meta_http_get(const char *url, char *buf, size_t buf_size,
                                     size_t *out_len)
{
    if (!url || !buf || buf_size < 2) return ESP_FAIL;
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 4000,
        .buffer_size = 1024,
        .user_agent = "micimike-ai-fw/1.0",
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    size_t total = 0;
    while (total + 1 < buf_size) {
        int r = esp_http_client_read(client, buf + total, buf_size - 1 - total);
        if (r <= 0) break;
        total += (size_t)r;
    }
    buf[total] = '\0';
    if (out_len) *out_len = total;

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return total > 0 ? ESP_OK : ESP_FAIL;
}

// Icecast /status-json.xsl layout:
//   {"icestats":{"source":[{"title":"...","yp_currently_playing":"..."}, ...]}}
// or with a single source it's a bare object instead of an array.
static bool radio_parse_icecast_status(const char *body)
{
    cJSON *root = cJSON_Parse(body);
    if (!root) return false;
    cJSON *icestats = cJSON_GetObjectItem(root, "icestats");
    if (!icestats) { cJSON_Delete(root); return false; }
    cJSON *source = cJSON_GetObjectItem(icestats, "source");
    if (cJSON_IsArray(source)) source = cJSON_GetArrayItem(source, 0);
    if (!source) { cJSON_Delete(root); return false; }

    const char *picks[] = { "title", "yp_currently_playing", "song", NULL };
    for (int i = 0; picks[i]; i++) {
        cJSON *t = cJSON_GetObjectItem(source, picks[i]);
        if (t && cJSON_IsString(t) && t->valuestring && t->valuestring[0]) {
            radio_set_track_title(t->valuestring, "icecast");
            cJSON_Delete(root);
            return true;
        }
    }
    cJSON_Delete(root);
    return false;
}

// Shoutcast v1 /7.html layout:
//   <HTML>...<body>listeners,status,peak,max,unique,bitrate,Song Title</body></html>
// The 7th comma-separated value is the song title; some servers wrap it
// in HTML entities, we leave those raw — the web UI shows them verbatim.
static bool radio_parse_shoutcast_7html(const char *body)
{
    const char *b = strstr(body, "<body>");
    if (!b) b = strstr(body, "<BODY>");
    if (!b) return false;
    b += 6;
    const char *e = strstr(b, "</body>");
    if (!e) e = strstr(b, "</BODY>");
    if (!e) e = b + strlen(b);

    int commas = 0;
    const char *p = b;
    while (p < e && commas < 6) {
        if (*p == ',') commas++;
        p++;
    }
    if (commas < 6 || p >= e) return false;

    size_t len = (size_t)(e - p);
    if (len == 0 || len >= RADIO_TRACK_TITLE_SIZE) return false;
    char tmp[RADIO_TRACK_TITLE_SIZE];
    memcpy(tmp, p, len);
    tmp[len] = '\0';
    radio_set_track_title(tmp, "shoutcast");
    return true;
}

// Try a metadata endpoint. `prefer_https=true` tries https first, then
// http; false swaps the order. On constrained boards a second TLS
// context sometimes can't allocate while the audio HTTPS session is
// alive, and even plaintext socket() can fail when LWIP's small socket
// pool is contended — both cases come back here as ESP_FAIL and we
// just move on.
static bool radio_meta_try_endpoint(const char *base_url, const char *path,
                                    char *buf, size_t buf_size, size_t *out_len)
{
    char url[256];
    snprintf(url, sizeof(url), "%s%s", base_url, path);
    return radio_meta_http_get(url, buf, buf_size, out_len) == ESP_OK &&
           *out_len > 0;
}

static void radio_meta_poll_task(void *arg)
{
    char base_url[160] = "";
    char cached_url[sizeof(current_url)] = "";

    // 4 KiB is enough headroom for either /status-json.xsl or /7.html
    // payloads we see in the wild; allocated in SPIRAM to stay off the
    // internal heap.
    char *buf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        radio_meta_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    // Sticky source selection: once a probe wins, we keep polling just
    // that one. Until then we cycle through endpoints. After each
    // failed poll we back off exponentially — burning the LWIP socket
    // pool on a host that has no stats endpoint just creates log spam.
    enum { SRC_PROBE_ICECAST, SRC_PROBE_SHOUTCAST,
           SRC_ICECAST, SRC_SHOUTCAST, SRC_GIVEN_UP } src = SRC_PROBE_ICECAST;
    int consecutive_failures = 0;
    const int give_up_after = 4;       // ~4 probe cycles per endpoint = 8 tries total
    int extra_backoff_seconds = 0;     // grows when sockets are scarce

    while (!radio_meta_stop_requested) {
        int wait_ms;
        if (src == SRC_GIVEN_UP) {
            wait_ms = 30000;  // park; only wake to check for URL change
        } else if (src == SRC_ICECAST || src == SRC_SHOUTCAST) {
            wait_ms = 10000;
        } else {
            wait_ms = 3000;
        }
        wait_ms += extra_backoff_seconds * 1000;
        for (int waited = 0; waited < wait_ms && !radio_meta_stop_requested;
             waited += 100) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (radio_meta_stop_requested) break;
        if (radio_state != RADIO_STATE_PLAYING) continue;

        // Station change detection. The poller stays alive across
        // media_radio_stop / media_radio_start cycles so it picks up
        // a switched URL within one cycle instead of requiring a full
        // task tear-down.
        if (strcmp(cached_url, current_url) != 0) {
            strncpy(cached_url, current_url, sizeof(cached_url) - 1);
            cached_url[sizeof(cached_url) - 1] = '\0';
            if (!radio_extract_host_base(current_url, base_url, sizeof(base_url))) {
                base_url[0] = '\0';
                continue;
            }
            src = SRC_PROBE_ICECAST;
            consecutive_failures = 0;
            extra_backoff_seconds = 0;
            ESP_LOGI(TAG, "Metadata poll: switched to %s", base_url);
        }
        if (base_url[0] == '\0') continue;

        size_t got = 0;
        bool got_title = false;
        const char *path = NULL;

        switch (src) {
            case SRC_PROBE_ICECAST:
            case SRC_ICECAST:
                path = "/status-json.xsl";
                break;
            case SRC_PROBE_SHOUTCAST:
            case SRC_SHOUTCAST:
                path = "/7.html";
                break;
            default:
                continue;
        }

        if (radio_meta_try_endpoint(base_url, path, buf, 4096, &got)) {
            if (src == SRC_PROBE_ICECAST || src == SRC_ICECAST) {
                got_title = radio_parse_icecast_status(buf);
                if (got_title) src = SRC_ICECAST;
            } else {
                got_title = radio_parse_shoutcast_7html(buf);
                if (got_title) src = SRC_SHOUTCAST;
            }
        }

        if (got_title) {
            consecutive_failures = 0;
            extra_backoff_seconds = 0;
        } else {
            consecutive_failures++;
            // Cycle to the other probe endpoint after each failed
            // probe so we eventually find the right one.
            if (src == SRC_PROBE_ICECAST) {
                src = SRC_PROBE_SHOUTCAST;
            } else if (src == SRC_PROBE_SHOUTCAST) {
                src = SRC_PROBE_ICECAST;
            }
            if (consecutive_failures >= give_up_after * 2 &&
                src != SRC_ICECAST && src != SRC_SHOUTCAST) {
                src = SRC_GIVEN_UP;
                ESP_LOGI(TAG, "Metadata poll: %s — no stats endpoint, parking task",
                         base_url);
            }
            // Exponential back-off (capped at 30 s extra) when the
            // socket subsystem is starved — stops the log flood when
            // LWIP can't keep opening connections.
            if (extra_backoff_seconds == 0) extra_backoff_seconds = 5;
            else if (extra_backoff_seconds < 30) extra_backoff_seconds *= 2;
        }
    }

    free(buf);
    radio_meta_task_handle = NULL;
    vTaskDelete(NULL);
}

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

// Scan the leading bytes of an audio stream for a frame sync header so
// we can pick the right decoder when the server told us nothing useful.
// Both MPEG-1/2 audio (MP3) and AAC ADTS use 11-bit 0x7FF sync words
// (0xFF E_/F_) but the bits below the sync separate them cleanly:
//   byte[1] bits [2..1] are the "layer" field:
//     00 → AAC ADTS (layer field is always zero for AAC)
//     01 → MP3 layer III (the layer encoding used by every MP3 stream)
// We scan for the first plausible sync and decide from byte[1].
static esp_audio_simple_dec_type_t sniff_audio_format(const uint8_t *buf, size_t len)
{
    if (len >= 3 && memcmp(buf, "ID3", 3) == 0) {
        // ID3v2 header: this is almost certainly a tagged MP3 file —
        // the decoder skips the tag and lands on an MP3 frame.
        return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    }
    if (len >= 4 && memcmp(buf, "ADIF", 4) == 0) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    }
    if (len >= 4 && memcmp(buf, "OggS", 4) == 0) {
        // Ogg container — not supported by our decoder set, just bail
        return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
    }

    size_t limit = len > 512 ? 512 : len;
    for (size_t i = 0; i + 1 < limit; i++) {
        if (buf[i] != 0xFF) continue;
        uint8_t b1 = buf[i + 1];
        // ADTS sync: top 12 bits are 0xFFF, layer (bits 2..1) is 00,
        // so b1 has the pattern 1111 X 00 Y → 0xF0/0xF1/0xF8/0xF9.
        if ((b1 & 0xF6) == 0xF0) {
            return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
        }
        // MP3 sync: top 11 bits are 0x7FF, layer (bits 2..1) is 01 for
        // Layer III. b1 patterns: 0xFB (v1), 0xF3 (v2), 0xE3 (v2.5).
        if ((b1 & 0xE6) == 0xE2) {
            return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
        }
    }
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
    // 4080 bytes is the ICY metadata block hard cap (length byte × 16);
    // 4096 leaves room for a NUL terminator and rounding.
    uint8_t *meta_buf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    esp_http_client_handle_t client = NULL;
    esp_audio_simple_dec_handle_t decoder = NULL;
    esp_audio_simple_dec_info_t dec_info = {0};
    radio_resampler_t resampler = {0};
    bool have_info = false;
    uint32_t decoded_chunks = 0;

    if (!in_buf || !pcm_buf || !meta_buf) {
        set_error("Radio buffer allocation failed");
        radio_state = RADIO_STATE_ERROR;
        goto done;
    }

    // Register the default codec set exactly once for the life of the
    // device. Calling these on every radio start triggers the noisy
    // "Overwrote ES decoder" warning storm from AUD_SDEC_REG (one warn
    // per codec, four total) without changing anything functionally.
    static bool decoders_registered = false;
    if (!decoders_registered) {
        esp_audio_dec_register_default();
        esp_audio_simple_dec_register_default();
        decoders_registered = true;
    }

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

    // Stay on the safe "no inline metadata" path. We tried setting this
    // to "1" so the server would embed Shoutcast `StreamTitle` blocks
    // every icy-metaint bytes for the web UI, but ESP HTTP client does
    // not reliably surface the icy-metaint response header — and some
    // servers embed the metadata anyway, regardless of whether they
    // advertise the interval. The MP3 decoder then trips over the
    // metadata bytes and produces audible clicks (or loses sync
    // entirely). Until we have a robust way to detect or read the
    // interval, we keep ICY off and the track-title field stays empty.
    esp_http_client_set_header(client, "Icy-MetaData", "0");
    radio_clear_track_title();

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        snprintf(last_error, sizeof(last_error), "HTTP open failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", last_error);
        radio_state = RADIO_STATE_ERROR;
        goto done;
    }

    int64_t content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    // Follow 3xx redirects manually — esp_http_client_open + fetch_headers
    // does not chase them automatically (only esp_http_client_perform does,
    // and we can't use perform here because we stream the body ourselves).
    // The Location header is NOT visible via get_header() on this client
    // — it's already consumed into the client's internal `location` slot
    // by fetch_headers. esp_http_client_set_redirection() reads from that
    // slot and updates the URL in place; we just need to close and reopen.
    int redirect_hops = 0;
    while ((status == 301 || status == 302 || status == 303 || status == 307 ||
            status == 308) && redirect_hops < 5) {
        if (esp_http_client_set_redirection(client) != ESP_OK) {
            ESP_LOGW(TAG, "Redirect %d but set_redirection failed (no Location?) — giving up", status);
            break;
        }
        ESP_LOGI(TAG, "Radio redirect %d, hop %d", status, redirect_hops + 1);
        esp_http_client_close(client);
        err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            snprintf(last_error, sizeof(last_error),
                     "Redirect reopen failed: %s", esp_err_to_name(err));
            ESP_LOGE(TAG, "%s", last_error);
            radio_state = RADIO_STATE_ERROR;
            goto done;
        }
        content_len = esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        redirect_hops++;
    }

    if (status != 200 && status != 206) {
        snprintf(last_error, sizeof(last_error),
                 "HTTP status %d after %d redirects", status, redirect_hops);
        ESP_LOGE(TAG, "%s", last_error);
        radio_state = RADIO_STATE_ERROR;
        goto done;
    }

    char *content_type = NULL;
    esp_http_client_get_header(client, "Content-Type", &content_type);

    // ICY metadata interval. 0 means the server didn't honour our request
    // (no metadata embedded — fall back to plain audio stream).
    size_t icy_metaint = 0;
    char *icy_metaint_str = NULL;
    if (esp_http_client_get_header(client, "icy-metaint", &icy_metaint_str) == ESP_OK &&
        icy_metaint_str) {
        long v = atol(icy_metaint_str);
        if (v > 0 && v < 65536) {
            icy_metaint = (size_t)v;
            ESP_LOGI(TAG, "ICY metadata interval: %u bytes", (unsigned)icy_metaint);
        }
    }

    esp_audio_simple_dec_type_t dec_type = type_from_content_type(content_type);
    if (dec_type == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE) {
        dec_type = type_from_url(current_url);
    }

    // If neither Content-Type nor the URL extension told us the format,
    // peek at the first audio bytes for an ADTS / MP3 sync header. This
    // is what catches the Shoutcast servers (radio88.hu:8400 and friends)
    // that respond with "ICY 200 OK" and no usable Content-Type while
    // actually streaming HE-AAC.
    size_t prefilled_len = 0;
    if (dec_type == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE) {
        int r0 = esp_http_client_read(client, (char *)in_buf, RADIO_HTTP_BUF_SIZE);
        if (r0 > 0) {
            prefilled_len = (size_t)r0;
            dec_type = sniff_audio_format(in_buf, prefilled_len);
            if (dec_type != ESP_AUDIO_SIMPLE_DEC_TYPE_NONE) {
                ESP_LOGI(TAG, "Stream-sniff picked %s",
                         esp_audio_simple_dec_get_name(dec_type));
            }
        }
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

    // ICY demultiplex state. The Shoutcast metadata format embeds a
    // single length byte every icy_metaint audio bytes; that byte ×16
    // gives the size of the following metadata block (often 0). We walk
    // the read buffer in place: audio bytes are compacted to the front
    // for the decoder, metadata bytes go into meta_buf for parsing.
    size_t audio_bytes_until_meta = icy_metaint;
    int meta_phase = 0;           // 0 = expecting length byte, 1 = collecting payload
    size_t meta_total = 0;
    size_t meta_collected = 0;

    bool id3_sniffed = false;

    while (!stop_requested) {
        int r;
        if (prefilled_len > 0) {
            // First iteration consumes the bytes we already read for the
            // format sniff above, so they aren't lost to the decoder.
            r = (int)prefilled_len;
            prefilled_len = 0;
        } else {
            r = esp_http_client_read(client, (char *)in_buf, RADIO_HTTP_BUF_SIZE);
        }
        if (r < 0) {
            set_error("HTTP read failed");
            radio_state = RADIO_STATE_ERROR;
            break;
        }
        if (r == 0) {
            ESP_LOGI(TAG, "Radio stream ended");
            break;
        }

        // Peek at the first read for an ID3v2 header. We do NOT change
        // the buffer — the decoder skips ID3 on its own — we just lift
        // out title/artist for the UI.
        if (!id3_sniffed) {
            radio_sniff_id3v2(in_buf, (size_t)r);
            id3_sniffed = true;
        }

        size_t audio_len = (size_t)r;
        if (icy_metaint > 0) {
            // Walk the just-read block. Most reads land entirely inside
            // one audio run, but a block can also contain a length byte,
            // a partial metadata block, or even the tail of a previous
            // metadata block followed by a new audio run.
            size_t read_pos = 0;
            size_t write_pos = 0;
            while (read_pos < (size_t)r) {
                if (audio_bytes_until_meta > 0) {
                    size_t take = (size_t)r - read_pos;
                    if (take > audio_bytes_until_meta) take = audio_bytes_until_meta;
                    if (write_pos != read_pos) {
                        memmove(in_buf + write_pos, in_buf + read_pos, take);
                    }
                    write_pos += take;
                    read_pos += take;
                    audio_bytes_until_meta -= take;
                } else if (meta_phase == 0) {
                    uint8_t len_byte = in_buf[read_pos++];
                    meta_total = (size_t)len_byte * 16;
                    meta_collected = 0;
                    if (meta_total == 0) {
                        // Empty metadata block — just rearm the counter.
                        audio_bytes_until_meta = icy_metaint;
                    } else {
                        meta_phase = 1;
                    }
                } else {
                    size_t avail = (size_t)r - read_pos;
                    size_t need = meta_total - meta_collected;
                    size_t take = avail < need ? avail : need;
                    if (meta_collected + take < 4095) {
                        memcpy(meta_buf + meta_collected, in_buf + read_pos, take);
                    }
                    meta_collected += take;
                    read_pos += take;
                    if (meta_collected == meta_total) {
                        size_t term = meta_total < 4095 ? meta_total : 4095;
                        meta_buf[term] = '\0';
                        radio_parse_icy_metadata((const char *)meta_buf);
                        audio_bytes_until_meta = icy_metaint;
                        meta_phase = 0;
                    }
                }
            }
            audio_len = write_pos;
        }

        esp_audio_simple_dec_raw_t raw = {
            .buffer = in_buf,
            .len = (uint32_t)audio_len,
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
                        // Dropped from LOG_I to LOG_D: at 3-second cadence
                        // this floods the serial console during long
                        // playbacks and there's no actionable info if the
                        // pcm size and queue depths are steady. Re-enable
                        // by raising the component log level if you ever
                        // need to debug decoder/queue behaviour.
                        ESP_LOGD(TAG, "Radio decoded: chunk=%lu pcm=%u free=%u play=%u",
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
    free(meta_buf);
    radio_clear_track_title();
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

    // The web UI might fire /api/radio/play moments after boot — earlier
    // than the audio path is up. Without this guard the playback task
    // hits a NULL I2S handle and the driver log explodes with one error
    // per chunk until init catches up.
    if (!mm_i2s_is_ready()) {
        set_error("Audio stack not initialized yet, please retry in a moment");
        ESP_LOGW(TAG, "media_radio_start rejected: I2S not ready");
        return ESP_ERR_INVALID_STATE;
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

    // Out-of-band metadata poller. We start it once and let it live for
    // the rest of the device's runtime — it detects URL changes itself
    // by re-reading current_url each iteration, so a station switch
    // doesn't require any task lifecycle juggling and the new title can
    // appear within the next poll cycle. Failing to start it is not
    // fatal — the audio path is what actually matters.
    if (!radio_meta_task_handle) {
        BaseType_t meta_ok = xTaskCreatePinnedToCore(radio_meta_poll_task,
                                                     "radio_meta", 6144, NULL, 4,
                                                     &radio_meta_task_handle, 0);
        if (meta_ok != pdPASS) {
            ESP_LOGW(TAG, "Metadata poll task launch failed; titles unavailable");
            radio_meta_task_handle = NULL;
        }
    }

    return ESP_OK;
}

esp_err_t media_radio_stop(void)
{
    stop_requested = true;
    // Note: we deliberately do NOT signal the metadata poll task here.
    // It stays alive across station changes and just idles when
    // radio_state isn't PLAYING — that's how a station switch picks up
    // the new title within one poll cycle without a tear-down delay.
    radio_amp_enable(false);
    radio_state = RADIO_STATE_STOPPED;
    radio_clear_track_title();
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

void media_radio_get_track_title(char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    // Updated only by the radio task between ICY parses; we accept a
    // benign race against a UI poll for the same reason as the WS
    // transcript copy — a torn read is at worst a truncated title.
    size_t len = strnlen(current_track_title, RADIO_TRACK_TITLE_SIZE);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, current_track_title, len);
    out[len] = '\0';
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
        // CRITICAL: media_radio_stop() only sets a flag. We must wait
        // for the radio task to actually exit before letting the voice
        // playback task take over the I2S TX channel — otherwise the
        // two tasks interleave frames into the same DMA and the
        // assistant's response audio comes out shredded. Wait up to
        // ~1 s; that's well above the 100 ms typical drain time but
        // bounded so a stuck radio task can't hang the wake event.
        for (int i = 0; i < 40 && radio_task_handle; i++) {
            vTaskDelay(pdMS_TO_TICKS(25));
        }
        if (radio_task_handle) {
            ESP_LOGW(TAG, "Radio task still running after pause grace period — voice may glitch");
        }
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
