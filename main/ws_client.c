/**
 * OpenAI Realtime API WebSocket Client - Implementation
 *
 * Uses esp_websocket_client for the WebSocket connection.
 * Audio is base64-encoded PCM16 for send, base64-decoded for receive.
 *
 * Key events handled:
 *   - session.created → ready
 *   - input_audio_buffer.speech_started → user speaking
 *   - input_audio_buffer.speech_stopped → user stopped
 *   - response.audio.delta → audio chunk (base64 PCM16)
 *   - response.audio.done → response complete
 *   - error → log and handle
 */

#include "ws_client.h"
#include "app_config.h"
#include "esp_websocket_client.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "ws_client";

static esp_websocket_client_handle_t ws_handle = NULL;
static ws_client_config_t client_cfg;
static volatile ws_state_t current_state = WS_STATE_DISCONNECTED;

// Base64 encode buffer (for sending audio)
// 30ms of 24kHz mono PCM16 = 720 samples * 2 bytes = 1440 bytes
// Base64 expands by 4/3 → ~1920 bytes + JSON overhead
#define SEND_JSON_BUF_SIZE  4096
#define RECV_AUDIO_BUF_SIZE 32768

static char *send_buf = NULL;
static char *b64_buf = NULL;
static uint8_t *decode_buf = NULL;
static char *recv_msg_buf = NULL;
static size_t recv_msg_buf_size = 0;
static SemaphoreHandle_t ws_lock = NULL;
static portMUX_TYPE usage_lock = portMUX_INITIALIZER_UNLOCKED;
static ws_usage_info_t last_usage = {0};
static char last_function_call_id[64] = {0};
static volatile bool web_lookup_in_flight = false;

typedef struct {
    char call_id[64];
    char query[512];
} web_lookup_task_arg_t;

// ---------------------------------------------------------------------------
// Base64 helpers
// ---------------------------------------------------------------------------
static size_t base64_encode_audio(const int16_t *pcm, size_t samples,
                                  char *out, size_t out_size)
{
    size_t olen = 0;
    mbedtls_base64_encode((unsigned char *)out, out_size, &olen,
                          (const unsigned char *)pcm, samples * sizeof(int16_t));
    return olen;
}

static size_t base64_decode_audio(const char *b64, size_t b64_len,
                                  uint8_t *out, size_t out_size)
{
    size_t olen = 0;
    int ret = mbedtls_base64_decode(out, out_size, &olen,
                                    (const unsigned char *)b64, b64_len);
    if (ret != 0) {
        ESP_LOGE(TAG, "Base64 decode error: %d b64_len=%u out_size=%u needed=%u",
                 ret, (unsigned)b64_len, (unsigned)out_size, (unsigned)olen);
        return 0;
    }
    return olen;
}

static void log_response_usage(cJSON *root)
{
    ws_usage_info_t snapshot = {0};
    bool usage_found = false;

    portENTER_CRITICAL(&usage_lock);
    snapshot.response_count = last_usage.response_count + 1;
    portEXIT_CRITICAL(&usage_lock);

    cJSON *usage = cJSON_GetObjectItem(root, "usage");
    if (!usage) {
        cJSON *response = cJSON_GetObjectItem(root, "response");
        if (response) {
            usage = cJSON_GetObjectItem(response, "usage");
        }
    }

    if (!usage) {
        ESP_LOGW(TAG, "API response done usage: <missing>");
    } else {
        usage_found = true;
        snapshot.has_usage = true;

        cJSON *input_tokens = cJSON_GetObjectItem(usage, "input_tokens");
        cJSON *output_tokens = cJSON_GetObjectItem(usage, "output_tokens");
        cJSON *total_tokens = cJSON_GetObjectItem(usage, "total_tokens");

        if (input_tokens && cJSON_IsNumber(input_tokens)) {
            snapshot.input_tokens = (uint32_t)input_tokens->valuedouble;
        }
        if (output_tokens && cJSON_IsNumber(output_tokens)) {
            snapshot.output_tokens = (uint32_t)output_tokens->valuedouble;
        }
        if (total_tokens && cJSON_IsNumber(total_tokens)) {
            snapshot.total_tokens = (uint32_t)total_tokens->valuedouble;
        }

        char *usage_json = cJSON_PrintUnformatted(usage);
        if (usage_json) {
            ESP_LOGI(TAG, "API response done usage: %s", usage_json);
            strncpy(snapshot.raw_json, usage_json, sizeof(snapshot.raw_json) - 1);
            snapshot.raw_json[sizeof(snapshot.raw_json) - 1] = '\0';
            cJSON_free(usage_json);
        } else {
            ESP_LOGW(TAG, "API response done usage: <print failed>");
        }
    }

    portENTER_CRITICAL(&usage_lock);
    if (usage_found) {
        last_usage = snapshot;
    } else {
        last_usage.response_count = snapshot.response_count;
        last_usage.has_usage = false;
    }
    portEXIT_CRITICAL(&usage_lock);
}

