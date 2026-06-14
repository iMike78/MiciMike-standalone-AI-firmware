/**
 * WiFi Provisioning - Captive Portal
 *
 * Flow:
 *   1. ESP32-S3 starts AP: "MiciMike-Setup"
 *   2. Client connects → captive portal auto-opens
 *   3. User enters: WiFi SSID/pass, API key, wakeword selection, API URL
 *   4. Settings saved to NVS → reboot into station mode
 */

#pragma once
#include "esp_err.h"
#include "nvs_config.h"

/**
 * Start captive portal AP mode.
 * Blocks until user submits config or timeout.
 * On success, config is saved to NVS and device reboots.
 */
esp_err_t wifi_provision_start(void);

/**
 * Connect to WiFi using stored credentials.
 * Returns ESP_OK when connected, ESP_FAIL on timeout.
 */
esp_err_t wifi_station_connect(const micimike_config_t *cfg);

/**
 * Sync system clock via SNTP. Required before any TLS connection
 * (mbedtls expiry / not-yet-valid checks need a real wall clock).
 *
 * Blocks up to `timeout_ms` milliseconds waiting for first sync.
 * Returns ESP_OK on success, ESP_ERR_TIMEOUT if no SNTP response in time.
 */
esp_err_t wifi_sntp_sync(uint32_t timeout_ms);
