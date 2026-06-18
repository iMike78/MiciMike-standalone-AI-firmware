/**
 * Sendspin player wrapper — C++ glue around sendspin-cpp.
 *
 * The C++ client + listener live on a dedicated FreeRTOS task; the C API
 * only signals it via flags. In normal mode the MA-side Sendspin server
 * discovers us via mDNS and pulls. If an explicit server URL is configured,
 * we connect outbound and keep the local WS server disabled.
 */

#include "sendspin_iface.h"

#include "sendspin/client.h"
#include "sendspin/controller_role.h"
#include "sendspin/player_role.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "mdns.h"

#include <atomic>
#include <cstring>
#include <cstdio>
#include <string>
#include <utility>

namespace {

constexpr const char *TAG = "sendspin";
constexpr uint32_t  TASK_STACK_BYTES = 12 * 1024;
constexpr UBaseType_t TASK_PRIORITY  = 6;
constexpr BaseType_t  TASK_CORE      = 0;
constexpr uint32_t  LOOP_TICK_MS     = 10;
constexpr uint16_t  SENDSPIN_PORT    = 8928;
constexpr const char *SENDSPIN_PATH  = "/sendspin";
constexpr const char *SENDSPIN_SVC   = "_sendspin";
constexpr const char *SENDSPIN_PROTO = "_tcp";
constexpr uint32_t SENDSPIN_PCM_SAMPLE_RATE = 48000;
constexpr uint32_t SENDSPIN_PCM_CHANNELS = 2;
constexpr uint32_t SENDSPIN_PCM_BYTES_PER_SAMPLE = sizeof(int16_t);
constexpr uint32_t SENDSPIN_PCM_BYTES_PER_FRAME =
    SENDSPIN_PCM_CHANNELS * SENDSPIN_PCM_BYTES_PER_SAMPLE;

// Wrap PlayerRoleListener so we can forward PCM into the firmware-side
// callback while keeping the library's pointer-stability requirement.
class PcmForwarder final : public sendspin::PlayerRoleListener {
public:
    size_t on_audio_write(uint8_t *data, size_t length,
                          uint32_t timeout_ms) override
    {
        if (external_media_active_.load()) {
            return notify_consumed(length);
        }

        auto cb = pcm_cb_.load();
        size_t consumed = 0;
        if (!cb) {
            // Nowhere to send it yet — claim it was written so the library
            // doesn't pile up its internal buffer.
            consumed = length;
        }
        if (cb) {
            consumed = cb(data, length, timeout_ms, pcm_user_.load());
        }

        auto player = player_.load();
        if (player && consumed >= SENDSPIN_PCM_BYTES_PER_FRAME) {
            uint32_t frames = consumed / SENDSPIN_PCM_BYTES_PER_FRAME;
            int64_t duration_us =
                ((int64_t)frames * 1000000LL) / SENDSPIN_PCM_SAMPLE_RATE;
            player->notify_audio_played(frames, esp_timer_get_time() + duration_us);
        }

        return consumed;
    }

    void set_pcm_cb(sendspin_pcm_cb_t cb, void *user)
    {
        pcm_user_.store(user);
        pcm_cb_.store(cb);
    }

    void set_player(sendspin::PlayerRole *player)
    {
        player_.store(player);
    }

    void set_volume_cb(sendspin_volume_cb_t cb, void *user)
    {
        volume_user_.store(user);
        volume_cb_.store(cb);
    }

    void set_mute_cb(sendspin_mute_cb_t cb, void *user)
    {
        mute_user_.store(user);
        mute_cb_.store(cb);
    }

    void on_volume_changed(uint8_t volume) override
    {
        ESP_LOGI(TAG, "server volume -> %u", (unsigned)volume);
        auto cb = volume_cb_.load();
        if (cb) cb(volume, volume_user_.load());
    }

    void on_mute_changed(bool muted) override
    {
        ESP_LOGI(TAG, "server mute -> %s", muted ? "true" : "false");
        auto cb = mute_cb_.load();
        if (cb) cb(muted, mute_user_.load());
    }

    void on_stream_start() override
    {
        external_media_active_.store(false);
        ESP_LOGI(TAG, "stream start; Sendspin may take media output");
    }

    void external_media_started()
    {
        external_media_active_.store(true);
    }

private:
    size_t notify_consumed(size_t consumed)
    {
        auto player = player_.load();
        if (player && consumed >= SENDSPIN_PCM_BYTES_PER_FRAME) {
            uint32_t frames = consumed / SENDSPIN_PCM_BYTES_PER_FRAME;
            int64_t duration_us =
                ((int64_t)frames * 1000000LL) / SENDSPIN_PCM_SAMPLE_RATE;
            player->notify_audio_played(frames, esp_timer_get_time() + duration_us);
        }
        return consumed;
    }

