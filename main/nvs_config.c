/**
 * NVS Configuration Storage - Implementation
 */

#include "nvs_config.h"
#include "app_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>

static const char *TAG = "nvs_cfg";
static const char *LEGACY_DEFAULT_API_URL = "wss://api.openai.com/v1/realtime?model=gpt-realtime";

static void normalize_device_name(char *name, size_t size)
{
    char input[33];
    strncpy(input, name && name[0] ? name : "", sizeof(input) - 1);
    input[sizeof(input) - 1] = '\0';

    const char *suffix = input;
    if (strncasecmp(input, DEFAULT_DEVICE_NAME, strlen(DEFAULT_DEVICE_NAME)) == 0) {
        suffix = input + strlen(DEFAULT_DEVICE_NAME);
        while (*suffix == ' ' || *suffix == '-' || *suffix == '_' || *suffix == ':') {
            suffix++;
        }
    }

    if (suffix[0] == '\0') {
        strncpy(name, DEFAULT_DEVICE_NAME, size - 1);
    } else {
        snprintf(name, size, "%s %s", DEFAULT_DEVICE_NAME, suffix);
    }
    name[size - 1] = '\0';
}

static bool is_valid_eq_profile(const char *profile)
{
    static const char *const valid[] = {
        "Flat", "Rock", "Pop", "Voice", "Clear", "Loudness", "Jazz",
    };
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        if (strcmp(profile, valid[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool is_valid_realtime_voice(const char *voice)
{
    static const char *const valid[] = {
        "alloy", "ash", "ballad", "coral", "echo",
        "sage", "shimmer", "verse", "marin", "cedar",
    };
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        if (strcmp(voice, valid[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool is_valid_conversation_style(const char *style)
{
    static const char *const valid[] = {
        "default", "professional", "friendly", "honest",
        "quirky", "efficient", "cynical",
    };
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        if (strcmp(style, valid[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool is_valid_ui_language(const char *language)
{
    static const char *const valid[] = {
        "en", "de", "es", "pt", "it", "pl", "hu",
    };
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        if (strcmp(language, valid[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool is_valid_wakeword_sensitivity(const char *sensitivity)
{
    static const char *const valid[] = {
        "slight", "moderate", "very",
    };
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        if (strcmp(sensitivity, valid[i]) == 0) {
            return true;
        }
    }
    return false;
}

esp_err_t nvs_config_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

esp_err_t nvs_config_load(micimike_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "NVS namespace not found, using defaults");
        memset(cfg, 0, sizeof(*cfg));
        strncpy(cfg->device_name, DEFAULT_DEVICE_NAME, sizeof(cfg->device_name) - 1);
        cfg->device_name[sizeof(cfg->device_name) - 1] = '\0';
        strncpy(cfg->wakeword, DEFAULT_WAKEWORD, sizeof(cfg->wakeword) - 1);
        cfg->wakeword[sizeof(cfg->wakeword) - 1] = '\0';
        strncpy(cfg->wakeword_sensitivity, DEFAULT_WW_SENSITIVITY, sizeof(cfg->wakeword_sensitivity) - 1);
        cfg->wakeword_sensitivity[sizeof(cfg->wakeword_sensitivity) - 1] = '\0';
        strncpy(cfg->api_url, DEFAULT_API_URL, sizeof(cfg->api_url) - 1);
        cfg->api_url[sizeof(cfg->api_url) - 1] = '\0';
        strncpy(cfg->eq_profile, DEFAULT_EQ_PROFILE, sizeof(cfg->eq_profile) - 1);
        cfg->eq_profile[sizeof(cfg->eq_profile) - 1] = '\0';
        strncpy(cfg->realtime_voice, DEFAULT_REALTIME_VOICE, sizeof(cfg->realtime_voice) - 1);
        cfg->realtime_voice[sizeof(cfg->realtime_voice) - 1] = '\0';
        strncpy(cfg->conversation_style, DEFAULT_CONV_STYLE, sizeof(cfg->conversation_style) - 1);
        cfg->conversation_style[sizeof(cfg->conversation_style) - 1] = '\0';
        strncpy(cfg->ui_language, DEFAULT_UI_LANGUAGE, sizeof(cfg->ui_language) - 1);
        cfg->ui_language[sizeof(cfg->ui_language) - 1] = '\0';
        strncpy(cfg->system_prompt, DEFAULT_SYSTEM_PROMPT, sizeof(cfg->system_prompt) - 1);
        cfg->system_prompt[sizeof(cfg->system_prompt) - 1] = '\0';
        cfg->volume = DEFAULT_VOLUME;
        cfg->session_timeout_s = DEFAULT_SESSION_IDLE_TIMEOUT_S;
        return ESP_ERR_NOT_FOUND;
    }

    size_t len;

    len = sizeof(cfg->wifi_ssid);
    nvs_get_str(h, NVS_KEY_WIFI_SSID, cfg->wifi_ssid, &len);

    len = sizeof(cfg->wifi_pass);
    nvs_get_str(h, NVS_KEY_WIFI_PASS, cfg->wifi_pass, &len);

    len = sizeof(cfg->device_name);
    if (nvs_get_str(h, NVS_KEY_DEVICE_NAME, cfg->device_name, &len) != ESP_OK ||
        cfg->device_name[0] == '\0') {
        strncpy(cfg->device_name, DEFAULT_DEVICE_NAME, sizeof(cfg->device_name) - 1);
        cfg->device_name[sizeof(cfg->device_name) - 1] = '\0';
    }
    cfg->device_name[sizeof(cfg->device_name) - 1] = '\0';
    normalize_device_name(cfg->device_name, sizeof(cfg->device_name));

    len = sizeof(cfg->api_key);
    if (nvs_get_str(h, NVS_KEY_API_KEY, cfg->api_key, &len) != ESP_OK) {
        cfg->api_key[0] = '\0';
    }
    cfg->api_key[sizeof(cfg->api_key) - 1] = '\0';

    len = sizeof(cfg->admin_api_key);
    if (nvs_get_str(h, NVS_KEY_ADMIN_API_KEY, cfg->admin_api_key, &len) != ESP_OK) {
        cfg->admin_api_key[0] = '\0';
    }
    cfg->admin_api_key[sizeof(cfg->admin_api_key) - 1] = '\0';

    len = sizeof(cfg->api_url);
    if (nvs_get_str(h, NVS_KEY_API_URL, cfg->api_url, &len) != ESP_OK) {
        strncpy(cfg->api_url, DEFAULT_API_URL, sizeof(cfg->api_url) - 1);
        cfg->api_url[sizeof(cfg->api_url) - 1] = '\0';
    }
    if (strcmp(cfg->api_url, LEGACY_DEFAULT_API_URL) == 0) {
        strncpy(cfg->api_url, DEFAULT_API_URL, sizeof(cfg->api_url) - 1);
        cfg->api_url[sizeof(cfg->api_url) - 1] = '\0';
        ESP_LOGI(TAG, "Migrated default API URL to %s", cfg->api_url);
    }

    len = sizeof(cfg->wakeword);
    if (nvs_get_str(h, NVS_KEY_WAKEWORD, cfg->wakeword, &len) != ESP_OK) {
        strncpy(cfg->wakeword, DEFAULT_WAKEWORD, sizeof(cfg->wakeword) - 1);
        cfg->wakeword[sizeof(cfg->wakeword) - 1] = '\0';
    }

    len = sizeof(cfg->wakeword_sensitivity);
    if (nvs_get_str(h, NVS_KEY_WW_SENSITIVITY, cfg->wakeword_sensitivity, &len) != ESP_OK ||
        cfg->wakeword_sensitivity[0] == '\0' || !is_valid_wakeword_sensitivity(cfg->wakeword_sensitivity)) {
        strncpy(cfg->wakeword_sensitivity, DEFAULT_WW_SENSITIVITY, sizeof(cfg->wakeword_sensitivity) - 1);
        cfg->wakeword_sensitivity[sizeof(cfg->wakeword_sensitivity) - 1] = '\0';
    }
    cfg->wakeword_sensitivity[sizeof(cfg->wakeword_sensitivity) - 1] = '\0';

    len = sizeof(cfg->radio_url);
    if (nvs_get_str(h, NVS_KEY_RADIO_URL, cfg->radio_url, &len) != ESP_OK) {
        cfg->radio_url[0] = '\0';
    }
    cfg->radio_url[sizeof(cfg->radio_url) - 1] = '\0';

    // Radio stations: blob of packed radio_station_t array.
    cfg->radio_station_count = 0;
    cfg->radio_current_index = -1;
    size_t blob_len = sizeof(cfg->radio_stations);
    if (nvs_get_blob(h, NVS_KEY_RADIO_STATIONS, cfg->radio_stations, &blob_len) == ESP_OK) {
        uint8_t count = (uint8_t)(blob_len / sizeof(radio_station_t));
        if (count > MAX_RADIO_STATIONS) count = MAX_RADIO_STATIONS;
        cfg->radio_station_count = count;
    }

    int8_t cur_idx = -1;
    if (nvs_get_i8(h, NVS_KEY_RADIO_CUR_IDX, &cur_idx) == ESP_OK) {
        if (cur_idx >= 0 && cur_idx < (int8_t)cfg->radio_station_count) {
            cfg->radio_current_index = cur_idx;
        }
    }

    // Migration: if there are no stations yet but the legacy single radio_url
    // is populated, seed the list with one entry so the user keeps their URL.
    if (cfg->radio_station_count == 0 && cfg->radio_url[0] != '\0') {
        strncpy(cfg->radio_stations[0].name, "Saved", sizeof(cfg->radio_stations[0].name) - 1);
        cfg->radio_stations[0].name[sizeof(cfg->radio_stations[0].name) - 1] = '\0';
        strncpy(cfg->radio_stations[0].url, cfg->radio_url, sizeof(cfg->radio_stations[0].url) - 1);
        cfg->radio_stations[0].url[sizeof(cfg->radio_stations[0].url) - 1] = '\0';
        cfg->radio_station_count = 1;
        cfg->radio_current_index = 0;
        ESP_LOGI(TAG, "Migrated legacy radio_url into station[0]");
    }

    len = sizeof(cfg->eq_profile);
    if (nvs_get_str(h, NVS_KEY_EQ_PROFILE, cfg->eq_profile, &len) != ESP_OK ||
        cfg->eq_profile[0] == '\0' || !is_valid_eq_profile(cfg->eq_profile)) {
        strncpy(cfg->eq_profile, DEFAULT_EQ_PROFILE, sizeof(cfg->eq_profile) - 1);
        cfg->eq_profile[sizeof(cfg->eq_profile) - 1] = '\0';
    }
    cfg->eq_profile[sizeof(cfg->eq_profile) - 1] = '\0';

    len = sizeof(cfg->realtime_voice);
    if (nvs_get_str(h, NVS_KEY_RT_VOICE, cfg->realtime_voice, &len) != ESP_OK ||
        cfg->realtime_voice[0] == '\0' || !is_valid_realtime_voice(cfg->realtime_voice)) {
        strncpy(cfg->realtime_voice, DEFAULT_REALTIME_VOICE, sizeof(cfg->realtime_voice) - 1);
        cfg->realtime_voice[sizeof(cfg->realtime_voice) - 1] = '\0';
    }
    cfg->realtime_voice[sizeof(cfg->realtime_voice) - 1] = '\0';

    len = sizeof(cfg->conversation_style);
    if (nvs_get_str(h, NVS_KEY_CONV_STYLE, cfg->conversation_style, &len) != ESP_OK ||
        cfg->conversation_style[0] == '\0' || !is_valid_conversation_style(cfg->conversation_style)) {
        strncpy(cfg->conversation_style, DEFAULT_CONV_STYLE, sizeof(cfg->conversation_style) - 1);
        cfg->conversation_style[sizeof(cfg->conversation_style) - 1] = '\0';
    }
    cfg->conversation_style[sizeof(cfg->conversation_style) - 1] = '\0';

    len = sizeof(cfg->ui_language);
    if (nvs_get_str(h, NVS_KEY_UI_LANGUAGE, cfg->ui_language, &len) != ESP_OK ||
        cfg->ui_language[0] == '\0' || !is_valid_ui_language(cfg->ui_language)) {
        strncpy(cfg->ui_language, DEFAULT_UI_LANGUAGE, sizeof(cfg->ui_language) - 1);
        cfg->ui_language[sizeof(cfg->ui_language) - 1] = '\0';
    }
    cfg->ui_language[sizeof(cfg->ui_language) - 1] = '\0';

    len = sizeof(cfg->system_prompt);
    if (nvs_get_str(h, NVS_KEY_SYSTEM_PROMPT, cfg->system_prompt, &len) != ESP_OK) {
        strncpy(cfg->system_prompt, DEFAULT_SYSTEM_PROMPT, sizeof(cfg->system_prompt) - 1);
        cfg->system_prompt[sizeof(cfg->system_prompt) - 1] = '\0';
    }
    cfg->system_prompt[sizeof(cfg->system_prompt) - 1] = '\0';

    if (nvs_get_u8(h, NVS_KEY_VOLUME, &cfg->volume) != ESP_OK) {
        cfg->volume = DEFAULT_VOLUME;
    }

    if (nvs_get_u16(h, NVS_KEY_SESSION_TIMEOUT, &cfg->session_timeout_s) != ESP_OK) {
        cfg->session_timeout_s = DEFAULT_SESSION_IDLE_TIMEOUT_S;
    }
    if (cfg->session_timeout_s < 3 || cfg->session_timeout_s > 300) {
        cfg->session_timeout_s = DEFAULT_SESSION_IDLE_TIMEOUT_S;
    }

    nvs_close(h);
    ESP_LOGI(TAG, "Config loaded: name=%s SSID=%s wakeword=%s sens=%s vol=%d eq=%s voice=%s style=%s ui_lang=%s prompt_len=%u session_timeout=%us api_key_len=%u api_url=%s",
             cfg->device_name, cfg->wifi_ssid, cfg->wakeword, cfg->wakeword_sensitivity, cfg->volume, cfg->eq_profile,
             cfg->realtime_voice, cfg->conversation_style, cfg->ui_language,
             (unsigned)strlen(cfg->system_prompt),
             cfg->session_timeout_s, (unsigned)strlen(cfg->api_key), cfg->api_url);
    return ESP_OK;
}

esp_err_t nvs_config_save(const micimike_config_t *cfg)
{
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h));

    nvs_set_str(h, NVS_KEY_WIFI_SSID, cfg->wifi_ssid);
    nvs_set_str(h, NVS_KEY_WIFI_PASS, cfg->wifi_pass);
    nvs_set_str(h, NVS_KEY_DEVICE_NAME, cfg->device_name);
    nvs_set_str(h, NVS_KEY_API_KEY, cfg->api_key);
    nvs_set_str(h, NVS_KEY_ADMIN_API_KEY, cfg->admin_api_key);
    nvs_set_str(h, NVS_KEY_API_URL, cfg->api_url);
    nvs_set_str(h, NVS_KEY_RADIO_URL, cfg->radio_url);
    {
        uint8_t count = cfg->radio_station_count;
        if (count > MAX_RADIO_STATIONS) count = MAX_RADIO_STATIONS;
        nvs_set_blob(h, NVS_KEY_RADIO_STATIONS, cfg->radio_stations,
                     count * sizeof(radio_station_t));
        int8_t cur_idx = cfg->radio_current_index;
        if (cur_idx >= (int8_t)count) cur_idx = -1;
        nvs_set_i8(h, NVS_KEY_RADIO_CUR_IDX, cur_idx);
    }
    nvs_set_str(h, NVS_KEY_EQ_PROFILE, cfg->eq_profile);
    nvs_set_str(h, NVS_KEY_RT_VOICE, cfg->realtime_voice);
    nvs_set_str(h, NVS_KEY_CONV_STYLE, cfg->conversation_style);
    nvs_set_str(h, NVS_KEY_UI_LANGUAGE, cfg->ui_language);
    nvs_set_str(h, NVS_KEY_SYSTEM_PROMPT, cfg->system_prompt);
    nvs_set_str(h, NVS_KEY_WAKEWORD, cfg->wakeword);
    nvs_set_str(h, NVS_KEY_WW_SENSITIVITY, cfg->wakeword_sensitivity);
    nvs_set_u8(h, NVS_KEY_VOLUME, cfg->volume);
    nvs_set_u16(h, NVS_KEY_SESSION_TIMEOUT, cfg->session_timeout_s);

    esp_err_t ret = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Config saved");
    return ret;
}

esp_err_t nvs_config_clear(void)
{
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h));
    esp_err_t ret = nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Config cleared");
    return ret;
}

bool nvs_config_is_provisioned(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;

    char ssid[33] = {0};
    size_t len = sizeof(ssid);
    esp_err_t ret = nvs_get_str(h, NVS_KEY_WIFI_SSID, ssid, &len);
    nvs_close(h);

    return (ret == ESP_OK && strlen(ssid) > 0);
}
