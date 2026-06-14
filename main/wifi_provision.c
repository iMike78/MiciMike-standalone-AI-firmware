/**
 * WiFi Provisioning - Captive Portal Implementation
 *
 * Runs an HTTP server on the AP interface that serves a setup form
 * and handles DNS hijacking for captive portal detection.
 */

#include "wifi_provision.h"
#include "app_config.h"
#include "nvs_config.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "lwip/inet.h"
// DNS server not implemented yet — captive portal relies on HTTP redirects
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "wifi_prov";
static httpd_handle_t server = NULL;
static EventGroupHandle_t wifi_events;
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static void make_hostname(const char *name, char *out, size_t out_size)
{
    size_t pos = 0;
    const char *src = (name && name[0]) ? name : DEFAULT_DEVICE_NAME;

    for (size_t i = 0; src[i] && pos + 1 < out_size; i++) {
        char c = src[i];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out[pos++] = c;
        } else if (c >= 'A' && c <= 'Z') {
            out[pos++] = (char)(c - 'A' + 'a');
        } else if (pos > 0 && out[pos - 1] != '-') {
            out[pos++] = '-';
        }
    }

    while (pos > 0 && out[pos - 1] == '-') pos--;
    if (pos == 0) {
        strncpy(out, "micimike", out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }
    out[pos] = '\0';
}

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

// ---------------------------------------------------------------------------
// Captive portal HTML (embedded, minimal)
// ---------------------------------------------------------------------------
static const char SETUP_HTML[] =
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>MiciMike Setup</title>"
    "<style>"
    "body{font-family:sans-serif;background:#1a1a2e;color:#e0e0e0;margin:0;padding:20px;}"
    ".c{max-width:400px;margin:0 auto;}"
    "h1{color:#00d4aa;font-size:1.4em;}"
    "label{display:block;margin:12px 0 4px;font-size:0.9em;}"
    "input,select{width:100%;padding:10px;border:1px solid #333;border-radius:6px;"
    "background:#16213e;color:#e0e0e0;font-size:1em;box-sizing:border-box;}"
    "button{width:100%;padding:12px;margin-top:20px;border:none;border-radius:6px;"
    "background:#00d4aa;color:#1a1a2e;font-size:1.1em;font-weight:bold;cursor:pointer;}"
    "button:hover{background:#00b894;}"
    ".info{font-size:0.8em;color:#888;margin-top:4px;}"
    "</style></head><body><div class='c'>"
    "<h1>&#9881; MiciMike Setup</h1>"
    "<p>Step 1: Connect to your WiFi network.</p>"
    "<label>Device Suffix</label>"
    "<input name='device' id='device' placeholder='Kitchen' maxlength='24'>"
    "<label>WiFi Network (SSID)</label>"
    "<input name='ssid' id='ssid' required>"
    "<label>WiFi Password</label>"
    "<input name='pass' id='pass' type='password'>"
    "<label>Wake Word</label>"
    "<select name='wakeword' id='wakeword'>"
    "<option value='okay_nabu'>Okay Nabu</option>"
    "<option value='hey_jarvis'>Hey Jarvis</option>"
    "<option value='hey_mycroft'>Hey Mycroft</option>"
    "<option value='alexa'>Alexa</option>"
    "</select>"
    "<button onclick='submit()'>Save &amp; Connect</button>"
    "<p class='info'>After connecting, open the device's IP address in your browser to configure API key and other settings.</p>"
    "<script>"
    "function submit(){"
    "var d={device_name:document.getElementById('device').value,"
    "ssid:document.getElementById('ssid').value,"
    "pass:document.getElementById('pass').value,"
    "wakeword:document.getElementById('wakeword').value};"
    "fetch('/save',{method:'POST',headers:{'Content-Type':'application/json'},"
    "body:JSON.stringify(d)}).then(r=>r.text()).then(t=>{"
    "document.body.innerHTML='<div class=\"c\"><h1>'+t+'</h1><p>Device will reboot. Then open its IP in your browser to set up API key.</p></div>';"
    "}).catch(e=>alert('Error: '+e));"
    "}"
    "</script>"
    "</div></body></html>";

// ---------------------------------------------------------------------------
// HTTP handlers
// ---------------------------------------------------------------------------
static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, SETUP_HTML, sizeof(SETUP_HTML) - 1);
}