// ---------------------------------------------------------------------------
// Send session configuration
// ---------------------------------------------------------------------------
static const char *safe_voice(void)
{
    const char *voice = client_cfg.voice && client_cfg.voice[0]
                      ? client_cfg.voice
                      : DEFAULT_REALTIME_VOICE;
    static const char *const valid[] = {
        "alloy", "ash", "ballad", "coral", "echo",
        "sage", "shimmer", "verse", "marin", "cedar",
    };
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        if (strcmp(voice, valid[i]) == 0) {
            return voice;
        }
    }
    return DEFAULT_REALTIME_VOICE;
}

static const char *safe_style(void)
{
    return client_cfg.conversation_style && client_cfg.conversation_style[0]
         ? client_cfg.conversation_style
         : DEFAULT_CONV_STYLE;
}

static const char *style_instruction(const char *style)
{
    if (strcmp(style, "professional") == 0) {
        return "Style: professional, precise, structured, and technically careful.";
    }
    if (strcmp(style, "friendly") == 0) {
        return "Style: warm, friendly, calm, and supportive.";
    }
    if (strcmp(style, "honest") == 0) {
        return "Style: direct, honest, and clear; say when something is uncertain.";
    }
    if (strcmp(style, "quirky") == 0) {
        return "Style: playful and unusual, while still being useful and understandable.";
    }
    if (strcmp(style, "efficient") == 0) {
        return "Style: concise, action-oriented, and low on filler.";
    }
    if (strcmp(style, "cynical") == 0) {
        return "Style: dry and mildly cynical, but remain helpful and respectful.";
    }
    return "Style: natural, helpful, and context-aware.";
}

static bool extract_response_text(cJSON *root, char *out, size_t out_size)
{
    if (!root || !out || out_size == 0) {
        return false;
    }
    out[0] = '\0';

    cJSON *output_text = cJSON_GetObjectItem(root, "output_text");
    if (output_text && cJSON_IsString(output_text)) {
        strncpy(out, output_text->valuestring, out_size - 1);
        out[out_size - 1] = '\0';
        return out[0] != '\0';
    }

    cJSON *output = cJSON_GetObjectItem(root, "output");
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, output) {
        cJSON *content = cJSON_GetObjectItem(item, "content");
        cJSON *part = NULL;
        cJSON_ArrayForEach(part, content) {
            cJSON *text = cJSON_GetObjectItem(part, "text");
            if (text && cJSON_IsString(text)) {
                strncpy(out, text->valuestring, out_size - 1);
                out[out_size - 1] = '\0';
                return true;
            }
        }
    }

    return false;
}

