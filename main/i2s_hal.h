/**
 * I2S Hardware Abstraction Layer
 *
 * Manages two I2S channels:
 *   - Input:  XMOS → ESP32-S3 (16kHz/32bit/stereo, ESP is secondary)
 *   - Output: ESP32-S3 → AIC3204 (48kHz/32bit/stereo, ESP is secondary)
 */

#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t mm_i2s_init(void);
esp_err_t mm_i2s_deinit(void);

/**
 * Read raw audio from XMOS I2S input.
 * Returns interleaved stereo 32-bit samples at 16kHz.
 * @param buf       destination buffer
 * @param buf_size  buffer size in bytes
 * @param bytes_read  actual bytes read
 * @param timeout_ms  read timeout
 */
esp_err_t mm_i2s_read(void *buf, size_t buf_size, size_t *bytes_read, uint32_t timeout_ms);

/**
 * Write audio to AIC3204 I2S output.
 * Expects interleaved stereo 32-bit samples at 48kHz.
 * @param buf       source buffer
 * @param buf_size  buffer size in bytes
 * @param bytes_written  actual bytes written
 * @param timeout_ms  write timeout
 */
esp_err_t mm_i2s_write(const void *buf, size_t buf_size, size_t *bytes_written, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
