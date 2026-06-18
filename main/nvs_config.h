/**
 * NVS Configuration Storage
 *
 * Stores user settings: WiFi credentials, API key, wakeword selection, volume.
 */

#pragma once
#include "esp_err.h"
#include <stdint.h>

#define MAX_RADIO_STATIONS 8

typedef struct {
    char name[32];
    char url[224];
} radio_station_t;

typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char device_name[33];
    char api_key[256];
    char admin_api_key[256];
    char api_url[256];
    char radio_url[256];                                // legacy single URL, only used for migration
    radio_station_t radio_stations[MAX_RADIO_STATIONS];
    uint8_t radio_station_count;
    int8_t  radio_current_index;                        // -1 if no current selection
    char eq_profile[16];
    char realtime_voice[16];
    char conversation_style[16];
    char ui_language[8];
    char system_prompt[1024];
    char wakeword[32];
    char wakeword_sensitivity[16];
    uint8_t volume;
    uint16_t session_timeout_s;

    // Snapcast / Sendspin client. host="" means use mDNS discovery
    // (_snapcast._tcp.). client_name is what shows up in the Snapcast
    // group UI / Music Assistant; defaults to device_name if empty.
    uint8_t  snapcast_enable;
    char     snapcast_host[64];
    uint16_t snapcast_port;
    char     snapcast_name[32];
} micimike_config_t;

esp_err_t nvs_config_init(void);
esp_err_t nvs_config_load(micimike_config_t *cfg);
esp_err_t nvs_config_save(const micimike_config_t *cfg);
esp_err_t nvs_config_clear(void);
bool nvs_config_is_provisioned(void);
