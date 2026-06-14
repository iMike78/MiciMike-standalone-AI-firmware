/**
 * Micro Wake Word Detection - Implementation
 *
 * Uses TFLite Micro for inference and the micro_speech frontend
 * for audio feature extraction (mel spectrogram).
 *
 * Compatible with microWakeWord v2 models:
 *   - 16kHz mono audio input
 *   - 40 mel features, 30ms window, 10ms stride
 *   - Streaming inference with sliding window averaging
 */

#include "micro_wake_word.h"
#include "i2s_hal.h"
#include "app_config.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_allocator.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"
#include "tensorflow/lite/schema/schema_generated.h"

// Micro frontend for audio feature extraction (local copy)
#include "microfrontend/frontend.h"
#include "microfrontend/frontend_util.h"

#include <cstring>
#include <cmath>
#include <cstdint>

static const char *TAG = "mww";

// ---------------------------------------------------------------------------
// Configuration constants matching microWakeWord v2 models
// ---------------------------------------------------------------------------
#define MWW_SAMPLE_RATE         16000
#define MWW_WINDOW_MS           30
#define MWW_STRIDE_MS           10
#define MWW_WINDOW_SAMPLES      (MWW_SAMPLE_RATE * MWW_WINDOW_MS / 1000)   // 480
#define MWW_STRIDE_SAMPLES      (MWW_SAMPLE_RATE * MWW_STRIDE_MS / 1000)   // 160
#define MWW_NUM_FEATURES        40
#define MWW_FFT_SIZE            512
#define MWW_DETECTION_COOLDOWN_MS 2500
#define MWW_LOWER_FREQ          125
#define MWW_UPPER_FREQ          7500

// Audio buffer: read stereo 32-bit from I2S, extract mono 16-bit
#define I2S_READ_SIZE           (MWW_STRIDE_SAMPLES * 2 * sizeof(int32_t))  // stereo 32bit
#define AUDIO_RING_BUF_SIZE     (MWW_STRIDE_SAMPLES * 16 * sizeof(int16_t)) // ~16 strides

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static mww_config_t cfg;
static volatile bool running = false;
static bool mww_initialized = false;

// TFLite Micro
static const tflite::Model *model = nullptr;
static tflite::MicroInterpreter *interpreter = nullptr;
static uint8_t *tensor_arena = nullptr;

// Audio frontend
static struct FrontendState frontend_state;
static bool frontend_initialized = false;

// Stride step counter: tracks which slot in the model's input tensor to write next.
// ESPHome streaming model pattern: write directly into input tensor slots,
// Invoke() only when all stride slots are filled.
static size_t features_per_inference = 0;  // = input->dims->data[1], e.g. 3
static size_t current_stride_step = 0;     // cycles 0..features_per_inference-1

// Probability sliding window
static float *prob_window = nullptr;
static size_t prob_window_pos = 0;

// ---------------------------------------------------------------------------
// Audio frontend initialization
// ---------------------------------------------------------------------------
static bool init_frontend(void)
{
    struct FrontendConfig frontend_config;
    frontend_config.window.size_ms = MWW_WINDOW_MS;
    frontend_config.window.step_size_ms = MWW_STRIDE_MS;
    frontend_config.filterbank.num_channels = MWW_NUM_FEATURES;
    frontend_config.filterbank.lower_band_limit = MWW_LOWER_FREQ;
    frontend_config.filterbank.upper_band_limit = MWW_UPPER_FREQ;
    frontend_config.noise_reduction.smoothing_bits = 10;
    frontend_config.noise_reduction.even_smoothing = 0.025f;
    frontend_config.noise_reduction.odd_smoothing = 0.06f;
    frontend_config.noise_reduction.min_signal_remaining = 0.05f;
    frontend_config.pcan_gain_control.enable_pcan = 1;
    frontend_config.pcan_gain_control.strength = 0.95f;
    frontend_config.pcan_gain_control.offset = 80.0f;
    frontend_config.pcan_gain_control.gain_bits = 21;
    frontend_config.log_scale.enable_log = 1;
    frontend_config.log_scale.scale_shift = 6;

    if (!FrontendPopulateState(&frontend_config, &frontend_state, MWW_SAMPLE_RATE)) {
        ESP_LOGE(TAG, "Failed to initialize audio frontend");
        return false;
    }

    frontend_initialized = true;
    ESP_LOGI(TAG, "Audio frontend initialized: %d features, %dms window, %dms stride",
             MWW_NUM_FEATURES, MWW_WINDOW_MS, MWW_STRIDE_MS);
    return true;
}

