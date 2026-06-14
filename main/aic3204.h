/**
 * AIC3204 Audio Codec Driver
 *
 * Basic initialization and volume control for TI TLV320AIC3204.
 * The codec is configured by XMOS at boot; this driver handles
 * volume control and mute from ESP32-S3 side.
 */

#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

esp_err_t aic3204_init(void);
esp_err_t aic3204_set_volume(uint8_t volume_pct);  // 0-100
esp_err_t aic3204_mute(bool mute);
esp_err_t aic3204_set_eq_profile(const char *profile);
bool aic3204_is_ready(void);