static esp_err_t perform_web_lookup(const char *query, char *result, size_t result_size)
{
    if (!query || !query[0] || !result || result_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    result[0] = '\0';

    char auth[300];
    snprintf(auth, sizeof(auth), "Bearer %s", client_cfg.api_key ? client_cfg.api_key : "");

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", "gpt-5.5");
    cJSON_AddNumberToObject(req, "max_output_tokens", 400);
    cJSON *tools = cJSON_CreateArray();
    cJSON *web_tool = cJSON_CreateObject();
    cJSON_AddStringToObject(web_tool, "type", "web_search");
    cJSON_AddItemToArray(tools, web_tool);
    cJSON_AddItemToObject(req, "tools", tools);

    char input[768];
    snprintf(input, sizeof(input),
             "Use web search if needed and answer briefly for a voice assistant. Query: %.650s",
             query);
    cJSON_AddStringToObject(req, "input", input);

    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t cfg = {
        .url = "https://api.openai.com/v1/responses",
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
        .buffer_size = 2048,
        .user_agent = "micimike-ai-fw/1.0",
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        cJSON_free(body);
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_err_t err = esp_http_client_open(client, strlen(body));
    if (err != ESP_OK) {
        snprintf(result, result_size, "Web lookup failed: %s.", esp_err_to_name(err));
        cJSON_free(body);
        esp_http_client_cleanup(client);
        return err;
    }

    int written = esp_http_client_write(client, body, strlen(body));
    cJSON_free(body);
    if (written < 0) {
        snprintf(result, result_size, "Web lookup failed: write failed.");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    esp_http_client_fetch_headers(client);

    int status = esp_http_client_get_status_code(client);
    const int web_resp_cap = 65536;
    char *resp = heap_caps_malloc(web_resp_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!resp) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    int total = 0;
    while (total < web_resp_cap - 1) {
        int r = esp_http_client_read(client, resp + total, web_resp_cap - 1 - total);
        if (r <= 0) {
            break;
        }
        total += r;
    }
    resp[total] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (total >= web_resp_cap - 1) {
        snprintf(result, result_size, "Web lookup failed: response too large.");
        free(resp);
        return ESP_FAIL;
    }

    if (status < 200 || status >= 300) {
        snprintf(result, result_size, "Web lookup failed: OpenAI status %d.", status);
        free(resp);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root) {
        snprintf(result, result_size, "Web lookup failed: could not parse response.");
        return ESP_FAIL;
    }

    bool ok = extract_response_text(root, result, result_size);
    cJSON_Delete(root);
    if (!ok) {
        snprintf(result, result_size, "Web lookup returned no text.");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "web_lookup ok: %.80s", query);
    return ESP_OK;
}

static void send_session_update(void)
{
    const char *voice = safe_voice();
    const char *style = safe_style();
    const char *custom = client_cfg.system_prompt ? client_cfg.system_prompt : "";
    char instructions[1800];

    snprintf(instructions, sizeof(instructions),
             "You are the voice assistant inside a MiciMike device. "
             "Detect and follow the user's spoken language automatically. "
             "If the user asks for real-time translation between languages, do that. "
             "You may control this device only by calling the device_control tool. "
             "Use it for radio playback and volume commands. "
             "For current facts, dates, holidays, news, prices, or anything that may have changed, call web_lookup before answering. "
             "Never start radio playback unless the user's latest request explicitly asks to start, play, resume, or turn on the radio. "
             "Do not infer radio_play from background audio, prior context, or as a follow-up action. "
             "After a successful tool call, briefly confirm the action in the user's language. "
             "%s\n\nCustom system prompt:\n%.1000s",
             style_instruction(style), custom);

    cJSON *event = cJSON_CreateObject();
    cJSON_AddStringToObject(event, "type", "session.update");

    cJSON *session = cJSON_CreateObject();
    cJSON_AddStringToObject(session, "type", "realtime");
    cJSON_AddStringToObject(session, "instructions", instructions);

    cJSON *modalities = cJSON_CreateArray();
    cJSON_AddItemToArray(modalities, cJSON_CreateString("audio"));
    cJSON_AddItemToObject(session, "output_modalities", modalities);

    cJSON *audio = cJSON_CreateObject();
    cJSON *input = cJSON_CreateObject();
    cJSON *input_format = cJSON_CreateObject();
    cJSON_AddStringToObject(input_format, "type", "audio/pcm");
    cJSON_AddNumberToObject(input_format, "rate", OPENAI_SAMPLE_RATE);
    cJSON_AddItemToObject(input, "format", input_format);

    cJSON *turn_detection = cJSON_CreateObject();
    cJSON_AddStringToObject(turn_detection, "type", "semantic_vad");
    cJSON_AddStringToObject(turn_detection, "eagerness", "high");
    cJSON_AddBoolToObject(turn_detection, "create_response", true);
    cJSON_AddBoolToObject(turn_detection, "interrupt_response", true);
    cJSON_AddItemToObject(input, "turn_detection", turn_detection);
    cJSON_AddItemToObject(audio, "input", input);

    cJSON *output = cJSON_CreateObject();
    cJSON *output_format = cJSON_CreateObject();
    cJSON_AddStringToObject(output_format, "type", "audio/pcm");
    cJSON_AddNumberToObject(output_format, "rate", OPENAI_SAMPLE_RATE);
    cJSON_AddItemToObject(output, "format", output_format);
    cJSON_AddStringToObject(output, "voice", voice);
    cJSON_AddItemToObject(audio, "output", output);
    cJSON_AddItemToObject(session, "audio", audio);

    cJSON *tools = cJSON_CreateArray();
    cJSON *tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "type", "function");
    cJSON_AddStringToObject(tool, "name", "device_control");
    cJSON_AddStringToObject(tool, "description",
                            "Control the local MiciMike device: internet radio playback and speaker volume.");

    cJSON *parameters = cJSON_CreateObject();
    cJSON_AddStringToObject(parameters, "type", "object");
    cJSON *properties = cJSON_CreateObject();
    cJSON *action = cJSON_CreateObject();
    cJSON_AddStringToObject(action, "type", "string");
    cJSON *action_enum = cJSON_CreateArray();
    cJSON_AddItemToArray(action_enum, cJSON_CreateString("radio_play"));
    cJSON_AddItemToArray(action_enum, cJSON_CreateString("radio_stop"));
    cJSON_AddItemToArray(action_enum, cJSON_CreateString("volume_set"));
    cJSON_AddItemToArray(action_enum, cJSON_CreateString("volume_delta"));
    cJSON_AddItemToObject(action, "enum", action_enum);
    cJSON_AddStringToObject(action, "description",
                            "radio_play starts the selected station, radio_stop stops playback, volume_set sets absolute volume percent, volume_delta changes volume percent by a signed amount.");
    cJSON_AddItemToObject(properties, "action", action);

    cJSON *value = cJSON_CreateObject();
    cJSON_AddStringToObject(value, "type", "integer");
    cJSON_AddStringToObject(value, "description",
                            "For volume_set use 0..100. For volume_delta use a signed percent change such as -10 or 10. Ignored for radio actions.");
    cJSON_AddItemToObject(properties, "value", value);
    cJSON_AddItemToObject(parameters, "properties", properties);

    cJSON *required = cJSON_CreateArray();
    cJSON_AddItemToArray(required, cJSON_CreateString("action"));
    cJSON_AddItemToObject(parameters, "required", required);
    cJSON_AddBoolToObject(parameters, "additionalProperties", false);
    cJSON_AddItemToObject(tool, "parameters", parameters);
    cJSON_AddItemToArray(tools, tool);

    cJSON *web_lookup = cJSON_CreateObject();
    cJSON_AddStringToObject(web_lookup, "type", "function");
    cJSON_AddStringToObject(web_lookup, "name", "web_lookup");
    cJSON_AddStringToObject(web_lookup, "description",
                            "Look up current public web information for facts that may have changed, such as holidays, dates, news, prices, or schedules.");
    cJSON *web_params = cJSON_CreateObject();
    cJSON_AddStringToObject(web_params, "type", "object");
    cJSON *web_props = cJSON_CreateObject();
    cJSON *query = cJSON_CreateObject();
    cJSON_AddStringToObject(query, "type", "string");
    cJSON_AddStringToObject(query, "description", "The concise web search query.");
    cJSON_AddItemToObject(web_props, "query", query);
    cJSON_AddItemToObject(web_params, "properties", web_props);
    cJSON *web_required = cJSON_CreateArray();
    cJSON_AddItemToArray(web_required, cJSON_CreateString("query"));
    cJSON_AddItemToObject(web_params, "required", web_required);
    cJSON_AddBoolToObject(web_params, "additionalProperties", false);
    cJSON_AddItemToObject(web_lookup, "parameters", web_params);
    cJSON_AddItemToArray(tools, web_lookup);

    cJSON_AddItemToObject(session, "tools", tools);
    cJSON_AddStringToObject(session, "tool_choice", "auto");

    cJSON_AddItemToObject(event, "session", session);

    char *session_json = cJSON_PrintUnformatted(event);
    cJSON_Delete(event);
    if (!session_json) {
        ESP_LOGE(TAG, "Failed to format session.update");
        return;
    }

    esp_websocket_client_send_text(ws_handle, session_json, strlen(session_json), portMAX_DELAY);
    cJSON_free(session_json);
    ESP_LOGI(TAG, "Session config sent (semantic VAD/create_response, PCM16, voice=%s style=%s, device_control tool)",
             voice, style);
}

static void send_function_call_output(const char *call_id, bool ok, const char *message)
{
    if (!ws_handle || !call_id || !call_id[0]) {
        return;
    }

    cJSON *output = cJSON_CreateObject();
    cJSON_AddStringToObject(output, "status", ok ? "ok" : "error");
    cJSON_AddStringToObject(output, "message", message ? message : "");
    char *output_json = cJSON_PrintUnformatted(output);
    cJSON_Delete(output);
    if (!output_json) {
        ESP_LOGE(TAG, "Failed to format function_call_output payload");
        return;
    }

    cJSON *event = cJSON_CreateObject();
    cJSON_AddStringToObject(event, "type", "conversation.item.create");
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "function_call_output");
    cJSON_AddStringToObject(item, "call_id", call_id);
    cJSON_AddStringToObject(item, "output", output_json);
    cJSON_AddItemToObject(event, "item", item);

    char *event_json = cJSON_PrintUnformatted(event);
    cJSON_Delete(event);
    cJSON_free(output_json);

    if (!event_json) {
        ESP_LOGE(TAG, "Failed to format function_call_output event");
        return;
    }

    esp_websocket_client_send_text(ws_handle, event_json, strlen(event_json), portMAX_DELAY);
    cJSON_free(event_json);

    const char *response_create = "{\"type\":\"response.create\"}";
    esp_websocket_client_send_text(ws_handle, response_create, strlen(response_create), portMAX_DELAY);
}

static void web_lookup_task(void *arg)
{
    web_lookup_task_arg_t *task_arg = (web_lookup_task_arg_t *)arg;
    char result[1200] = {0};
    esp_err_t ret = ESP_FAIL;

    if (task_arg) {
        ret = perform_web_lookup(task_arg->query, result, sizeof(result));
        ESP_LOGI(TAG, "web_lookup query=%s ret=%s",
                 task_arg->query, esp_err_to_name(ret));
        send_function_call_output(task_arg->call_id, ret == ESP_OK, result);
        free(task_arg);
    }

    web_lookup_in_flight = false;
    vTaskDelete(NULL);
}

static void handle_function_call_item(cJSON *item)
{
    if (!item || !cJSON_IsObject(item)) {
        return;
    }

    cJSON *item_type = cJSON_GetObjectItem(item, "type");
    if (!item_type || !cJSON_IsString(item_type) ||
        strcmp(item_type->valuestring, "function_call") != 0) {
        return;
    }

    cJSON *name = cJSON_GetObjectItem(item, "name");
    cJSON *call_id = cJSON_GetObjectItem(item, "call_id");
    cJSON *arguments = cJSON_GetObjectItem(item, "arguments");
    if (!name || !cJSON_IsString(name) ||
        !call_id || !cJSON_IsString(call_id) ||
        !arguments || !cJSON_IsString(arguments)) {
        ESP_LOGW(TAG, "Malformed function_call item");
        return;
    }

    if (strcmp(call_id->valuestring, last_function_call_id) == 0) {
        ESP_LOGW(TAG, "Ignoring duplicate function_call: %s", call_id->valuestring);
        return;
    }
    strncpy(last_function_call_id, call_id->valuestring, sizeof(last_function_call_id) - 1);
    last_function_call_id[sizeof(last_function_call_id) - 1] = '\0';

    if (strcmp(name->valuestring, "device_control") != 0 &&
        strcmp(name->valuestring, "web_lookup") != 0) {
        ESP_LOGW(TAG, "Unknown function_call: %s", name->valuestring);
        send_function_call_output(call_id->valuestring, false, "Unknown function.");
        return;
    }

    cJSON *args = cJSON_Parse(arguments->valuestring);
    if (!args) {
        ESP_LOGW(TAG, "Failed to parse device_control arguments: %s", arguments->valuestring);
        send_function_call_output(call_id->valuestring, false, "Invalid arguments JSON.");
        return;
    }

    char result[1200] = {0};
    esp_err_t ret = ESP_ERR_INVALID_ARG;

    if (strcmp(name->valuestring, "web_lookup") == 0) {
        cJSON *query = cJSON_GetObjectItem(args, "query");
        const char *query_str = (query && cJSON_IsString(query)) ? query->valuestring : NULL;
        if (query_str && query_str[0]) {
            if (web_lookup_in_flight) {
                snprintf(result, sizeof(result), "Web lookup is already running.");
                ret = ESP_ERR_INVALID_STATE;
                cJSON_Delete(args);
                send_function_call_output(call_id->valuestring, false, result);
                return;
            }
            web_lookup_task_arg_t *task_arg = calloc(1, sizeof(*task_arg));
            if (task_arg) {
                strncpy(task_arg->call_id, call_id->valuestring, sizeof(task_arg->call_id) - 1);
                strncpy(task_arg->query, query_str, sizeof(task_arg->query) - 1);
                current_state = WS_STATE_TOOL_RUNNING;
                if (client_cfg.state_cb) client_cfg.state_cb(WS_STATE_TOOL_RUNNING);
                web_lookup_in_flight = true;
                BaseType_t ok = xTaskCreatePinnedToCore(web_lookup_task, "web_lookup",
                                                        8192, task_arg, 6, NULL, 0);
                if (ok == pdPASS) {
                    cJSON_Delete(args);
                    return;
                }
                web_lookup_in_flight = false;
                free(task_arg);
            }
            snprintf(result, sizeof(result), "Web lookup failed: could not start task.");
            ret = ESP_FAIL;
        } else {
            snprintf(result, sizeof(result), "Web lookup failed: missing query.");
            ret = ESP_ERR_INVALID_ARG;
        }
    } else {
        cJSON *action = cJSON_GetObjectItem(args, "action");
        cJSON *value = cJSON_GetObjectItem(args, "value");
        int int_value = (value && cJSON_IsNumber(value)) ? value->valueint : 0;
        const char *action_str = (action && cJSON_IsString(action)) ? action->valuestring : NULL;

        if (action_str && client_cfg.device_control_cb) {
            ret = client_cfg.device_control_cb(action_str, int_value, result, sizeof(result));
        } else if (!client_cfg.device_control_cb) {
            snprintf(result, sizeof(result), "Device control is not available.");
        } else {
            snprintf(result, sizeof(result), "Missing or invalid action.");
        }

        ESP_LOGI(TAG, "device_control action=%s value=%d ret=%s result=%s",
                 action_str ? action_str : "<invalid>", int_value, esp_err_to_name(ret), result);
    }

    cJSON_Delete(args);
    send_function_call_output(call_id->valuestring, ret == ESP_OK, result);
}

// ---------------------------------------------------------------------------
// Handle incoming messages
// ---------------------------------------------------------------------------
static void handle_ws_message(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse WS message");
        return;
    }

    cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!type || !cJSON_IsString(type)) {
        cJSON_Delete(root);
        return;
    }

    const char *evt = type->valuestring;

    if (strcmp(evt, "session.created") == 0) {
        ESP_LOGI(TAG, "Session created");
        send_session_update();

    } else if (strcmp(evt, "session.updated") == 0) {
        ESP_LOGI(TAG, "Session config confirmed");
        current_state = WS_STATE_CONNECTED;
        if (client_cfg.state_cb) client_cfg.state_cb(WS_STATE_CONNECTED);

    } else if (strcmp(evt, "input_audio_buffer.speech_started") == 0) {
        ESP_LOGI(TAG, "API VAD: speech started");
        current_state = WS_STATE_LISTENING;
        if (client_cfg.state_cb) client_cfg.state_cb(WS_STATE_LISTENING);

    } else if (strcmp(evt, "input_audio_buffer.speech_stopped") == 0) {
        ESP_LOGI(TAG, "API VAD: speech stopped");

    } else if (strcmp(evt, "input_audio_buffer.committed") == 0) {
        ESP_LOGI(TAG, "API input audio committed");

    } else if (strcmp(evt, "response.created") == 0) {
        ESP_LOGI(TAG, "API response created");

    } else if (strcmp(evt, "response.output_item.done") == 0 ||
               strcmp(evt, "conversation.item.done") == 0) {
        cJSON *item = cJSON_GetObjectItem(root, "item");
        if (item) {
            handle_function_call_item(item);
        } else {
            ESP_LOGI(TAG, "API event: %s", evt);
        }

    } else if (strcmp(evt, "response.output_audio.delta") == 0 ||
               strcmp(evt, "response.audio.delta") == 0) {
        // Incoming audio chunk
        if (current_state != WS_STATE_RESPONDING) {
            current_state = WS_STATE_RESPONDING;
            if (client_cfg.state_cb) client_cfg.state_cb(WS_STATE_RESPONDING);
        }

        cJSON *delta = cJSON_GetObjectItem(root, "delta");
        if (delta && cJSON_IsString(delta)) {
            size_t decoded = base64_decode_audio(delta->valuestring,
                                                  strlen(delta->valuestring),
                                                  decode_buf, RECV_AUDIO_BUF_SIZE);
            if (decoded > 0 && client_cfg.audio_cb) {
                client_cfg.audio_cb((const int16_t *)decode_buf, decoded / sizeof(int16_t));
            }
        }

    } else if (strcmp(evt, "response.output_audio.done") == 0 ||
               strcmp(evt, "response.audio.done") == 0) {
        ESP_LOGI(TAG, "API response audio complete");
        current_state = WS_STATE_CONNECTED;
        if (client_cfg.state_cb) client_cfg.state_cb(WS_STATE_CONNECTED);

    } else if (strcmp(evt, "response.done") == 0) {
        ESP_LOGI(TAG, "API response done");
        log_response_usage(root);

    } else if (strcmp(evt, "error") == 0) {
        cJSON *err = cJSON_GetObjectItem(root, "error");
        if (err) {
            cJSON *msg = cJSON_GetObjectItem(err, "message");
            ESP_LOGE(TAG, "API error: %s", msg ? msg->valuestring : "unknown");
            char *err_json = cJSON_PrintUnformatted(err);
            if (err_json) {
                ESP_LOGE(TAG, "API error object: %s", err_json);
                cJSON_free(err_json);
            }
        }
        current_state = WS_STATE_ERROR;
        if (client_cfg.state_cb) client_cfg.state_cb(WS_STATE_ERROR);
    } else {
        ESP_LOGI(TAG, "API event: %s", evt);
    }

    cJSON_Delete(root);
}

