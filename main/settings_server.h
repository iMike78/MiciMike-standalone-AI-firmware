/**
 * Settings Web Server
 *
 * Runs on the local network (STA mode) after WiFi provisioning.
 * Provides a web UI for:
 *   - API key and endpoint configuration (copy-paste friendly)
 *   - Wake word selection
 *   - Volume control
 *   - Internet radio URLs (future)
 *   - Sendspin enable/disable (future)
 *   - Factory reset
 */

#pragma once
#include "esp_err.h"
#include "nvs_config.h"
#include <stdbool.h>
#include <stdint.h>

#define SETTINGS_CHANGED_API        (1u << 0)
#define SETTINGS_CHANGED_WAKEWORD   (1u << 1)
#define SETTINGS_CHANGED_WW_SENS    (1u << 2)
#define SETTINGS_CHANGED_VOICE      (1u << 3)
#define SETTINGS_CHANGED_STYLE      (1u << 4)
#define SETTINGS_CHANGED_PROMPT     (1u << 5)
#define SETTINGS_CHANGED_TIMEOUT    (1u << 6)
#define SETTINGS_CHANGED_SNAPCAST   (1u << 7)

typedef void (*settings_changed_cb_t)(uint32_t changed_mask);

esp_err_t settings_server_start(micimike_config_t *cfg, settings_changed_cb_t changed_cb);
void settings_server_set_runtime_wakeword(const char *active_wakeword, bool reboot_required);
esp_err_t settings_server_stop(void);
