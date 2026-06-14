/**
 * LED Control - Implementation
 *
 * Uses ESP-IDF led_strip component with RMT backend for SK6812.
 * Animation runs in a FreeRTOS task.
 */

#include "led_control.h"
#include "app_config.h"
#include "led_strip.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>

static const char *TAG = "led";

static led_strip_handle_t strip = NULL;
static volatile led_state_t current_state = LED_STATE_OFF;
static volatile uint8_t current_volume = 0;
static volatile uint8_t volume_display = 0;
static volatile int64_t volume_display_until_ms = 0;
static volatile bool hardware_muted = false;
static TaskHandle_t led_task_handle = NULL;

static void set_all(uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < LED_COUNT; i++) {
        led_strip_set_pixel(strip, i, r, g, b);
    }
    led_strip_refresh(strip);
}

static void clear_all(void)
{
    led_strip_clear(strip);
}

static uint8_t breath_val(int64_t t_ms, int period_ms)
{
    float phase = (float)(t_ms % period_ms) / period_ms;
    return (uint8_t)(127.5f * (1.0f + sinf(2.0f * M_PI * phase - M_PI / 2.0f)));
}

static uint8_t triangle_val(int64_t t_ms, int period_ms)
{
    int32_t phase = (int32_t)(t_ms % period_ms);
    int32_t half = period_ms / 2;
    int32_t v = phase < half ? phase : period_ms - phase;
    return (uint8_t)((v * 255) / half);
}

static uint8_t volume_led_brightness(uint8_t volume_pct, int led_index)
{
    if (volume_pct == 0) {
        return 0;
    }

    int start = led_index * 25;
    int filled = (int)volume_pct - start;
    if (filled <= 0) {
        return 0;
    }
    if (filled > 25) {
        filled = 25;
    }

    int brightness = (filled * 100 + 24) / 25;
    if (brightness < 1) brightness = 1;
    if (brightness > 100) brightness = 100;
    return (uint8_t)brightness;
}

static void draw_volume_overlay(uint8_t volume_pct)
{
    if (volume_pct == 0) {
        for (int i = 0; i < LED_COUNT; i++) {
            bool red = (i == 1 || i == 2) || (hardware_muted && (i == 0 || i == 3));
            if (red) {
                led_strip_set_pixel(strip, i, 120, 0, 0);
            } else {
                led_strip_set_pixel(strip, i, 0, 0, 0);
            }
        }
        led_strip_refresh(strip);
        return;
    }

    for (int i = 0; i < LED_COUNT; i++) {
        uint8_t pct = volume_led_brightness(volume_pct, i);
        uint8_t v = (uint8_t)((uint16_t)pct * 90 / 100);
        led_strip_set_pixel(strip, i, v, v, v);
    }
    led_strip_refresh(strip);
}

static bool draw_priority_alert_if_needed(void)
{
    bool zero_volume = current_volume == 0;
    bool muted = hardware_muted;
    if (!zero_volume && !muted) {
        return false;
    }

    for (int i = 0; i < LED_COUNT; i++) {
        bool red = (zero_volume && (i == 1 || i == 2)) ||
                   (muted && (i == 0 || i == 3));
        led_strip_set_pixel(strip, i, red ? 120 : 0, 0, 0);
    }
    led_strip_refresh(strip);
    return true;
}

static void led_animation_task(void *arg)
{
    while (1) {
        int64_t now = esp_timer_get_time() / 1000;

        if (now < volume_display_until_ms) {
            draw_volume_overlay(volume_display);
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }

        if (draw_priority_alert_if_needed()) {
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }

        switch (current_state) {
        case LED_STATE_OFF:
            clear_all();
            break;

        case LED_STATE_IDLE: {
            uint8_t v = breath_val(now, 4000) / 8;  // very dim pulse
            set_all(v, v, v);
            break;
        }

        case LED_STATE_LISTENING: {
            uint8_t v = breath_val(now, 1500);
            set_all(0, 0, v);  // blue breathing
            break;
        }

        case LED_STATE_SESSION_ACTIVE: {
            int pos = (int)(now / 220) % LED_COUNT;
            uint8_t pulse = 50 + triangle_val(now, 1200) / 5;
            for (int i = 0; i < LED_COUNT; i++) {
                if (i == pos) {
                    led_strip_set_pixel(strip, i, 40, 180, 255);
                } else {
                    led_strip_set_pixel(strip, i, 0, pulse, 90);
                }
            }
            led_strip_refresh(strip);
            break;
        }

        case LED_STATE_PROCESSING: {
            // Spinning cyan dot
            int pos = (int)(now / 150) % LED_COUNT;
            for (int i = 0; i < LED_COUNT; i++) {
                if (i == pos) {
                    led_strip_set_pixel(strip, i, 0, 180, 180);
                } else {
                    led_strip_set_pixel(strip, i, 0, 20, 20);
                }
            }
            led_strip_refresh(strip);
            break;
        }

        case LED_STATE_SPEAKING: {
            uint8_t v = 80 + breath_val(now, 500) / 2;
            for (int i = 0; i < LED_COUNT; i++) {
                uint8_t accent = ((now / 120 + i) % 2) ? 20 : 0;
                led_strip_set_pixel(strip, i, accent, v, 0);
            }
            led_strip_refresh(strip);
            break;
        }

        case LED_STATE_ERROR:
            if ((now / 300) % 2) {
                set_all(200, 0, 0);
            } else {
                clear_all();
            }
            break;

        case LED_STATE_VOLUME: {
            draw_volume_overlay(volume_display);
            break;
        }

        case LED_STATE_SETUP: {
            uint8_t v = breath_val(now, 3000);
            set_all(v, v / 3, 0);  // orange slow pulse
            break;
        }

        case LED_STATE_MUTED:
            set_all(64, 0, 64);
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(30));  // ~33 fps
    }
}

esp_err_t led_control_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = PIN_LED_STRIP,
        .max_leds = LED_COUNT,
        .led_model = LED_MODEL_SK6812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };

    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,  // 10 MHz
        .mem_block_symbols = 192,
        .flags.with_dma = false,
    };

    esp_err_t ret = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LED strip init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    clear_all();

    xTaskCreatePinnedToCore(led_animation_task, "led_anim", 2048, NULL, 3, &led_task_handle, 1);
    ESP_LOGI(TAG, "LED control initialized (%d LEDs)", LED_COUNT);
    return ESP_OK;
}

void led_control_set_state(led_state_t state)
{
    current_state = state;
}

void led_control_show_volume(uint8_t volume_pct)
{
    if (volume_pct > 100) volume_pct = 100;
    current_volume = volume_pct;
    volume_display = volume_pct;
    volume_display_until_ms = esp_timer_get_time() / 1000 + 3000;
}

void led_control_set_volume(uint8_t volume_pct)
{
    if (volume_pct > 100) volume_pct = 100;
    current_volume = volume_pct;
}

void led_control_set_muted(bool muted)
{
    hardware_muted = muted;
}