    std::atomic<sendspin_pcm_cb_t> pcm_cb_{nullptr};
    std::atomic<void *>            pcm_user_{nullptr};
    std::atomic<sendspin::PlayerRole *> player_{nullptr};
    std::atomic<sendspin_volume_cb_t> volume_cb_{nullptr};
    std::atomic<void *>               volume_user_{nullptr};
    std::atomic<sendspin_mute_cb_t>   mute_cb_{nullptr};
    std::atomic<void *>               mute_user_{nullptr};
    std::atomic<bool>                 external_media_active_{false};
};

class AlwaysReadyNet final : public sendspin::SendspinNetworkProvider {
public:
    bool is_network_ready() override { return true; }
};

class ClientPerfListener final : public sendspin::SendspinClientListener {
public:
    void on_request_high_performance() override
    {
        if (!wifi_ps_disabled_.exchange(true)) {
            esp_wifi_set_ps(WIFI_PS_NONE);
            ESP_LOGI(TAG, "WiFi power save disabled for Sendspin");
        }
    }

    void on_release_high_performance() override
    {
        // This is a mains-powered speaker. Keeping WiFi PS off avoids latency
        // spikes between Sendspin time-sync bursts and sustained audio frames.
    }

private:
    std::atomic<bool> wifi_ps_disabled_{false};
};

struct Ctx {
    SemaphoreHandle_t mutex     = nullptr;
    TaskHandle_t      task      = nullptr;
    std::atomic<bool> stop_req{false};
    std::atomic<sendspin_state_t> state{SENDSPIN_STATE_STOPPED};
    char              last_error[160] = {0};

    sendspin_iface_config_t cfg{};

    PcmForwarder      pcm_forwarder{};
    AlwaysReadyNet    network{};
    ClientPerfListener perf_listener{};

    // Constructed inside the task to keep RAII tied to the task lifetime.
    sendspin::SendspinClient *client = nullptr;
    std::atomic<bool> controller_stop_req{false};
};

Ctx s_ctx;

void set_state(sendspin_state_t st)
{
    if (s_ctx.state.load() == st) return;
    s_ctx.state.store(st);
    ESP_LOGI(TAG, "state -> %d", static_cast<int>(st));
}

void set_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_ctx.last_error, sizeof(s_ctx.last_error), fmt, ap);
    va_end(ap);
    ESP_LOGE(TAG, "%s", s_ctx.last_error);
}

void ensure_mutex()
{
    if (!s_ctx.mutex) s_ctx.mutex = xSemaphoreCreateMutex();
}

