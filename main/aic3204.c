/**
 * AIC3204 Audio Codec Driver - Implementation
 *
 * NOTE: The XMOS firmware / voice_kit component normally initializes
 * the codec fully. This driver provides volume control from ESP32-S3.
 * Full codec init sequence may need adjustment based on XMOS fw behavior.
 *
 * TODO: Verify which registers XMOS sets at boot. If XMOS fully configures
 * the codec, we only need volume/mute here. If not, add full init sequence
 * from the ESPHome aic3204 component source.
 */

#include "aic3204.h"
#include "app_config.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <strings.h>

static const char *TAG = "aic3204";
static bool aic3204_ready = false;

// TLV320AIC3204 register definitions used by the ESPHome aic3204 driver.
// Page 0
#define AIC3204_REG_PAGE_SELECT     0x00
#define AIC3204_REG_RESET           0x01
#define AIC3204_REG_NDAC            0x0B
#define AIC3204_REG_MDAC            0x0C
#define AIC3204_REG_DOSR            0x0E
#define AIC3204_REG_CODEC_IF        0x1B
#define AIC3204_REG_AUDIO_IF_4      0x1F
#define AIC3204_REG_AUDIO_IF_5      0x20
#define AIC3204_REG_SCLK_MFP3       0x38
#define AIC3204_REG_DAC_SIG_PROC    0x3C
#define AIC3204_REG_DAC_CH_SET1     0x3F
#define AIC3204_REG_DAC_CH_SET2     0x40
#define AIC3204_REG_DAC_VOL_L       0x41
#define AIC3204_REG_DAC_VOL_R       0x42

// Page 1
#define AIC3204_REG_PWR_CFG         0x01
#define AIC3204_REG_LDO_CTRL        0x02
#define AIC3204_REG_PLAY_CFG1       0x03
#define AIC3204_REG_PLAY_CFG2       0x04
#define AIC3204_REG_OP_PWR_CTRL     0x09
#define AIC3204_REG_CM_CTRL         0x0A
#define AIC3204_REG_HPL_ROUTE       0x0C
#define AIC3204_REG_HPR_ROUTE       0x0D
#define AIC3204_REG_LOL_ROUTE       0x0E
#define AIC3204_REG_LOR_ROUTE       0x0F
#define AIC3204_REG_HPL_GAIN        0x10
#define AIC3204_REG_HPR_GAIN        0x11
#define AIC3204_REG_LOL_DRV_GAIN    0x12
#define AIC3204_REG_LOR_DRV_GAIN    0x13
#define AIC3204_REG_HP_START        0x14
#define AIC3204_REG_REF_STARTUP     0x7B

static esp_err_t aic3204_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t data[2] = {reg, val};
    esp_err_t ret = i2c_master_write_to_device(I2C_PORT, AIC3204_I2C_ADDR, data, 2, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Write reg 0x%02x=0x%02x failed: %s", reg, val, esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t aic3204_read_reg(uint8_t reg, uint8_t *val)
{
    return i2c_master_write_read_device(I2C_PORT, AIC3204_I2C_ADDR,
                                        &reg, 1, val, 1, pdMS_TO_TICKS(100));
}

static esp_err_t aic3204_select_page(uint8_t page)
{
    return aic3204_write_reg(AIC3204_REG_PAGE_SELECT, page);
}

#define AIC_CHECK(expr) do { \
    esp_err_t _ret = (expr); \
    if (_ret != ESP_OK) return _ret; \
} while (0)

typedef struct {
    uint32_t n0;
    uint32_t n1;
    uint32_t n2;
    uint32_t d1;
    uint32_t d2;
} aic3204_biquad_t;

typedef struct {
    const char *name;
    aic3204_biquad_t biquad[5];
} aic3204_eq_profile_t;

#define BQ(N0, N1, N2, D1, D2) { (N0), (N1), (N2), (D1), (D2) }

