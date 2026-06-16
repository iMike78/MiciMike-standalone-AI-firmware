/**
 * Micro Wake Word Detection - Implementation
 *
 * One TFLite Micro interpreter, one audio frontend, fed from a single
 * XMOS output channel (cfg.input_channel). The XMOS firmware exposes
 * channel 1 as the NS-stage tap; that is what the microWakeWord models
 * were trained on, and what ESPHome's HA Voice PE config uses.
 *
 * Streaming microWakeWord v2 contract:
 *   - 16kHz mono PCM, 30ms window, 10ms stride
 *   - 40 mel features per frame
 *   - int8 quantized input, sliding window of probabilities
 */

#include "micro_wake_word.h"
#include "i2s_hal.h"
#include "app_config.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_allocator.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "microfrontend/frontend.h"
#include "microfrontend/frontend_util.h"

#include <cstring>
#include <cmath>
#include <cstdint>
#include <new>

static const char *TAG = "mww";

#define MWW_SAMPLE_RATE         16000
#define MWW_WINDOW_MS           30
#define MWW_STRIDE_MS           10
#define MWW_WINDOW_SAMPLES      (MWW_SAMPLE_RATE * MWW_WINDOW_MS / 1000)   // 480
#define MWW_STRIDE_SAMPLES      (MWW_SAMPLE_RATE * MWW_STRIDE_MS / 1000)   // 160
#define MWW_NUM_FEATURES        40
#define MWW_DETECTION_COOLDOWN_MS 2500
#define MWW_STARTUP_DRAIN_MS    1200
#define MWW_LOWER_FREQ          125
#define MWW_UPPER_FREQ          7500

#define I2S_READ_SIZE           (MWW_STRIDE_SAMPLES * 2 * sizeof(int32_t))

static mww_config_t cfg;
static volatile bool running = false;
static bool mww_initialized = false;

typedef struct {
    const char *label;
    const uint8_t *model_data;
    size_t model_size;
    float probability_cutoff;
    size_t tensor_arena_size;
    bool enabled;
    bool output_info_logged;
    const tflite::Model *model;
    tflite::MicroInterpreter *interpreter;
    uint8_t *tensor_arena;
    float *prob_window;
    size_t prob_window_pos;
    size_t features_per_inference;
    size_t current_stride_step;
    float log_peak_probability;
    float log_peak_average;
} detector_model_t;

static detector_model_t wake_model;
static detector_model_t stop_model;

static struct FrontendState frontend_state;

static tflite::MicroMutableOpResolver<13> *get_resolver(void)
{
    static tflite::MicroMutableOpResolver<13> resolver;
    static bool initialized = false;
    if (!initialized) {
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
        initialized = true;
    }
    return &resolver;
}

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

    ESP_LOGI(TAG, "Audio frontend ready: %d features, %dms window, %dms stride",
             MWW_NUM_FEATURES, MWW_WINDOW_MS, MWW_STRIDE_MS);
    return true;
}

static bool init_model(detector_model_t *detector)
{
    detector->model = tflite::GetModel(detector->model_data);
    if (detector->model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "%s schema version mismatch: got %lu, expected %d",
                 detector->label, detector->model->version(), TFLITE_SCHEMA_VERSION);
        return false;
    }

    detector->tensor_arena = (uint8_t *)heap_caps_malloc(detector->tensor_arena_size,
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!detector->tensor_arena) {
        ESP_LOGE(TAG, "Failed to allocate tensor arena for %s (%d bytes)",
                 detector->label, detector->tensor_arena_size);
        return false;
    }

    constexpr int kNumResourceVariables = 8;

    tflite::MicroAllocator *allocator =
        tflite::MicroAllocator::Create(detector->tensor_arena, detector->tensor_arena_size);

    tflite::MicroResourceVariables *resource_variables =
        tflite::MicroResourceVariables::Create(allocator, kNumResourceVariables);

    void *interpreter_mem = heap_caps_malloc(sizeof(tflite::MicroInterpreter),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!interpreter_mem) {
        ESP_LOGE(TAG, "Failed to allocate interpreter for %s", detector->label);
        return false;
    }

    detector->interpreter = new (interpreter_mem) tflite::MicroInterpreter(
        detector->model,
        *get_resolver(),
        allocator,
        resource_variables);

    if (detector->interpreter->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "Failed to allocate tensors for %s", detector->label);
        return false;
    }

    TfLiteTensor *input = detector->interpreter->input(0);
    if (!input) {
        ESP_LOGE(TAG, "Failed to get input tensor for %s", detector->label);
        return false;
    }

    ESP_LOGI(TAG, "%s input: dims=%d, type=%d", detector->label, input->dims->size, input->type);
    for (int i = 0; i < input->dims->size; i++) {
        ESP_LOGI(TAG, "  dim[%d] = %d", i, input->dims->data[i]);
    }

    if (input->dims->size == 2) {
        detector->features_per_inference = 1;
    } else if (input->dims->size == 4) {
        detector->features_per_inference = input->dims->data[1];
    } else if (input->dims->size == 3) {
        detector->features_per_inference = input->dims->data[1];
    } else {
        detector->features_per_inference = 1;
    }

    detector->prob_window = (float *)heap_caps_calloc(cfg.sliding_window_size, sizeof(float),
                                                      MALLOC_CAP_SPIRAM);
    if (!detector->prob_window) {
        ESP_LOGE(TAG, "Failed to allocate probability window for %s", detector->label);
        return false;
    }

    detector->enabled = true;
    ESP_LOGI(TAG, "Model loaded: %s, threshold=%.2f arena=%d bytes, features_per_inf=%d",
             detector->label, detector->probability_cutoff,
             detector->tensor_arena_size, detector->features_per_inference);
    return true;
}