// ---------------------------------------------------------------------------
// TFLite model initialization
// ---------------------------------------------------------------------------
static bool init_model(void)
{
    model = tflite::GetModel(cfg.model_data);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "Model schema version mismatch: got %lu, expected %d",
                 model->version(), TFLITE_SCHEMA_VERSION);
        return false;
    }

    // Allocate tensor arena in PSRAM
    tensor_arena = (uint8_t *)heap_caps_malloc(cfg.tensor_arena_size,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!tensor_arena) {
        ESP_LOGE(TAG, "Failed to allocate tensor arena (%d bytes)", cfg.tensor_arena_size);
        return false;
    }

    // Register only the ops used by okay_nabu_model.h.
    // Model op list:
    // CALL_ONCE, VAR_HANDLE, RESHAPE, READ_VARIABLE, CONCATENATION,
    // STRIDED_SLICE, ASSIGN_VARIABLE, CONV_2D, DEPTHWISE_CONV_2D,
    // SPLIT_V, FULLY_CONNECTED, LOGISTIC, QUANTIZE
    static tflite::MicroMutableOpResolver<13> resolver;
    resolver.AddCallOnce();
    resolver.AddVarHandle();
    resolver.AddReshape();
    resolver.AddReadVariable();
    resolver.AddConcatenation();
    resolver.AddStridedSlice();
    resolver.AddAssignVariable();
    resolver.AddConv2D();
    resolver.AddDepthwiseConv2D();
    resolver.AddSplitV();
    resolver.AddFullyConnected();
    resolver.AddLogistic();
    resolver.AddQuantize();

    // VAR_HANDLE / READ_VARIABLE / ASSIGN_VARIABLE require resource variables.
    constexpr int kNumResourceVariables = 8;

    static tflite::MicroAllocator *allocator =
        tflite::MicroAllocator::Create(tensor_arena, cfg.tensor_arena_size);

    static tflite::MicroResourceVariables *resource_variables =
        tflite::MicroResourceVariables::Create(allocator, kNumResourceVariables);

    static tflite::MicroInterpreter static_interpreter(
        model,
        resolver,
        allocator,
        resource_variables);
    interpreter = &static_interpreter;

    if (interpreter->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "Failed to allocate tensors");
        return false;
    }

    // Determine model input shape to know how many feature slices it expects
    TfLiteTensor *input = interpreter->input(0);
    if (!input) {
        ESP_LOGE(TAG, "Failed to get model input tensor");
        return false;
    }

    // Model input is typically [1, num_frames, num_features, 1] for streaming models
    // or [1, num_features] for single-frame streaming
    ESP_LOGI(TAG, "Model input: dims=%d, type=%d", input->dims->size, input->type);
    for (int i = 0; i < input->dims->size; i++) {
        ESP_LOGI(TAG, "  dim[%d] = %d", i, input->dims->data[i]);
    }

    // For streaming microWakeWord models, input is typically [1, num_features]
    // meaning it takes one spectrogram slice at a time
    if (input->dims->size == 2) {
        features_per_inference = 1;
    } else if (input->dims->size == 4) {
        features_per_inference = input->dims->data[1];  // [1, frames, features, 1]
    } else if (input->dims->size == 3) {
        features_per_inference = input->dims->data[1];  // [1, frames, features]
    } else {
        features_per_inference = 1;
    }

    // Allocate probability sliding window
    prob_window = (float *)heap_caps_calloc(cfg.sliding_window_size, sizeof(float),
                                            MALLOC_CAP_SPIRAM);
    if (!prob_window) {
        ESP_LOGE(TAG, "Failed to allocate probability window");
        return false;
    }

    ESP_LOGI(TAG, "Model loaded: %s, arena=%d bytes, features_per_inf=%d",
             cfg.wake_word_label, cfg.tensor_arena_size, features_per_inference);
    return true;
}

// ---------------------------------------------------------------------------
// Extract mono 16-bit from stereo 32-bit I2S data (left channel)
// ---------------------------------------------------------------------------
static size_t extract_mono_16bit(const int32_t *stereo_32, int16_t *mono_16, size_t frames)
{
    const uint8_t channel = cfg.input_channel > 1 ? 0 : cfg.input_channel;
    size_t clipped_samples = 0;

    for (size_t i = 0; i < frames; i++) {
        int32_t sample = stereo_32[i * 2 + channel] >> 16;
        sample *= cfg.gain_factor;

        if (sample > 32767) {
            sample = 32767;
            clipped_samples++;
        } else if (sample < -32768) {
            sample = -32768;
            clipped_samples++;
        }

        mono_16[i] = (int16_t)sample;
    }

    return clipped_samples;
}

