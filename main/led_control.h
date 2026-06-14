/**
 * LED Control - SK6812 RGBW strip (4 LEDs)
 *
 * Visual feedback for device state:
 *   - Idle:        off / dim pulse
 *   - Listening:   blue breathing
 *   - Processing:  spinning cyan
 *   - Speaking:    green pulse (amplitude-reactive)
 *   - Error:       red flash
 *   - Volume:      3s white level overlay
 *   - 0% volume:   LEDs 2-3 red priority overlay
 *   - HW mute:     LEDs 1 and 4 red priority overlay
 *   - Setup mode:  orange slow pulse
 */

#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    LED_STATE_OFF = 0,
    LED_STATE_IDLE,
    LED_STATE_LISTENING,
    LED_STATE_SESSION_ACTIVE,
    LED_STATE_PROCESSING,
    LED_STATE_SPEAKING,
    LED_STATE_ERROR,
    LED_STATE_VOLUME,
    LED_STATE_SETUP,
    LED_STATE_MUTED,
} led_state_t;

esp_err_t led_control_init(void);
void led_control_set_state(led_state_t state);
void led_control_show_volume(uint8_t volume_pct);  // 0-100, segmented over 4 LEDs for 3s
void led_control_set_volume(uint8_t volume_pct);
void led_control_set_muted(bool muted);