static void reset_ws_message_buffer(void)
{
    if (recv_msg_buf) {
        free(recv_msg_buf);
        recv_msg_buf = NULL;
    }
    recv_msg_buf_size = 0;
}

static void handle_ws_text_event(const esp_websocket_event_data_t *data)
{
    if (data->data_len <= 0) {
        return;
    }

    bool fragmented = (data->payload_len > data->data_len) ||
                      (data->payload_offset > 0);
    if (!fragmented) {
        handle_ws_message(data->data_ptr, data->data_len);
        return;
    }

    if (data->payload_len <= 0 || data->payload_len > 65536) {
        ESP_LOGW(TAG, "Dropping oversized/invalid WS payload: len=%d offset=%d chunk=%d",
                 data->payload_len, data->payload_offset, data->data_len);
        reset_ws_message_buffer();
        return;
    }

    size_t need = (size_t)data->payload_len + 1;
    if (recv_msg_buf_size < need) {
        char *new_buf = heap_caps_realloc(recv_msg_buf, need,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!new_buf) {
            ESP_LOGE(TAG, "Failed to allocate WS message buffer (%u bytes)", (unsigned)need);
            reset_ws_message_buffer();
            return;
        }
        recv_msg_buf = new_buf;
        recv_msg_buf_size = need;
    }

    if (data->payload_offset < 0 ||
        data->payload_offset + data->data_len > data->payload_len) {
        ESP_LOGW(TAG, "Invalid WS fragment bounds: len=%d offset=%d chunk=%d",
                 data->payload_len, data->payload_offset, data->data_len);
        reset_ws_message_buffer();
        return;
    }

    memcpy(recv_msg_buf + data->payload_offset, data->data_ptr, data->data_len);

    if (data->payload_offset + data->data_len >= data->payload_len) {
        recv_msg_buf[data->payload_len] = '\0';
        handle_ws_message(recv_msg_buf, data->payload_len);
    }
}