// Captive portal detection endpoints
static esp_err_t captive_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t save_handler(httpd_req_t *req)
{
    char buf[768];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    // Simple JSON parsing (no external dependency)
    // TODO: Use cJSON component for robust parsing
    micimike_config_t cfg = {0};

    // Extract fields from JSON (minimal parser)
    char *p;
    p = strstr(buf, "\"ssid\":\"");
    if (p) sscanf(p, "\"ssid\":\"%32[^\"]\"", cfg.wifi_ssid);

    p = strstr(buf, "\"device_name\":\"");
    if (p) sscanf(p, "\"device_name\":\"%32[^\"]\"", cfg.device_name);

    p = strstr(buf, "\"pass\":\"");
    if (p) sscanf(p, "\"pass\":\"%64[^\"]\"", cfg.wifi_pass);

    p = strstr(buf, "\"wakeword\":\"");
    if (p) sscanf(p, "\"wakeword\":\"%31[^\"]\"", cfg.wakeword);
    strncpy(cfg.wakeword_sensitivity, DEFAULT_WW_SENSITIVITY, sizeof(cfg.wakeword_sensitivity) - 1);
    cfg.wakeword_sensitivity[sizeof(cfg.wakeword_sensitivity) - 1] = '\0';

    if (strlen(cfg.device_name) == 0) {
        strncpy(cfg.device_name, DEFAULT_DEVICE_NAME, sizeof(cfg.device_name) - 1);
        cfg.device_name[sizeof(cfg.device_name) - 1] = '\0';
    }
    normalize_device_name(cfg.device_name, sizeof(cfg.device_name));

    cfg.volume = DEFAULT_VOLUME;
    cfg.session_timeout_s = DEFAULT_SESSION_IDLE_TIMEOUT_S;
    strncpy(cfg.eq_profile, DEFAULT_EQ_PROFILE, sizeof(cfg.eq_profile) - 1);
    cfg.eq_profile[sizeof(cfg.eq_profile) - 1] = '\0';
    strncpy(cfg.realtime_voice, DEFAULT_REALTIME_VOICE, sizeof(cfg.realtime_voice) - 1);
    cfg.realtime_voice[sizeof(cfg.realtime_voice) - 1] = '\0';
    strncpy(cfg.conversation_style, DEFAULT_CONV_STYLE, sizeof(cfg.conversation_style) - 1);
    cfg.conversation_style[sizeof(cfg.conversation_style) - 1] = '\0';
    strncpy(cfg.ui_language, DEFAULT_UI_LANGUAGE, sizeof(cfg.ui_language) - 1);
    cfg.ui_language[sizeof(cfg.ui_language) - 1] = '\0';
    strncpy(cfg.system_prompt, DEFAULT_SYSTEM_PROMPT, sizeof(cfg.system_prompt) - 1);
    cfg.system_prompt[sizeof(cfg.system_prompt) - 1] = '\0';
    strncpy(cfg.api_url, DEFAULT_API_URL, sizeof(cfg.api_url) - 1);
    cfg.api_url[sizeof(cfg.api_url) - 1] = '\0';

    if (strlen(cfg.wifi_ssid) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
        return ESP_FAIL;
    }

    nvs_config_save(&cfg);
    httpd_resp_sendstr(req, "&#10003; Settings saved!");

    ESP_LOGI(TAG, "Config saved, rebooting in 2s...");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();

    return ESP_OK;
}

