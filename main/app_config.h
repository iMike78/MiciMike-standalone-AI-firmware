/**
 * MiciMike AI Firmware - Hardware Configuration
 *
 * Pin assignments derived from ESPHome YAML config.
 * ESP32-S3 + XMOS XU316 voice processing platform.
 */

#pragma once

// ---------------------------------------------------------------------------
// I2C bus (shared: AIC3204 codec + future MPR121 touch controller)
// ---------------------------------------------------------------------------
#define PIN_I2C_SDA             GPIO_NUM_5
#define PIN_I2C_SCL             GPIO_NUM_6
#define I2C_PORT                I2C_NUM_0
#define I2C_FREQ_HZ             400000

// ---------------------------------------------------------------------------
// AIC3204 audio codec (I2C address)
// ---------------------------------------------------------------------------
#define AIC3204_I2C_ADDR        0x18

// ---------------------------------------------------------------------------
// I2S output (ESP32-S3 → AIC3204 → speaker)
// ESP32-S3 is SECONDARY (XMOS provides clocks)
// ---------------------------------------------------------------------------
#define PIN_I2S_OUT_LRCLK       GPIO_NUM_7
#define PIN_I2S_OUT_BCLK        GPIO_NUM_8
#define PIN_I2S_OUT_DOUT        GPIO_NUM_10
#define I2S_OUT_SAMPLE_RATE     48000
#define I2S_OUT_BITS            32

// ---------------------------------------------------------------------------
// I2S input (XMOS → ESP32-S3 microphone stream)
// ESP32-S3 is SECONDARY (XMOS provides clocks)
// ---------------------------------------------------------------------------
#define PIN_I2S_IN_LRCLK        GPIO_NUM_14
#define PIN_I2S_IN_BCLK         GPIO_NUM_13
#define PIN_I2S_IN_DIN          GPIO_NUM_15
#define I2S_IN_SAMPLE_RATE      16000
#define I2S_IN_BITS             32
#define MWW_INPUT_CHANNEL       1       // ESPHome voice_kit uses channel 1 for microWakeWord

// ---------------------------------------------------------------------------
// XMOS control
// ---------------------------------------------------------------------------
#define PIN_XMOS_RESET          GPIO_NUM_45

// ---------------------------------------------------------------------------
// SK6812 LED strip (4 LEDs, GRB order)
// ---------------------------------------------------------------------------
#define PIN_LED_STRIP           GPIO_NUM_21
#define LED_COUNT               4

// ---------------------------------------------------------------------------
// Speaker amplifier enable
// ---------------------------------------------------------------------------
#define PIN_SPEAKER_AMP         GPIO_NUM_18

// ---------------------------------------------------------------------------
// Hardware mute switch (active low with pullup)
// ---------------------------------------------------------------------------
#define PIN_MUTE_SWITCH         GPIO_NUM_1

// ---------------------------------------------------------------------------
// Touch buttons (native ESP32 touch - will migrate to MPR121 later)
// ---------------------------------------------------------------------------
#define PIN_TOUCH_VOL_UP        GPIO_NUM_4
#define PIN_TOUCH_CENTER        GPIO_NUM_3
#define PIN_TOUCH_VOL_DOWN      GPIO_NUM_2
#define TOUCH_THRESH_VOL_UP     8000
#define TOUCH_THRESH_CENTER     5000
#define TOUCH_THRESH_VOL_DOWN   1500

// ---------------------------------------------------------------------------
// Audio pipeline parameters
// ---------------------------------------------------------------------------
#define OPENAI_SAMPLE_RATE      24000       // OpenAI Realtime API expects 24kHz
#define OPENAI_BITS             16          // PCM16
#define OPENAI_CHANNELS         1           // mono

#define AUDIO_BUF_SIZE_MS       30          // audio buffer chunk in ms
#define MIC_BUF_SAMPLES         (I2S_IN_SAMPLE_RATE * AUDIO_BUF_SIZE_MS / 1000)
#define SPK_BUF_SAMPLES         (I2S_OUT_SAMPLE_RATE * AUDIO_BUF_SIZE_MS / 1000)

// ---------------------------------------------------------------------------
// Session parameters
// ---------------------------------------------------------------------------
#define DEFAULT_SESSION_IDLE_TIMEOUT_S 10    // close session after 10s without local speech
#define SESSION_MAX_DURATION_MS     300000  // 5 min max session
#define SESSION_LOCAL_SPEECH_AVG_THRESHOLD 250
#define SESSION_BARGE_IN_AVG_THRESHOLD 900

// ---------------------------------------------------------------------------
// NVS keys
// ---------------------------------------------------------------------------
#define NVS_NAMESPACE           "micimike"
#define NVS_KEY_WIFI_SSID       "wifi_ssid"
#define NVS_KEY_WIFI_PASS       "wifi_pass"
#define NVS_KEY_DEVICE_NAME     "dev_name"
#define NVS_KEY_API_KEY         "api_key"
#define NVS_KEY_ADMIN_API_KEY   "admin_key"
#define NVS_KEY_WAKEWORD        "wakeword"
#define NVS_KEY_WW_SENSITIVITY  "ww_sens"
#define NVS_KEY_VOLUME          "volume"
#define NVS_KEY_API_URL         "api_url"   // for OpenAI-compatible endpoints
#define NVS_KEY_SESSION_TIMEOUT "sess_to_s"
#define NVS_KEY_RADIO_URL       "radio_url"      // legacy single-URL, kept for migration
#define NVS_KEY_RADIO_STATIONS  "radio_stns"     // blob: packed radio_station_t array
#define NVS_KEY_RADIO_CUR_IDX   "radio_idx"      // int8_t: currently selected station, -1 if none
#define NVS_KEY_EQ_PROFILE      "eq_profile"
#define NVS_KEY_RT_VOICE        "rt_voice"
#define NVS_KEY_CONV_STYLE      "conv_style"
#define NVS_KEY_UI_LANGUAGE     "ui_lang"
#define NVS_KEY_SYSTEM_PROMPT   "sys_prompt"

// ---------------------------------------------------------------------------
// Defaults
// ---------------------------------------------------------------------------
#define DEFAULT_WAKEWORD        "okay_nabu"
#define DEFAULT_WW_SENSITIVITY  "moderate"
#define DEFAULT_DEVICE_NAME     "MiciMike"
#define DEFAULT_VOLUME          30          // 0-100
#define DEFAULT_EQ_PROFILE      "Loudness"
#define DEFAULT_REALTIME_VOICE  "marin"
#define DEFAULT_CONV_STYLE      "default"
#define DEFAULT_UI_LANGUAGE     "hu"
#define DEFAULT_SYSTEM_PROMPT   ""
#define DEFAULT_API_URL         "wss://api.openai.com/v1/realtime?model=gpt-realtime-2"
#define CAPTIVE_PORTAL_SSID     "MiciMike-Setup"
