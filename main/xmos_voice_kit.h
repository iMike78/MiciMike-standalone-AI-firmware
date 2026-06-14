/**
 * XMOS Voice Kit control.
 *
 * Minimal ESP-IDF port of the ESPHome voice_kit component setup path:
 * reset the XU316, verify I2C/DFU communication, and select pipeline stages.
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    XMOS_PIPELINE_STAGE_NONE = 0,
    XMOS_PIPELINE_STAGE_AEC = 1,
    XMOS_PIPELINE_STAGE_IC = 2,
    XMOS_PIPELINE_STAGE_NS = 3,
    XMOS_PIPELINE_STAGE_AGC = 4,
} xmos_pipeline_stage_t;

typedef struct {
    uint8_t major;
    uint8_t minor;
    uint8_t patch;
} xmos_firmware_version_t;

esp_err_t xmos_voice_kit_setup(xmos_pipeline_stage_t channel_0_stage,
                               xmos_pipeline_stage_t channel_1_stage,
                               xmos_firmware_version_t *version);

esp_err_t xmos_voice_kit_read_vnr(uint8_t *vnr);

#ifdef __cplusplus
}
#endif