// ---------------------------------------------------------------------------
// Streaming inference: ESPHome pattern
// Write one feature slice directly into the model's input tensor at the
// current stride slot. Invoke() only when all stride slots are filled.
// Returns probability [0..1], or -1.0 if not yet ready to invoke.
// ---------------------------------------------------------------------------
static float streaming_inference(const int8_t *features)
{
    TfLiteTensor *input = interpreter->input(0);

    // Write this frame into the correct stride slot of the input tensor
    int8_t *tensor_data = tflite::GetTensorData<int8_t>(input);
    std::memmove(tensor_data + MWW_NUM_FEATURES * current_stride_step,
                 features, MWW_NUM_FEATURES);
    current_stride_step++;

    if (current_stride_step < features_per_inference) {
        return -1.0f;  // not ready yet
    }
    current_stride_step = 0;

    if (interpreter->Invoke() != kTfLiteOk) {
        ESP_LOGE(TAG, "Inference failed");
        return 0.0f;
    }

    TfLiteTensor *output = interpreter->output(0);

    static bool output_info_logged = false;
    if (!output_info_logged) {
        ESP_LOGI(TAG, "Output tensor: dims=%d, type=%d, scale=%f, zero_point=%d",
                 output->dims->size, output->type,
                 output->params.scale, output->params.zero_point);
        for (int i = 0; i < output->dims->size; i++) {
            ESP_LOGI(TAG, "  out_dim[%d] = %d", i, output->dims->data[i]);
        }
        output_info_logged = true;
    }

    // ESPHome uses uint8 output (0..255 → 0.0..1.0)
    static uint32_t invoke_count = 0;
    invoke_count++;
    if (output->type == kTfLiteUInt8) {
        uint8_t raw_out = output->data.uint8[0];
        return raw_out / 255.0f;
    } else if (output->type == kTfLiteInt8) {
        int8_t raw_out = output->data.int8[0];
        float p = ((int32_t)raw_out - output->params.zero_point) * output->params.scale;
        return p;
    } else if (output->type == kTfLiteFloat32) {
        return output->data.f[0];
    }
    return 0.0f;
}

