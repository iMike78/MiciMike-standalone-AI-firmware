/**
 * Touch Input Handler
 *
 * Currently uses native ESP32-S3 touch pads.
 * TODO: Migrate to MPR121 I2C capacitive touch controller.
 *
 * Buttons:
 *   - Volume Up (GPIO4)
 *   - Center / Action (GPIO3) — multi-function:
 *       short press: manual wake (skip wake word)
 *       long press (3s): enter setup mode
 *       double tap: stop/cancel current response
 *   - Volume Down (GPIO2)
 */

#pragma once
#include "esp_err.h"

typedef enum {
    TOUCH_EVENT_NONE = 0,
    TOUCH_EVENT_VOL_UP,
    TOUCH_EVENT_VOL_DOWN,
    TOUCH_EVENT_CENTER_SHORT,
    TOUCH_EVENT_CENTER_LONG,
    TOUCH_EVENT_CENTER_DOUBLE,
} touch_event_t;

typedef void (*touch_event_cb_t)(touch_event_t event);

esp_err_t touch_input_init(touch_event_cb_t callback);
