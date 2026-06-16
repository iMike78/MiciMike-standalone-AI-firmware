/**
 * XMOS Voice Kit control - Implementation
 */

#include "xmos_voice_kit.h"
#include "app_config.h"

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>

static const char *TAG = "xmos_voice_kit";

#define XMOS_I2C_ADDR 0x42

#define DFU_CONTROLLER_SERVICER_RESID 240
#define CONFIGURATION_SERVICER_RESID 241
#define CONFIGURATION_COMMAND_READ_BIT 0x80
#define DFU_COMMAND_READ_BIT 0x80

#define CTRL_DONE 0

#define CONFIGURATION_SERVICER_RESID_VNR_VALUE 0x00
#define CONFIGURATION_SERVICER_RESID_CHANNEL_0_PIPELINE_STAGE 0x30
#define CONFIGURATION_SERVICER_RESID_CHANNEL_1_PIPELINE_STAGE 0x40

#define DFU_CONTROLLER_SERVICER_RESID_DFU_GETVERSION 88

static esp_err_t xmos_write(const uint8_t *data, size_t len)
{
    esp_err_t ret = i2c_master_write_to_device(I2C_PORT, XMOS_I2C_ADDR, data, len, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "XMOS I2C write failed addr=0x%02x len=%u: %s",
                 XMOS_I2C_ADDR, (unsigned) len, esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t xmos_read(uint8_t *data, size_t len)
{
    esp_err_t ret = i2c_master_read_from_device(I2C_PORT, XMOS_I2C_ADDR, data, len, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "XMOS I2C read failed addr=0x%02x len=%u: %s",
                 XMOS_I2C_ADDR, (unsigned) len, esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t xmos_request_read(const uint8_t *request, size_t request_len,
                                   uint8_t *response, size_t response_len)
{
    esp_err_t ret = xmos_write(request, request_len);
    if (ret != ESP_OK) {
        return ret;
    }

    return xmos_read(response, response_len);
}

static void xmos_i2c_scan(void)
{
    char found[160];
    size_t pos = 0;
    found[0] = '\0';

    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t ret = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(20));
        i2c_cmd_link_delete(cmd);

        if (ret == ESP_OK && pos + 6 < sizeof(found)) {
            pos += snprintf(found + pos, sizeof(found) - pos, "0x%02x ", addr);
        }
    }

    ESP_LOGI(TAG, "I2C scan found: %s", pos > 0 ? found : "<none>");
}

static void xmos_reset(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_XMOS_RESET),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    // ESPHome voice_kit pulses reset high for 1ms, then leaves it low.
    gpio_set_level(PIN_XMOS_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(1));
    gpio_set_level(PIN_XMOS_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(3000));
    ESP_LOGI(TAG, "XMOS reset pulse complete");
}

static esp_err_t xmos_get_version(xmos_firmware_version_t *version)
{
    const uint8_t version_req[] = {
        DFU_CONTROLLER_SERVICER_RESID,
        DFU_CONTROLLER_SERVICER_RESID_DFU_GETVERSION | DFU_COMMAND_READ_BIT,
        4,
    };
    uint8_t version_resp[4] = {0};

    esp_err_t ret = ESP_FAIL;
    for (int attempt = 1; attempt <= 5; attempt++) {
        ret = xmos_request_read(version_req, sizeof(version_req),
                                version_resp, sizeof(version_resp));
        if (ret == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "XMOS version request attempt %d failed: %s",
                 attempt, esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "XMOS version request failed after retries: %s", esp_err_to_name(ret));
        return ret;
    }

    if (version_resp[0] != CTRL_DONE) {
        ESP_LOGE(TAG, "XMOS version read returned status %u", version_resp[0]);
        return ESP_FAIL;
    }

    if (version) {
        version->major = version_resp[1];
        version->minor = version_resp[2];
        version->patch = version_resp[3];
    }

    ESP_LOGI(TAG, "XMOS firmware version: %u.%u.%u",
             version_resp[1], version_resp[2], version_resp[3]);
    return ESP_OK;
}

static esp_err_t xmos_write_pipeline_stage(uint8_t reg, xmos_pipeline_stage_t stage)
{
    uint8_t stage_set[] = {
        CONFIGURATION_SERVICER_RESID,
        reg,
        1,
        (uint8_t) stage,
    };

    esp_err_t ret = xmos_write(stage_set, sizeof(stage_set));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write XMOS pipeline reg 0x%02x: %s",
                 reg, esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t xmos_read_pipeline_stage(uint8_t reg, xmos_pipeline_stage_t *stage)
{
    const uint8_t stage_req[] = {
        CONFIGURATION_SERVICER_RESID,
        reg | CONFIGURATION_COMMAND_READ_BIT,
        2,
    };
    uint8_t stage_resp[2] = {0};

    esp_err_t ret = xmos_request_read(stage_req, sizeof(stage_req),
                                      stage_resp, sizeof(stage_resp));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read XMOS pipeline reg 0x%02x: %s",
                 reg, esp_err_to_name(ret));
        return ret;
    }

    if (stage) {
        *stage = (xmos_pipeline_stage_t) stage_resp[1];
    }

    return ESP_OK;
}