// ---------------------------------------------------------------------------
// Main detection task
// ---------------------------------------------------------------------------
static void mww_detection_task(void *arg)
{
    // Allocate audio buffers
    int32_t *i2s_buf = (int32_t *)heap_caps_malloc(I2S_READ_SIZE,
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *mono_buf = (int16_t *)heap_caps_malloc(MWW_STRIDE_SAMPLES * sizeof(int16_t),
                                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!i2s_buf || !mono_buf) {
        ESP_LOGE(TAG, "Failed to allocate audio buffers");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Detection task started, listening for '%s'...", cfg.wake_word_label);

    TickType_t cooldown_until = 0;

    while (running) {
        // Read audio from I2S (XMOS processed output)
        size_t bytes_read;
        esp_err_t ret = mm_i2s_read(i2s_buf, I2S_READ_SIZE, &bytes_read, 100);
        if (ret != ESP_OK || bytes_read == 0) {
            continue;
        }

        if (cooldown_until != 0 &&
            (int32_t)(xTaskGetTickCount() - cooldown_until) < 0) {
            continue;
        }

        size_t frames_read = bytes_read / (2 * sizeof(int32_t));  // stereo frames
        // Extract mono 16-bit audio
        extract_mono_16bit(i2s_buf, mono_buf, frames_read);

        // Feed audio to frontend and get features
        size_t num_samples_read = 0;
        struct FrontendOutput frontend_output = FrontendProcessSamples(
            &frontend_state, mono_buf, frames_read, &num_samples_read);

        if (frontend_output.size != MWW_NUM_FEATURES) {
            continue;  // not enough audio yet for a full feature frame
        }

        // Quantize frontend output to int8 — ESPHome reference formula:
        //   input = (feature * 256) / 666 - 128  where 666 = round(25.6 * 26.0)
        constexpr int32_t value_scale = 256;
        constexpr int32_t value_div   = 666;

        int8_t features[MWW_NUM_FEATURES];
        for (int i = 0; i < MWW_NUM_FEATURES; i++) {
            uint16_t raw = frontend_output.values[i];

            int32_t quantized = ((int32_t)raw * value_scale + value_div / 2) / value_div + INT8_MIN;
            if (quantized < -128) quantized = -128;
            if (quantized >  127) quantized = 127;
            features[i] = (int8_t)quantized;
        }

        // ESPHome streaming inference: write into input tensor directly,
        // Invoke() only every features_per_inference frames.
        float probability = streaming_inference(features);
        if (probability < 0.0f) {
            continue;  // not enough frames yet
        }

        // Update sliding window
        prob_window[prob_window_pos % cfg.sliding_window_size] = probability;
        prob_window_pos++;

        // Calculate average probability over sliding window
        float avg = 0.0f;
        size_t avg_count = prob_window_pos < cfg.sliding_window_size ?
                           prob_window_pos :
                           cfg.sliding_window_size;

        if (avg_count > 0) {
            for (size_t i = 0; i < avg_count; i++) {
                avg += prob_window[i];
            }

            avg /= avg_count;
        }

        if (prob_window_pos >= cfg.sliding_window_size) {
            if (avg >= cfg.probability_cutoff) {
                ESP_LOGI(TAG, "*** WAKE WORD DETECTED: '%s' (avg_prob=%.3f) ***",
                         cfg.wake_word_label, avg);

                // Reset sliding window
                memset(prob_window, 0, cfg.sliding_window_size * sizeof(float));
                prob_window_pos = 0;
                current_stride_step = 0;
                FrontendReset(&frontend_state);
                cooldown_until = xTaskGetTickCount() + pdMS_TO_TICKS(MWW_DETECTION_COOLDOWN_MS);

                // Callback
                if (cfg.on_detected) {
                    cfg.on_detected(cfg.wake_word_label);
                }
            }
        }
    }

    free(i2s_buf);
    free(mono_buf);
    ESP_LOGI(TAG, "Detection task stopped");
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
extern "C" {

esp_err_t mww_init(const mww_config_t *config)
{
    cfg = *config;

    // Apply defaults
    if (cfg.probability_cutoff <= 0.0f) cfg.probability_cutoff = 0.97f;  // okay_nabu.json
    if (cfg.sliding_window_size == 0) cfg.sliding_window_size = 5;
    if (cfg.tensor_arena_size == 0) cfg.tensor_arena_size = 49152;
    if (cfg.gain_factor == 0) cfg.gain_factor = 1;
    if (cfg.input_channel > 1) cfg.input_channel = 0;

    if (!cfg.model_data || cfg.model_size == 0) {
        ESP_LOGE(TAG, "No model data provided");
        return ESP_ERR_INVALID_ARG;
    }

    if (!init_frontend()) {
        return ESP_FAIL;
    }

    if (!init_model()) {
        return ESP_FAIL;
    }

    mww_initialized = true;
    ESP_LOGI(TAG, "MWW initialized: '%s', threshold=%.2f, window=%d, gain=%u, channel=%u",
             cfg.wake_word_label, cfg.probability_cutoff, cfg.sliding_window_size,
             cfg.gain_factor, cfg.input_channel);
    return ESP_OK;
}

esp_err_t mww_start(void)
{
    if (!mww_initialized) {
        ESP_LOGE(TAG, "mww_start called before successful mww_init, ignoring");
        return ESP_ERR_INVALID_STATE;
    }
    if (running) return ESP_OK;
    running = true;
    current_stride_step = 0;
    prob_window_pos = 0;

    xTaskCreatePinnedToCore(mww_detection_task, "mww_detect", 8192, NULL, 8, NULL, 0);
    return ESP_OK;
}

esp_err_t mww_stop(void)
{
    running = false;
    // Task will exit on next loop iteration
    vTaskDelay(pdMS_TO_TICKS(200));
    return ESP_OK;
}

esp_err_t mww_set_probability_cutoff(float probability_cutoff)
{
    if (!mww_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (probability_cutoff <= 0.0f || probability_cutoff >= 1.0f) {
        return ESP_ERR_INVALID_ARG;
    }

    cfg.probability_cutoff = probability_cutoff;
    if (prob_window) {
        memset(prob_window, 0, cfg.sliding_window_size * sizeof(float));
        prob_window_pos = 0;
    }
    ESP_LOGI(TAG, "MWW threshold updated: '%s' cutoff=%.2f",
             cfg.wake_word_label, cfg.probability_cutoff);
    return ESP_OK;
}

bool mww_is_running(void)
{
    return running;
}

}  // extern "C"