void task_main(void *)
{
    // Keep streaming logs quiet; serial output from the websocket/audio path
    // is enough to disturb real-time playback on the ESP32-S3.
    esp_log_level_set("sendspin", ESP_LOG_INFO);
    esp_log_level_set("sendspin.*", ESP_LOG_INFO);
    esp_log_level_set("websocket_client", ESP_LOG_WARN);

    set_state(SENDSPIN_STATE_STARTING);

    sendspin::SendspinClientConfig cfg;
    cfg.client_id        = s_ctx.cfg.client_id;
    cfg.name             = s_ctx.cfg.name;
    cfg.product_name     = "Home Mini DiP";
    cfg.manufacturer     = "MRD";
    cfg.software_version = "0.6";
    cfg.websocket_priority = 10;
    // Match the ESPHome sendspin hub defaults: park the httpd task stack in
    // PSRAM to free internal RAM for the WS client + sync task chain.
    cfg.httpd_psram_stack = true;
    if (s_ctx.cfg.server_url[0]) {
        // Explicit-connect mode uses only an outbound WS connection. Do not
        // also expose the local WS server; MA may have a cached player URL and
        // otherwise reconnect inbound, replacing the same client_id.
        cfg.start_ws_server = false;
    }

    s_ctx.client = new sendspin::SendspinClient(std::move(cfg));

    sendspin::PlayerRoleConfig player_cfg;
    // ESP32-S3 decodes FLAC in software here. In practice the decode +
    // websocket receive path falls behind and MA starts buffering tens of
    // seconds ahead. Raw PCM costs more WiFi bandwidth, but keeps the ESP
    // path cheap and deterministic.
    player_cfg.audio_formats = {
        {sendspin::SendspinCodecFormat::PCM, 2, 48000, 16},
    };
    player_cfg.audio_buffer_capacity = 1024 * 1024;
    player_cfg.psram_stack = true;

    auto &player = s_ctx.client->add_player(std::move(player_cfg));
    player.update_volume(s_ctx.cfg.initial_volume > 100 ? 100 : s_ctx.cfg.initial_volume);
    player.set_listener(&s_ctx.pcm_forwarder);
    s_ctx.pcm_forwarder.set_player(&player);
    auto &controller = s_ctx.client->add_controller();
    s_ctx.client->set_network_provider(&s_ctx.network);
    s_ctx.client->set_listener(&s_ctx.perf_listener);

    if (!s_ctx.client->start_server()) {
        set_error("SendspinClient::start_server() failed");
        set_state(SENDSPIN_STATE_ERROR);
        delete s_ctx.client;
        s_ctx.client = nullptr;
        s_ctx.task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    // Advertise _sendspin._tcp. on mDNS so the Music Assistant Sendspin
    // server can discover us. The library does the WS server but leaves
    // mDNS to the application (the host-side basic_client example uses
    // dns_sd.h, which has no ESP-IDF equivalent — esp-idf mdns is it).
    bool mdns_registered = false;
    if (!s_ctx.cfg.server_url[0]) {
        mdns_txt_item_t txt[] = {
            {"path", SENDSPIN_PATH},
            {"name", s_ctx.cfg.name},
        };
        esp_err_t mret = mdns_service_add(s_ctx.cfg.name,
                                          SENDSPIN_SVC, SENDSPIN_PROTO,
                                          SENDSPIN_PORT,
                                          txt, sizeof(txt) / sizeof(txt[0]));
        if (mret != ESP_OK) {
            ESP_LOGW(TAG, "mdns_service_add failed: %s — MA discovery will not work",
                     esp_err_to_name(mret));
        } else {
            ESP_LOGI(TAG, "mDNS service _sendspin._tcp. registered on port %u",
                     (unsigned)SENDSPIN_PORT);
            mdns_registered = true;
        }
    } else {
        ESP_LOGI(TAG, "Explicit server URL set; skipping local WS server and mDNS advertisement");
    }

    set_state(SENDSPIN_STATE_LISTENING);
    ESP_LOGI(TAG, "Sendspin player advertising as '%s' (id=%s)",
             s_ctx.cfg.name, s_ctx.cfg.client_id);

    if (s_ctx.cfg.server_url[0]) {
        ESP_LOGI(TAG, "Manual connect to %s", s_ctx.cfg.server_url);
        s_ctx.client->connect_to(s_ctx.cfg.server_url);
    }

    while (!s_ctx.stop_req.load()) {
        s_ctx.client->loop();
        if (s_ctx.controller_stop_req.exchange(false)) {
            ESP_LOGI(TAG, "Sending Sendspin controller STOP for local media");
            controller.send_command(sendspin::SendspinControllerCommand::STOP);
        }
        vTaskDelay(pdMS_TO_TICKS(LOOP_TICK_MS));
    }

    if (mdns_registered) {
        mdns_service_remove(SENDSPIN_SVC, SENDSPIN_PROTO);
    }

    s_ctx.client->disconnect(sendspin::SendspinGoodbyeReason::SHUTDOWN);
    s_ctx.pcm_forwarder.set_player(nullptr);
    delete s_ctx.client;
    s_ctx.client = nullptr;

    set_state(SENDSPIN_STATE_STOPPED);
    s_ctx.task = nullptr;
    vTaskDelete(nullptr);
}

}  // namespace

extern "C" {

void sendspin_iface_set_pcm_cb(sendspin_pcm_cb_t cb, void *user)
{
    s_ctx.pcm_forwarder.set_pcm_cb(cb, user);
}

void sendspin_iface_set_volume_cb(sendspin_volume_cb_t cb, void *user)
{
    s_ctx.pcm_forwarder.set_volume_cb(cb, user);
}

void sendspin_iface_set_mute_cb(sendspin_mute_cb_t cb, void *user)
{
    s_ctx.pcm_forwarder.set_mute_cb(cb, user);
}

void sendspin_iface_external_media_started(void)
{
    s_ctx.pcm_forwarder.external_media_started();
    s_ctx.controller_stop_req.store(true);
}

esp_err_t sendspin_iface_start(const sendspin_iface_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    ensure_mutex();
    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);

    if (s_ctx.task) {
        s_ctx.stop_req.store(true);
        for (int i = 0; i < 100 && s_ctx.task; i++) {
            xSemaphoreGive(s_ctx.mutex);
            vTaskDelay(pdMS_TO_TICKS(50));
            xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
        }
    }

    s_ctx.cfg = *cfg;
    s_ctx.last_error[0] = '\0';
    s_ctx.stop_req.store(false);

    BaseType_t ok = xTaskCreatePinnedToCore(task_main, "sendspin",
                                            TASK_STACK_BYTES, nullptr,
                                            TASK_PRIORITY, &s_ctx.task,
                                            TASK_CORE);
    if (ok != pdPASS) {
        s_ctx.task = nullptr;
        set_state(SENDSPIN_STATE_ERROR);
        set_error("Failed to create sendspin task");
        xSemaphoreGive(s_ctx.mutex);
        return ESP_FAIL;
    }

    xSemaphoreGive(s_ctx.mutex);
    return ESP_OK;
}

esp_err_t sendspin_iface_stop(void)
{
    ensure_mutex();
    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    if (s_ctx.task) {
        s_ctx.stop_req.store(true);
    }
    xSemaphoreGive(s_ctx.mutex);
    return ESP_OK;
}

sendspin_state_t sendspin_iface_get_state(void)
{
    return s_ctx.state.load();
}

const char *sendspin_iface_get_error(void)
{
    return s_ctx.last_error;
}

}  // extern "C"