esp_err_t xmos_voice_kit_setup(xmos_pipeline_stage_t channel_0_stage,
                               xmos_pipeline_stage_t channel_1_stage,
                               xmos_firmware_version_t *version)
{
    xmos_reset();
    xmos_i2c_scan();

    esp_err_t ret = xmos_get_version(version);
    if (ret != ESP_OK) {
        return ret;
    }

    esp_err_t ret0 = xmos_write_pipeline_stage(CONFIGURATION_SERVICER_RESID_CHANNEL_0_PIPELINE_STAGE,
                                               channel_0_stage);
    esp_err_t ret1 = xmos_write_pipeline_stage(CONFIGURATION_SERVICER_RESID_CHANNEL_1_PIPELINE_STAGE,
                                               channel_1_stage);
    if (ret0 != ESP_OK) {
        return ret0;
    }
    if (ret1 != ESP_OK) {
        return ret1;
    }

    xmos_pipeline_stage_t read_ch0 = XMOS_PIPELINE_STAGE_NONE;
    xmos_pipeline_stage_t read_ch1 = XMOS_PIPELINE_STAGE_NONE;
    ret0 = xmos_read_pipeline_stage(CONFIGURATION_SERVICER_RESID_CHANNEL_0_PIPELINE_STAGE, &read_ch0);
    ret1 = xmos_read_pipeline_stage(CONFIGURATION_SERVICER_RESID_CHANNEL_1_PIPELINE_STAGE, &read_ch1);

    ESP_LOGI(TAG, "XMOS pipeline stages: ch0=%u (want %u) ch1=%u (want %u)",
             read_ch0, channel_0_stage, read_ch1, channel_1_stage);

    if (ret0 != ESP_OK || ret1 != ESP_OK) {
        return ESP_FAIL;
    }
    if (read_ch0 != channel_0_stage || read_ch1 != channel_1_stage) {
        ESP_LOGE(TAG, "XMOS pipeline read-back mismatch — XMOS firmware did not accept config");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t xmos_voice_kit_read_vnr(uint8_t *vnr)
{
    if (!vnr) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t vnr_req[] = {
        CONFIGURATION_SERVICER_RESID,
        CONFIGURATION_SERVICER_RESID_VNR_VALUE | CONFIGURATION_COMMAND_READ_BIT,
        2,
    };
    uint8_t vnr_resp[2] = {0};

    esp_err_t ret = xmos_request_read(vnr_req, sizeof(vnr_req), vnr_resp, sizeof(vnr_resp));
    if (ret != ESP_OK) {
        return ret;
    }

    *vnr = vnr_resp[1];
    return ESP_OK;
}