// ---------------------------------------------------------------------------
// WebSocket event handler
// ---------------------------------------------------------------------------
static void ws_event_handler(void *arg, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket connected");
        reset_ws_message_buffer();
        current_state = WS_STATE_CONNECTING;
        break;

    case WEBSOCKET_EVENT_DATA:
        if ((data->op_code == 0x01 || data->op_code == 0x00) && data->data_len > 0) {
            handle_ws_text_event(data);
        }
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket disconnected");
        reset_ws_message_buffer();
        current_state = WS_STATE_DISCONNECTED;
        if (client_cfg.state_cb) client_cfg.state_cb(WS_STATE_DISCONNECTED);
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket error");
        reset_ws_message_buffer();
        current_state = WS_STATE_ERROR;
        if (client_cfg.state_cb) client_cfg.state_cb(WS_STATE_ERROR);
        break;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
esp_err_t ws_client_init(const ws_client_config_t *config)
{
    client_cfg = *config;

    if (!ws_lock) {
        ws_lock = xSemaphoreCreateMutex();
        if (!ws_lock) {
            ESP_LOGE(TAG, "Failed to create WS mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    send_buf = heap_caps_malloc(SEND_JSON_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    b64_buf = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    decode_buf = heap_caps_malloc(RECV_AUDIO_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!send_buf || !b64_buf || !decode_buf) {
        ESP_LOGE(TAG, "Failed to allocate audio buffers in PSRAM");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "WS client initialized");
    return ESP_OK;
}

esp_err_t ws_client_connect(void)
{
    if (ws_handle) {
        ws_client_disconnect();
    }

    // Build auth header: "Authorization: Bearer sk-..."
    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", client_cfg.api_key);

    esp_websocket_client_config_t ws_cfg = {
        .uri = client_cfg.api_url,
        .headers = NULL,  // set below
        .user_agent = "micimike-ai-fw/1.0 (esp32s3; idf6.0)",
        .buffer_size = 8192,
        .task_stack = 12288,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_reconnect = true,
        .network_timeout_ms = 30000,  // OpenAI realtime handshake + first frame can take 5–15s
    };

    // OpenAI Realtime GA API: only Authorization is required as a custom header.
    // User-Agent goes via ws_cfg.user_agent above — transport_ws already emits
    // its own User-Agent line, so duplicating it here causes Cloudflare to
    // reject the upgrade (this was the cause of the 15s RSTs).
    // Do NOT send OpenAI-Beta: the GA API closes with beta_api_shape_disabled.
    char headers[384];
    snprintf(headers, sizeof(headers),
             "Authorization: %s\r\n",
             auth_header);
    ws_cfg.headers = headers;

    ws_handle = esp_websocket_client_init(&ws_cfg);
    if (!ws_handle) {
        ESP_LOGE(TAG, "Failed to init WS client");
        return ESP_FAIL;
    }

    esp_websocket_register_events(ws_handle, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);

    esp_err_t ret = esp_websocket_client_start(ws_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WS client: %s", esp_err_to_name(ret));
        return ret;
    }

    current_state = WS_STATE_CONNECTING;
    ESP_LOGI(TAG, "Connecting to %s (free_heap=%lu internal=%lu)",
             client_cfg.api_url,
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return ESP_OK;
}

esp_err_t ws_client_disconnect(void)
{
    if (ws_lock) {
        xSemaphoreTake(ws_lock, portMAX_DELAY);
    }
    if (ws_handle) {
        current_state = WS_STATE_CLOSING;
        esp_websocket_client_stop(ws_handle);
        esp_websocket_client_destroy(ws_handle);
        ws_handle = NULL;
    }
    reset_ws_message_buffer();
    current_state = WS_STATE_DISCONNECTED;
    if (ws_lock) {
        xSemaphoreGive(ws_lock);
    }
    return ESP_OK;
}

esp_err_t ws_client_send_audio(const int16_t *pcm, size_t samples)
{
    if (ws_lock) {
        xSemaphoreTake(ws_lock, portMAX_DELAY);
    }
    bool can_send = current_state == WS_STATE_CONNECTED
                 || current_state == WS_STATE_LISTENING
                 || current_state == WS_STATE_RESPONDING
                 || current_state == WS_STATE_TOOL_RUNNING;
    if (!can_send || !ws_handle) {
        if (ws_lock) {
            xSemaphoreGive(ws_lock);
        }
        return ESP_ERR_INVALID_STATE;
    }

    // Encode audio as base64
    size_t b64_len = base64_encode_audio(pcm, samples, b64_buf, 2048);

    // Build JSON: {"type":"input_audio_buffer.append","audio":"<base64>"}
    int json_len = snprintf(send_buf, SEND_JSON_BUF_SIZE,
        "{\"type\":\"input_audio_buffer.append\",\"audio\":\"%.*s\"}",
        (int)b64_len, b64_buf);

    if (json_len >= SEND_JSON_BUF_SIZE) {
        ESP_LOGW(TAG, "Send buffer overflow, audio chunk too large");
        if (ws_lock) {
            xSemaphoreGive(ws_lock);
        }
        return ESP_ERR_NO_MEM;
    }

    int sent = esp_websocket_client_send_text(ws_handle, send_buf, json_len, pdMS_TO_TICKS(250));
    if (ws_lock) {
        xSemaphoreGive(ws_lock);
    }
    return sent > 0 ? ESP_OK : ESP_FAIL;
}

ws_state_t ws_client_get_state(void)
{
    return current_state;
}

void ws_client_get_last_usage(ws_usage_info_t *out)
{
    if (!out) return;

    portENTER_CRITICAL(&usage_lock);
    *out = last_usage;
    portEXIT_CRITICAL(&usage_lock);
}