// Extract one channel of stereo 32-bit (MSB-aligned, Q31 from XMOS) as int16.
// The XMOS NS-stage output sits in the top 16 bits, so a simple shift gives
// us a clean signed 16-bit sample. Apply gain afterwards so the wake model
// sees levels comparable to its training data.
static void extract_mono_16bit(const int32_t *stereo_32, int16_t *mono_16,
                               size_t frames, uint8_t channel,
                               size_t *out_clipped)
{
    size_t clipped = 0;
    for (size_t i = 0; i < frames; i++) {
        int32_t sample = stereo_32[i * 2 + channel] >> 16;
        sample *= cfg.gain_factor;
        if (sample > 32767) {
            sample = 32767;
            clipped++;
        } else if (sample < -32768) {
            sample = -32768;
            clipped++;
        }
        mono_16[i] = (int16_t)sample;
    }
    if (out_clipped) {
        *out_clipped = clipped;
    }
}

// Streaming inference: write one feature slice into the input tensor at the
// current stride slot, only Invoke() when the model's window is full.
// Returns probability in [0..1], or -1.0 if not yet ready.
static float streaming_inference(detector_model_t *detector, const int8_t *features)
{
    TfLiteTensor *input = detector->interpreter->input(0);
    int8_t *tensor_data = tflite::GetTensorData<int8_t>(input);
    std::memmove(tensor_data + MWW_NUM_FEATURES * detector->current_stride_step,
                 features, MWW_NUM_FEATURES);
    detector->current_stride_step++;

    if (detector->current_stride_step < detector->features_per_inference) {
        return -1.0f;
    }
    detector->current_stride_step = 0;

    if (detector->interpreter->Invoke() != kTfLiteOk) {
        ESP_LOGE(TAG, "Inference failed for %s", detector->label);
        return 0.0f;
    }

    TfLiteTensor *output = detector->interpreter->output(0);

    if (!detector->output_info_logged) {
        ESP_LOGI(TAG, "%s output: dims=%d, type=%d, scale=%f, zero_point=%d",
                 detector->label, output->dims->size, output->type,
                 output->params.scale, output->params.zero_point);
        for (int i = 0; i < output->dims->size; i++) {
            ESP_LOGI(TAG, "  out_dim[%d] = %d", i, output->dims->data[i]);
        }
        detector->output_info_logged = true;
    }

    if (output->type == kTfLiteUInt8) {
        return output->data.uint8[0] / 255.0f;
    } else if (output->type == kTfLiteInt8) {
        int8_t raw_out = output->data.int8[0];
        return ((int32_t)raw_out - output->params.zero_point) * output->params.scale;
    } else if (output->type == kTfLiteFloat32) {
        return output->data.f[0];
    }
    return 0.0f;
}

static bool evaluate_detector(detector_model_t *detector, const int8_t *features)
{
    if (!detector->enabled) {
        return false;
    }

    float probability = streaming_inference(detector, features);
    if (probability < 0.0f) {
        return false;
    }

    detector->prob_window[detector->prob_window_pos % cfg.sliding_window_size] = probability;
    detector->prob_window_pos++;

    float avg = 0.0f;
    size_t avg_count = detector->prob_window_pos < cfg.sliding_window_size
                       ? detector->prob_window_pos
                       : cfg.sliding_window_size;
    if (avg_count > 0) {
        for (size_t i = 0; i < avg_count; i++) {
            avg += detector->prob_window[i];
        }
        avg /= avg_count;
    }

    if (detector->prob_window_pos >= cfg.sliding_window_size &&
        avg >= detector->probability_cutoff) {
        ESP_LOGI(TAG, "*** MWW DETECTED: '%s' (avg_prob=%.3f) ***",
                 detector->label, avg);
        memset(detector->prob_window, 0, cfg.sliding_window_size * sizeof(float));
        detector->prob_window_pos = 0;
        detector->current_stride_step = 0;
        return true;
    }

    if (probability > detector->log_peak_probability) {
        detector->log_peak_probability = probability;
    }
    if (avg > detector->log_peak_average) {
        detector->log_peak_average = avg;
    }
    return false;
}