static const aic3204_eq_profile_t EQ_PROFILES[] = {
    {
        "Flat",
        {
            BQ(0x7FFFFF, 0x000000, 0x000000, 0x000000, 0x000000),
            BQ(0x7FFFFF, 0x000000, 0x000000, 0x000000, 0x000000),
            BQ(0x7FFFFF, 0x000000, 0x000000, 0x000000, 0x000000),
            BQ(0x7FFFFF, 0x000000, 0x000000, 0x000000, 0x000000),
            BQ(0x7FFFFF, 0x000000, 0x000000, 0x000000, 0x000000),
        },
    },
    {
        "Rock",
        {
            BQ(0x7FFFFF, 0x8109D2, 0x7DEE5F, 0x7F698C, 0x812AE4),
            BQ(0x7FFFFF, 0x83250C, 0x79D2E5, 0x7D7A58, 0x84EE2E),
            BQ(0x7C8B37, 0x8CA83F, 0x6BC9C4, 0x7357C1, 0x97AB04),
            BQ(0x7FFFFF, 0xB02945, 0x3335DB, 0x57812D, 0xBB9525),
            BQ(0x7FFFFF, 0x114C9F, 0x05ADC4, 0xEAFF46, 0xDDB3D8),
        },
    },
    {
        "Pop",
        {
            BQ(0x7FFFFF, 0x80B5C4, 0x7E9679, 0x7F7ED0, 0x81005A),
            BQ(0x7FFFFF, 0x822ECB, 0x7BBF9F, 0x7E0B7B, 0x83CBC5),
            BQ(0x7FFFFF, 0x880902, 0x71A457, 0x77F6FD, 0x8E5BA8),
            BQ(0x7FFFFF, 0xA74C99, 0x471A3A, 0x5CF31E, 0xAF5C39),
            BQ(0x7FFFFF, 0x13B197, 0x182E6E, 0xE9A90E, 0xD35F64),
        },
    },
    {
        "Voice",
        {
            BQ(0x7FD0B1, 0x80812F, 0x7F2EF2, 0x7F7ED0, 0x81005A),
            BQ(0x7F9C10, 0x81F484, 0x7C9829, 0x7E0B7B, 0x83CBC5),
            BQ(0x7FFFFF, 0x8BD877, 0x69F781, 0x77F6FD, 0x8E5BA8),
            BQ(0x7FFFFF, 0xB180E2, 0x30328E, 0x5CF31E, 0xAF5C39),
            BQ(0x7FFFFF, 0x157C5E, 0x260796, 0xE9A90E, 0xD35F64),
        },
    },
    {
        "Clear",
        {
            BQ(0x7FE5A2, 0x80812F, 0x7F1A02, 0x7F7ED0, 0x81005A),
            BQ(0x7FFFFF, 0x81F484, 0x7C343A, 0x7E0B7B, 0x83CBC5),
            BQ(0x7FFFFF, 0x89C09D, 0x6E2EDB, 0x77F6FD, 0x8E5BA8),
            BQ(0x7FFFFF, 0xAC203F, 0x3C449F, 0x5CF31E, 0xAF5C39),
            BQ(0x7FFFFF, 0x13B197, 0x182E6E, 0xE9A90E, 0xD35F64),
        },
    },
    {
        "Loudness",
        {
            BQ(0x7FFFFF, 0x80FFC1, 0x7E027E, 0x7F7ED0, 0x81005A),
            BQ(0x7FFFFF, 0x82B8B3, 0x7AABAE, 0x7E0B7B, 0x83CBC5),
            BQ(0x7FFFFF, 0x880902, 0x71A457, 0x77F6FD, 0x8E5BA8),
            BQ(0x7FFFFF, 0xA74C99, 0x471A3A, 0x5CF31E, 0xAF5C39),
            BQ(0x7FFFFF, 0x11D281, 0x09B856, 0xE9A90E, 0xD35F64),
        },
    },
    {
        "Jazz",
        {
            BQ(0x7FF20E, 0x80812F, 0x7F0D95, 0x7F7ED0, 0x81005A),
            BQ(0x7FFFFF, 0x822ECB, 0x7BBF9F, 0x7E0B7B, 0x83CBC5),
            BQ(0x7FFFFF, 0x8ABF85, 0x6C2D68, 0x77F6FD, 0x8E5BA8),
            BQ(0x7FFFFF, 0xA74C99, 0x471A3A, 0x5CF31E, 0xAF5C39),
            BQ(0x7FFFFF, 0x157C5E, 0x260796, 0xE9A90E, 0xD35F64),
        },
    },
};

static const aic3204_eq_profile_t *aic3204_find_eq_profile(const char *name)
{
    if (!name || name[0] == '\0') {
        name = DEFAULT_EQ_PROFILE;
    }

    for (size_t i = 0; i < sizeof(EQ_PROFILES) / sizeof(EQ_PROFILES[0]); i++) {
        if (strcasecmp(EQ_PROFILES[i].name, name) == 0) {
            return &EQ_PROFILES[i];
        }
    }
    return NULL;
}

static esp_err_t aic3204_write_coeff_24(uint8_t page, uint8_t reg, uint32_t val)
{
    AIC_CHECK(aic3204_select_page(page));
    AIC_CHECK(aic3204_write_reg(reg + 0, (uint8_t)((val >> 16) & 0xFF)));
    AIC_CHECK(aic3204_write_reg(reg + 1, (uint8_t)((val >> 8) & 0xFF)));
    AIC_CHECK(aic3204_write_reg(reg + 2, (uint8_t)(val & 0xFF)));
    return ESP_OK;
}

