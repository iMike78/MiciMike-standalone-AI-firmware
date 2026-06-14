/**
 * Touch Input Handler - Implementation
 *
 * Uses ESP32-S3 touch sensor v2 API.
 * Polls touch state in a FreeRTOS task and generates events.
 *
 * TODO: Replace with MPR121 I2C driver when hardware migrates.
 * The callback interface stays the same — only the init internals change.
 */

#include "touch_input.h"
#include "app_config.h"
#include "driver/touch_pad.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "touch";

static touch_event_cb_t event_cb = NULL;

// Center button timing
#define LONG_PRESS_MS       3000
#define DOUBLE_TAP_MS       400
#define DEBOUNCE_MS         50

// Touch channel mapping (ESP32-S3 touch channels)
// GPIO2 = TOUCH2, GPIO3 = TOUCH3, GPIO4 = TOUCH4
#define TOUCH_CH_VOL_DOWN   TOUCH_PAD_NUM2
#define TOUCH_CH_CENTER     TOUCH_PAD_NUM3
#define TOUCH_CH_VOL_UP     TOUCH_PAD_NUM4

static bool is_touched(touch_pad_t ch, uint32_t threshold)
{
    uint32_t val = 0;
    touch_pad_read_raw_data(ch, &val);
    return val > threshold;
}

static void touch_poll_task(void *arg)
{
    bool center_down = false;
    int64_t center_down_time = 0;
    int64_t last_center_release = 0;
    bool long_press_fired = false;
    bool vol_up_was_down = false;
    bool vol_down_was_down = false;

    while (1) {
        bool vu = is_touched(TOUCH_CH_VOL_UP, TOUCH_THRESH_VOL_UP);
        bool cn = is_touched(TOUCH_CH_CENTER, TOUCH_THRESH_CENTER);
        bool vd = is_touched(TOUCH_CH_VOL_DOWN, TOUCH_THRESH_VOL_DOWN);
        int64_t now = esp_timer_get_time() / 1000;

        // Volume up — fire on press (with debounce)
        if (vu && !vol_up_was_down) {
            if (event_cb) event_cb(TOUCH_EVENT_VOL_UP);
        }
        vol_up_was_down = vu;

        // Volume down — fire on press (with debounce)
        if (vd && !vol_down_was_down) {
            if (event_cb) event_cb(TOUCH_EVENT_VOL_DOWN);
        }
        vol_down_was_down = vd;

        // Center button — multi-function
        if (cn && !center_down) {
            // Press started
            center_down = true;
            center_down_time = now;
            long_press_fired = false;
        }

        if (cn && center_down && !long_press_fired) {
            // Check for long press
            if (now - center_down_time >= LONG_PRESS_MS) {
                long_press_fired = true;
                if (event_cb) event_cb(TOUCH_EVENT_CENTER_LONG);
            }
        }

        if (!cn && center_down) {
            // Release
            center_down = false;
            if (!long_press_fired) {
                // Check for double tap
                if (now - last_center_release < DOUBLE_TAP_MS) {
                    if (event_cb) event_cb(TOUCH_EVENT_CENTER_DOUBLE);
                    last_center_release = 0;  // consume
                } else {
                    last_center_release = now;
                }
            }
        }

        // Emit short press after double-tap window expires
        if (!center_down && last_center_release > 0 &&
            now - last_center_release >= DOUBLE_TAP_MS) {
            if (event_cb) event_cb(TOUCH_EVENT_CENTER_SHORT);
            last_center_release = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
    }
}

esp_err_t touch_input_init(touch_event_cb_t callback)
{
    event_cb = callback;

    esp_err_t ret = touch_pad_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Touch pad init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER);

    touch_pad_config(TOUCH_CH_VOL_UP);
    touch_pad_config(TOUCH_CH_CENTER);
    touch_pad_config(TOUCH_CH_VOL_DOWN);

    // Denoise config matching ESPHome settings
    touch_pad_denoise_t denoise_cfg = {
        .grade = TOUCH_PAD_DENOISE_BIT8,
        .cap_level = TOUCH_PAD_DENOISE_CAP_L0,
    };
    touch_pad_denoise_set_config(&denoise_cfg);
    touch_pad_denoise_enable();

    touch_filter_config_t filter_cfg = {
        .mode = TOUCH_PAD_FILTER_IIR_16,
        .debounce_cnt = 7,
        .noise_thr = 0,
        .jitter_step = 4,
        .smh_lvl = TOUCH_PAD_SMOOTH_IIR_2,
    };
    touch_pad_filter_set_config(&filter_cfg);
    touch_pad_filter_enable();
    touch_pad_fsm_start();

    xTaskCreatePinnedToCore(touch_poll_task, "touch", 3072, NULL, 5, NULL, 1);
    ESP_LOGI(TAG, "Touch input initialized (native ESP32 pads)");
    return ESP_OK;
}