static bool feed_frame(const int16_t *mono_buf, size_t frames_read)
{
    size_t num_samples_read = 0;
    struct FrontendOutput frontend_output = FrontendProcessSamples(
        &frontend_state, mono_buf, frames_read, &num_samples_read);

    if (frontend_output.size != MWW_NUM_FEATURES) {
        return false;
    }

    // microWakeWord int8 quantization: input = (feature * 256) / 666 - 128
    // (666 = round(25.6 * 26.0); from the reference implementation).
    constexpr int32_t value_scale = 256;
    constexpr int32_t value_div = 666;

    int8_t features[MWW_NUM_FEATURES];
    for (int i = 0; i < MWW_NUM_FEATURES; i++) {
        uint16_t raw = frontend_output.values[i];
        int32_t quantized = ((int32_t)raw * value_scale + value_div / 2) /
                            value_div + INT8_MIN;
        if (quantized < -128) quantized = -128;
        if (quantized > 127) quantized = 127;
        features[i] = (int8_t)quantized;
    }

    bool stop_hit = evaluate_detector(&stop_model, features);
    if (stop_hit) {
        if (cfg.on_stop_detected) {
            cfg.on_stop_detected("stop");
        }
    }
    return evaluate_detector(&wake_model, features);
}

static void mww_detection_task(void *arg)
{
    int32_t *i2s_buf = (int32_t *)heap_caps_malloc(I2S_READ_SIZE,
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *mono_buf = (int16_t *)heap_caps_malloc(MWW_STRIDE_SAMPLES * sizeof(int16_t),
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!i2s_buf || !mono_buf) {
        ESP_LOGE(TAG, "Failed to allocate audio buffers");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Detection task started: word='%s' channel=%u gain=%u cutoff=%.2f",
             cfg.wake_word_label, cfg.input_channel, cfg.gain_factor,
             cfg.probability_cutoff);

    TickType_t cooldown_until = 0;
    TickType_t startup_drain_until = xTaskGetTickCount() + pdMS_TO_TICKS(MWW_STARTUP_DRAIN_MS);
    int64_t last_log_ms = esp_timer_get_time() / 1000;
    uint32_t sample_count = 0;
    uint64_t abs_sum = 0;
    int32_t peak = 0;
    uint32_t clipped_total = 0;

    while (running) {
        size_t bytes_read = 0;
        esp_err_t ret = mm_i2s_read(i2s_buf, I2S_READ_SIZE, &bytes_read, 100);
        if (ret != ESP_OK || bytes_read == 0) {
            continue;
        }

        if ((int32_t)(xTaskGetTickCount() - startup_drain_until) < 0) {
            continue;
        }

        if (cooldown_until != 0 &&
            (int32_t)(xTaskGetTickCount() - cooldown_until) < 0) {
            continue;
        }

        size_t frames_read = bytes_read / (2 * sizeof(int32_t));
        for (size_t i = 0; i < frames_read; i++) {
            int32_t sample = i2s_buf[i * 2 + cfg.input_channel] >> 16;
            int32_t abs_sample = sample < 0 ? -sample : sample;
            abs_sum += abs_sample;
            if (abs_sample > peak) {
                peak = abs_sample;
            }
        }
        sample_count += frames_read;

        size_t frame_clipped = 0;
        extract_mono_16bit(i2s_buf, mono_buf, frames_read, cfg.input_channel,
                           &frame_clipped);
        clipped_total += frame_clipped;

        int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms - last_log_ms >= 5000) {
            uint32_t avg = sample_count > 0 ? (uint32_t)(abs_sum / sample_count) : 0;
            uint32_t clip_x100 = sample_count > 0
                ? (uint32_t)(((uint64_t)clipped_total * 10000ULL) / sample_count)
                : 0;
            ESP_LOGI(TAG,
                     "MWW: ch=%u gain=%u avg=%lu peak=%ld clip=%lu.%02lu%% wake_peak=%.3f wake_avg=%.3f",
                     cfg.input_channel,
                     cfg.gain_factor,
                     (unsigned long)avg,
                     (long)peak,
                     (unsigned long)(clip_x100 / 100),
                     (unsigned long)(clip_x100 % 100),
                     wake_model.log_peak_probability,
                     wake_model.log_peak_average);
            sample_count = 0;
            abs_sum = 0;
            peak = 0;
            clipped_total = 0;
            wake_model.log_peak_probability = 0.0f;
            wake_model.log_peak_average = 0.0f;
            last_log_ms = now_ms;
        }

        if (feed_frame(mono_buf, frames_read)) {
            FrontendReset(&frontend_state);
            cooldown_until = xTaskGetTickCount() + pdMS_TO_TICKS(MWW_DETECTION_COOLDOWN_MS);
            if (cfg.on_detected) {
                cfg.on_detected(cfg.wake_word_label);
            }
        }
    }

    free(i2s_buf);
    free(mono_buf);
    ESP_LOGI(TAG, "Detection task stopped");
    vTaskDelete(NULL);
}

