/**
 * I2S Hardware Abstraction Layer - Implementation
 *
 * Uses ESP-IDF v5.x I2S driver (i2s_std).
 * Both channels run in SECONDARY (slave) mode — XMOS provides all clocks.
 */

#include "i2s_hal.h"
#include "app_config.h"
#include "driver/i2s_std.h"
#include "esp_log.h"

static const char *TAG = "i2s_hal";

static i2s_chan_handle_t rx_chan = NULL;  // mic input from XMOS
static i2s_chan_handle_t tx_chan = NULL;  // speaker output to AIC3204

esp_err_t mm_i2s_init(void)
{
    esp_err_t ret;

    // -----------------------------------------------------------------------
    // I2S input channel (XMOS → ESP32-S3): 16kHz, 32bit, stereo, secondary
    // -----------------------------------------------------------------------
    i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_SLAVE);
    rx_chan_cfg.dma_desc_num = 6;
    rx_chan_cfg.dma_frame_num = 240;
    ret = i2s_new_channel(&rx_chan_cfg, NULL, &rx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create RX channel: %s", esp_err_to_name(ret));
        return ret;
    }

    i2s_std_config_t rx_std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2S_IN_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_I2S_IN_BCLK,
            .ws   = PIN_I2S_IN_LRCLK,
            .dout = I2S_GPIO_UNUSED,
            .din  = PIN_I2S_IN_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ret = i2s_channel_init_std_mode(rx_chan, &rx_std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init RX std mode: %s", esp_err_to_name(ret));
        return ret;
    }

    // -----------------------------------------------------------------------
    // I2S output channel (ESP32-S3 → AIC3204): 48kHz, 32bit, stereo, secondary
    // -----------------------------------------------------------------------
    i2s_chan_config_t tx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_SLAVE);
    tx_chan_cfg.dma_desc_num = 6;
    tx_chan_cfg.dma_frame_num = 240;
    ret = i2s_new_channel(&tx_chan_cfg, &tx_chan, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create TX channel: %s", esp_err_to_name(ret));
        return ret;
    }

    i2s_std_config_t tx_std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2S_OUT_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_I2S_OUT_BCLK,
            .ws   = PIN_I2S_OUT_LRCLK,
            .dout = PIN_I2S_OUT_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ret = i2s_channel_init_std_mode(tx_chan, &tx_std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init TX std mode: %s", esp_err_to_name(ret));
        return ret;
    }

    // Enable both channels
    i2s_channel_enable(rx_chan);
    i2s_channel_enable(tx_chan);

    ESP_LOGI(TAG, "I2S initialized: RX=%dHz/%dbit TX=%dHz/%dbit (both secondary)",
             I2S_IN_SAMPLE_RATE, I2S_IN_BITS, I2S_OUT_SAMPLE_RATE, I2S_OUT_BITS);
    return ESP_OK;
}

esp_err_t mm_i2s_deinit(void)
{
    if (rx_chan) {
        i2s_channel_disable(rx_chan);
        i2s_del_channel(rx_chan);
        rx_chan = NULL;
    }
    if (tx_chan) {
        i2s_channel_disable(tx_chan);
        i2s_del_channel(tx_chan);
        tx_chan = NULL;
    }
    return ESP_OK;
}

esp_err_t mm_i2s_read(void *buf, size_t buf_size, size_t *bytes_read, uint32_t timeout_ms)
{
    return i2s_channel_read(rx_chan, buf, buf_size, bytes_read, timeout_ms);
}

esp_err_t mm_i2s_write(const void *buf, size_t buf_size, size_t *bytes_written, uint32_t timeout_ms)
{
    esp_err_t ret = i2s_channel_write(tx_chan, buf, buf_size, bytes_written, timeout_ms);
    if (ret == ESP_ERR_INVALID_STATE && tx_chan) {
        ESP_LOGW(TAG, "TX channel disabled, re-enabling");
        i2s_channel_enable(tx_chan);
        ret = i2s_channel_write(tx_chan, buf, buf_size, bytes_written, timeout_ms);
    }
    return ret;
}