static esp_err_t aic3204_write_biquad(uint8_t biq, const aic3204_biquad_t *bq)
{
    uint8_t base_l = 12 + 20 * biq;
    uint8_t base_r = 20 + 20 * biq;

    AIC_CHECK(aic3204_write_coeff_24(44, base_l + 0, bq->n0));
    AIC_CHECK(aic3204_write_coeff_24(44, base_l + 4, bq->n1));
    AIC_CHECK(aic3204_write_coeff_24(44, base_l + 8, bq->n2));
    AIC_CHECK(aic3204_write_coeff_24(44, base_l + 12, bq->d1));
    AIC_CHECK(aic3204_write_coeff_24(44, base_l + 16, bq->d2));

    AIC_CHECK(aic3204_write_coeff_24(45, base_r + 0, bq->n0));
    AIC_CHECK(aic3204_write_coeff_24(45, base_r + 4, bq->n1));
    AIC_CHECK(aic3204_write_coeff_24(45, base_r + 8, bq->n2));
    AIC_CHECK(aic3204_write_coeff_24(45, base_r + 12, bq->d1));
    AIC_CHECK(aic3204_write_coeff_24(45, base_r + 16, bq->d2));
    return ESP_OK;
}

static esp_err_t aic3204_setup_playback_path(void)
{
    // ESPHome's 48 kHz / 32-bit I2S setup for the Voice PE / XMOS path.
    // XMOS provides 24.576 MHz MCLK; NDAC=2, MDAC=2, DOSR=128 gives 48 kHz.
    AIC_CHECK(aic3204_select_page(0));
    AIC_CHECK(aic3204_write_reg(AIC3204_REG_RESET, 0x01));
    vTaskDelay(pdMS_TO_TICKS(10));

    AIC_CHECK(aic3204_write_reg(AIC3204_REG_NDAC, 0x82));
    AIC_CHECK(aic3204_write_reg(AIC3204_REG_MDAC, 0x82));
    AIC_CHECK(aic3204_write_reg(AIC3204_REG_DOSR, 0x80));
    AIC_CHECK(aic3204_write_reg(AIC3204_REG_CODEC_IF, 0x30));      // I2S, 32 bits
    AIC_CHECK(aic3204_write_reg(AIC3204_REG_SCLK_MFP3, 0x02));     // MFP3 as DIN
    AIC_CHECK(aic3204_write_reg(AIC3204_REG_AUDIO_IF_4, 0x01));
    AIC_CHECK(aic3204_write_reg(AIC3204_REG_AUDIO_IF_5, 0x01));
    AIC_CHECK(aic3204_write_reg(AIC3204_REG_DAC_SIG_PROC, 0x01));  // PRB_P1

    AIC_CHECK(aic3204_select_page(1));
    AIC_CHECK(aic3204_write_reg(AIC3204_REG_LDO_CTRL, 0x09));
    AIC_CHECK(aic3204_write_reg(AIC3204_REG_PWR_CFG, 0x08));
    AIC_CHECK(aic3204_write_reg(AIC3204_REG_LDO_CTRL, 0x01));
    AIC_CHECK(aic3204_write_reg(AIC3204_REG_CM_CTRL, 0x40));
    AIC_CHECK(aic3204_write_reg(AIC3204_REG_PLAY_CFG1, 0x00));
    AIC_CHECK(aic3204_write_reg(AIC3204_REG_PLAY_CFG2, 0x00));
    AIC_CHECK(aic3204_write_reg(AIC3204_REG_REF_STARTUP, 0x01));
    AIC_CHECK(aic3204_write_reg(AIC3204_REG_HP_START, 0x25));
    AIC_CHECK(aic3204_write_reg(AIC3204_REG_HPL_ROUTE, 0x08));
    AIC_CHECK(aic3204_write_reg(AIC3204_REG_HPR_ROUTE, 0x08));
    AIC_CHECK(aic3204_write_reg(AIC3204_REG_LOL_ROUTE, 0x08));
    AIC_CHECK(aic3204_write_reg(AIC3204_REG_LOR_ROUTE, 0x08));
    AIC_CHECK(aic3204_write_reg(AIC3204_REG_HPL_GAIN, 0x3E));
    AIC_CHECK(aic3204_write_reg(AIC3204_REG_HPR_GAIN, 0x3E));
    AIC_CHECK(aic3204_write_reg(AIC3204_REG_LOL_DRV_GAIN, 0x00));
    AIC_CHECK(aic3204_write_reg(AIC3204_REG_LOR_DRV_GAIN, 0x00));
    AIC_CHECK(aic3204_write_reg(AIC3204_REG_OP_PWR_CTRL, 0x3C));

    vTaskDelay(pdMS_TO_TICKS(2500));

    AIC_CHECK(aic3204_select_page(0));
    AIC_CHECK(aic3204_write_reg(AIC3204_REG_DAC_CH_SET1, 0xD4));   // power L/R DAC
    AIC_CHECK(aic3204_mute(false));
    return ESP_OK;
}