extern "C" {

esp_err_t mww_init(const mww_config_t *config)
{
    cfg = *config;

    if (cfg.probability_cutoff <= 0.0f) cfg.probability_cutoff = 0.97f;
    if (cfg.stop_model_data && cfg.stop_probability_cutoff <= 0.0f) {
        cfg.stop_probability_cutoff = 0.5f;
    }
    if (cfg.sliding_window_size == 0) cfg.sliding_window_size = 5;
    if (cfg.tensor_arena_size == 0) cfg.tensor_arena_size = 49152;
    if (cfg.gain_factor == 0) cfg.gain_factor = 1;
    if (cfg.input_channel > 1) cfg.input_channel = 1;

    if (!cfg.model_data || cfg.model_size == 0) {
        ESP_LOGE(TAG, "No model data provided");
        return ESP_ERR_INVALID_ARG;
    }

    if (!init_frontend()) {
        return ESP_FAIL;
    }

    memset(&wake_model, 0, sizeof(wake_model));
    wake_model.label = cfg.wake_word_label;
    wake_model.model_data = cfg.model_data;
    wake_model.model_size = cfg.model_size;
    wake_model.probability_cutoff = cfg.probability_cutoff;
    wake_model.tensor_arena_size = cfg.tensor_arena_size;
    if (!init_model(&wake_model)) {
        return ESP_FAIL;
    }

    memset(&stop_model, 0, sizeof(stop_model));
    if (cfg.stop_model_data && cfg.stop_model_size > 0) {
        stop_model.label = "stop";
        stop_model.model_data = cfg.stop_model_data;
        stop_model.model_size = cfg.stop_model_size;
        stop_model.probability_cutoff = cfg.stop_probability_cutoff;
        stop_model.tensor_arena_size = cfg.tensor_arena_size;
        if (!init_model(&stop_model)) {
            ESP_LOGW(TAG, "Stop model init failed; continuing with wake model only");
        }
    }

    mww_initialized = true;
    ESP_LOGI(TAG, "MWW initialized: '%s' threshold=%.2f stop=%d window=%d gain=%u channel=%u",
             cfg.wake_word_label, cfg.probability_cutoff, stop_model.enabled ? 1 : 0,
             cfg.sliding_window_size, cfg.gain_factor, cfg.input_channel);
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
    FrontendReset(&frontend_state);

    wake_model.current_stride_step = 0;
    wake_model.prob_window_pos = 0;
    wake_model.log_peak_probability = 0.0f;
    wake_model.log_peak_average = 0.0f;
    if (wake_model.prob_window) {
        memset(wake_model.prob_window, 0, cfg.sliding_window_size * sizeof(float));
    }
    // microWakeWord uses VarHandle/ReadVariable/AssignVariable to carry
    // streaming state across Invoke() calls. Without Reset() that state
    // bleeds from the previous session into the new one, and the first
    // post-restart inferences return probabilities ~1.0 — a phantom wake
    // fires in dead silence the moment MWW comes back up.
    if (wake_model.interpreter) {
        wake_model.interpreter->Reset();
    }

    stop_model.current_stride_step = 0;
    stop_model.prob_window_pos = 0;
    if (stop_model.prob_window) {
        memset(stop_model.prob_window, 0, cfg.sliding_window_size * sizeof(float));
    }
    if (stop_model.interpreter && stop_model.enabled) {
        stop_model.interpreter->Reset();
    }

    xTaskCreatePinnedToCore(mww_detection_task, "mww_detect", 8192, NULL, 8, NULL, 0);
    return ESP_OK;
}

esp_err_t mww_stop(void)
{
    running = false;
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
    wake_model.probability_cutoff = probability_cutoff;
    if (wake_model.prob_window) {
        memset(wake_model.prob_window, 0, cfg.sliding_window_size * sizeof(float));
        wake_model.prob_window_pos = 0;
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