// ---------------------------------------------------------------------------
// AP + HTTP server
// ---------------------------------------------------------------------------
esp_err_t wifi_provision_start(void)
{
    ESP_LOGI(TAG, "Starting captive portal AP: %s", CAPTIVE_PORTAL_SSID);

    // Init netif and event loop (may already be done)
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

    wifi_config_t ap_config = {
        .ap = {
            .ssid = CAPTIVE_PORTAL_SSID,
            .ssid_len = strlen(CAPTIVE_PORTAL_SSID),
            .channel = 1,
            .authmode = WIFI_AUTH_OPEN,
            .max_connection = 2,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_max_tx_power(32);  // 8dBm — matches board RF design

    // Start HTTP server
    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.max_uri_handlers = 10;
    http_cfg.stack_size = 8192;
    ESP_ERROR_CHECK(httpd_start(&server, &http_cfg));

    // Main page
    httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_handler };
    httpd_register_uri_handler(server, &root);

    // Save endpoint
    httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = save_handler };
    httpd_register_uri_handler(server, &save);

    // Captive portal detection redirects
    const char *captive_paths[] = {
        "/generate_204", "/gen_204", "/hotspot-detect.html",
        "/connecttest.txt", "/redirect", "/canonical.html",
        "/success.txt", "/ncsi.txt", NULL
    };
    for (int i = 0; captive_paths[i]; i++) {
        httpd_uri_t cp = { .uri = captive_paths[i], .method = HTTP_GET, .handler = captive_handler };
        httpd_register_uri_handler(server, &cp);
    }

    // TODO: Start DNS server that resolves all domains to 192.168.4.1
    // This ensures the captive portal popup triggers on all platforms.
    // For MVP, most devices will auto-detect via the HTTP redirects above.

    ESP_LOGI(TAG, "Captive portal running at http://192.168.4.1/");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Station mode connection
// ---------------------------------------------------------------------------
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected, retrying...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_station_connect(const micimike_config_t *cfg)
{
    wifi_events = xEventGroupCreate();

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    if (sta_netif && cfg->device_name[0] != '\0') {
        char hostname[33];
        make_hostname(cfg->device_name, hostname, sizeof(hostname));
        esp_netif_set_hostname(sta_netif, hostname);
    }

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

    esp_event_handler_instance_t any_id, got_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &got_ip);

    wifi_config_t sta_config = {0};
    strncpy((char *)sta_config.sta.ssid, cfg->wifi_ssid, sizeof(sta_config.sta.ssid) - 1);
    strncpy((char *)sta_config.sta.password, cfg->wifi_pass, sizeof(sta_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_max_tx_power(32);  // 8dBm — matches board RF design
    ESP_ERROR_CHECK(esp_wifi_connect());

    ESP_LOGI(TAG, "Connecting to %s...", cfg->wifi_ssid);

    EventBits_t bits = xEventGroupWaitBits(wifi_events,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));

    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }

    ESP_LOGE(TAG, "WiFi connection timeout");
    return ESP_FAIL;
}

// ---------------------------------------------------------------------------
// SNTP time sync
// ---------------------------------------------------------------------------
static void sntp_sync_notify(struct timeval *tv)
{
    if (tv) {
        ESP_LOGI(TAG, "SNTP sync event: tv_sec=%lld", (long long)tv->tv_sec);
    }
}

esp_err_t wifi_sntp_sync(uint32_t timeout_ms)
{
    static bool sntp_started = false;

    if (!sntp_started) {
        // time.google.com is usually the fastest from EU; pool.ntp.org as fallback.
        // We only configure one server here (LWIP_SNTP_MAX_SERVERS=1 by default),
        // pool.ntp.org's RR DNS will give us a different host on retry anyway.
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("time.google.com");
        cfg.start = true;
        cfg.wait_for_sync = false;  // we poll ourselves below
        cfg.sync_cb = sntp_sync_notify;

        esp_err_t ret = esp_netif_sntp_init(&cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_netif_sntp_init failed: %s", esp_err_to_name(ret));
            return ret;
        }
        sntp_started = true;
        ESP_LOGI(TAG, "SNTP started (time.google.com)");
    }

    // Wait up to timeout_ms for first sync. ESP_NETIF_SNTP_DEFAULT_CONFIG sets
    // wait_for_sync = true normally, but here we drive it manually so we can
    // log progress and bail out cleanly.
    const TickType_t poll_interval = pdMS_TO_TICKS(500);
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline) {
        time_t now = 0;
        time(&now);
        if (now > 1700000000) {  // any timestamp after 2023-11-14 is "sane"
            struct tm tm_info;
            localtime_r(&now, &tm_info);
            char buf[32];
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_info);
            ESP_LOGI(TAG, "SNTP time synced: %s UTC", buf);
            return ESP_OK;
        }
        vTaskDelay(poll_interval);
    }

    ESP_LOGW(TAG, "SNTP sync timeout after %u ms — TLS may fail until clock is set",
             (unsigned)timeout_ms);
    return ESP_ERR_TIMEOUT;
}