static void aic3204_log_playback_regs(void)
{
    uint8_t dac_ch = 0, dac_mute = 0, codec_if = 0, op_pwr = 0;
    aic3204_select_page(0);
    aic3204_read_reg(AIC3204_REG_DAC_CH_SET1, &dac_ch);
    aic3204_read_reg(AIC3204_REG_DAC_CH_SET2, &dac_mute);
    aic3204_read_reg(AIC3204_REG_CODEC_IF, &codec_if);
    aic3204_select_page(1);
    aic3204_read_reg(AIC3204_REG_OP_PWR_CTRL, &op_pwr);
    aic3204_select_page(0);
    ESP_LOGI(TAG, "Playback regs: codec_if=0x%02x dac_ch=0x%02x dac_mute=0x%02x op_pwr=0x%02x",
             codec_if, dac_ch, dac_mute, op_pwr);
}

esp_err_t aic3204_init(void)
{
    aic3204_ready = false;

    // Initialize I2C master
    i2c_config_t i2c_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ_HZ,
    };
    esp_err_t ret = i2c_param_config(I2C_PORT, &i2c_cfg);
    if (ret != ESP_OK) return ret;

    ret = i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (ret != ESP_OK) return ret;

    // Verify codec is present on I2C bus
    uint8_t dummy;
    ret = i2c_master_read_from_device(I2C_PORT, AIC3204_I2C_ADDR, &dummy, 1, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AIC3204 not found at 0x%02x", AIC3204_I2C_ADDR);
        return ret;
    }

    ESP_LOGI(TAG, "AIC3204 found at 0x%02x", AIC3204_I2C_ADDR);

    ret = aic3204_setup_playback_path();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Playback path init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "DAC playback path initialized");

    // Set default volume
    aic3204_set_volume(DEFAULT_VOLUME);
    aic3204_log_playback_regs();
    aic3204_ready = true;

    return ESP_OK;
}

esp_err_t aic3204_set_volume(uint8_t volume_pct)
{
    if (volume_pct > 100) volume_pct = 100;

    // AIC3204 DAC digital volume is signed, 0x00 = 0dB.
    // Keep 100% at 0dB and map the useful range down to about -48dB.
    int8_t db_val;
    if (volume_pct == 0) {
        db_val = (int8_t)0x81;  // mute
    } else {
        // Linear mapping: 100% = 0dB, 1% ≈ -48dB
        db_val = (int8_t)(-(48 - (volume_pct * 48 / 100)));
    }

    aic3204_select_page(0);
    aic3204_write_reg(AIC3204_REG_DAC_VOL_L, (uint8_t)db_val);
    aic3204_write_reg(AIC3204_REG_DAC_VOL_R, (uint8_t)db_val);

    ESP_LOGI(TAG, "Volume set to %d%% (reg=0x%02x)", volume_pct, (uint8_t)db_val);
    return ESP_OK;
}

esp_err_t aic3204_mute(bool mute)
{
    aic3204_select_page(0);
    // Bits 3:2 mute left/right DAC digital paths. Bits 6:4 are auto-mute mode.
    uint8_t val = mute ? 0x0C : 0x00;
    return aic3204_write_reg(AIC3204_REG_DAC_CH_SET2, val);
}

esp_err_t aic3204_set_eq_profile(const char *profile)
{
    const aic3204_eq_profile_t *eq = aic3204_find_eq_profile(profile);
    if (!eq) {
        ESP_LOGW(TAG, "Unknown EQ profile '%s', using %s",
                 profile ? profile : "<null>", DEFAULT_EQ_PROFILE);
        eq = aic3204_find_eq_profile(DEFAULT_EQ_PROFILE);
        if (!eq) return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Applying EQ profile: %s", eq->name);

    uint8_t cfg = 0;
    AIC_CHECK(aic3204_select_page(44));
    AIC_CHECK(aic3204_read_reg(0x01, &cfg));
    cfg |= 0x04;   // adaptive mode on
    cfg &= ~0x01;  // do not switch before the inactive buffer is fully written
    AIC_CHECK(aic3204_write_reg(0x01, cfg));

    for (uint8_t i = 0; i < 5; i++) {
        AIC_CHECK(aic3204_write_biquad(i, &eq->biquad[i]));
    }

    AIC_CHECK(aic3204_select_page(44));
    AIC_CHECK(aic3204_read_reg(0x01, &cfg));
    cfg |= 0x04;
    cfg |= 0x01;   // request buffer switch at the next frame
    AIC_CHECK(aic3204_write_reg(0x01, cfg));
    AIC_CHECK(aic3204_select_page(0));

    ESP_LOGI(TAG, "EQ profile applied: %s", eq->name);
    return ESP_OK;
}

bool aic3204_is_ready(void)
{
    return aic3204_ready;
}
