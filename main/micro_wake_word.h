/**
 * Micro Wake Word Detection
 *
 * On-device wake word detection using TFLite Micro, compatible with
 * microWakeWord models (okay_nabu, hey_jarvis, etc.)
 *
 * Audio pipeline:
 *   16kHz mono PCM → micro_speech frontend (mel spectrogram) → TFLite inference
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef void (*mww_detection_cb_t)(const char *wake_word);

typedef struct {
    const char *wake_word_label;       // e.g. "okay_nabu"
    const uint8_t *model_data;         // pointer to embedded TFLite model
    size_t model_size;                 // model size in bytes
    float probability_cutoff;          // detection threshold (0.0-1.0), default 0.5
    size_t sliding_window_size;        // number of inferences to average, default 10
    size_t tensor_arena_size;          // tensor arena bytes, default 20000
    uint8_t gain_factor;               // audio gain before frontend, default 1
    uint8_t input_channel;             // I2S stereo channel to use, default 0
    mww_detection_cb_t on_detected;    // callback when wake word detected
} mww_config_t;

/**
 * Initialize the wake word detection system.
 * Must be called after I2S is initialized.
 */
esp_err_t mww_init(const mww_config_t *config);

/**
 * Start listening for wake word.
 * Launches the preprocessor and inference tasks.
 */
esp_err_t mww_start(void);

/**
 * Stop listening (e.g. during active conversation session).
 */
esp_err_t mww_stop(void);

/**
 * Update only the detection probability threshold.
 * This is safe while the current model keeps running.
 */
esp_err_t mww_set_probability_cutoff(float probability_cutoff);

/**
 * Check if MWW is currently listening.
 */
bool mww_is_running(void);

#ifdef __cplusplus
}
#endif
