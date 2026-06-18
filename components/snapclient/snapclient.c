/**
 * Snapcast client — state machine + TCP transport skeleton.
 *
 * Status of this file (iteration 1):
 *   - Public API + state machine wired up
 *   - TCP socket connect to the configured host:port
 *   - Hello → ServerSettings round-trip framed but not yet acting on payload
 *   - Codec header / wire chunk handling is stubbed (logs only)
 *   - Time-sync, decoding, and I2S output still TODO (next iterations)
 *
 * Threading: one client task pinned to core 1 owns the TCP socket. Public
 * functions only signal it via flags / queue — they never touch the socket
 * directly.
 */

#include "snapclient.h"
#include "snap_proto.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "mdns.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/time.h>

static const char *TAG = "snapclient";

#define SNAP_TASK_STACK_BYTES   8192
#define SNAP_TASK_PRIORITY      6
#define SNAP_TASK_CORE          1
#define SNAP_RECV_BUF_BYTES     4096
#define SNAP_RECONNECT_BACKOFF_MS 2000
#define SNAP_RECV_TIMEOUT_S     5
#define SNAP_BASE_HEADER_BYTES  26
#define SNAP_MDNS_QUERY_MS      3000

typedef struct {
    snapclient_config_t cfg;
    volatile bool stop_requested;
    volatile snapclient_state_t state;
    char last_error[160];
    TaskHandle_t task;
    int sock;
    uint16_t next_msg_id;
    snapclient_state_cb_t state_cb;
    void *state_cb_user;
} snap_ctx_t;

static snap_ctx_t s_ctx = { .sock = -1 };
static SemaphoreHandle_t s_ctx_mutex = NULL;

// -------------------------------------------------------------------------
// Small helpers
// -------------------------------------------------------------------------
static void set_state(snapclient_state_t st)
{
    if (s_ctx.state == st) return;
    s_ctx.state = st;
    if (s_ctx.state_cb) s_ctx.state_cb(st, s_ctx.state_cb_user);
    ESP_LOGI(TAG, "state → %d", (int)st);
}

static void set_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_ctx.last_error, sizeof(s_ctx.last_error), fmt, ap);
    va_end(ap);
    ESP_LOGE(TAG, "%s", s_ctx.last_error);
}

static void close_socket(void)
{
    if (s_ctx.sock >= 0) {
        shutdown(s_ctx.sock, SHUT_RDWR);
        close(s_ctx.sock);
        s_ctx.sock = -1;
    }
}

// Read exactly n bytes (or return < 0 on error). Honors stop_requested.
static int recv_exact(uint8_t *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        if (s_ctx.stop_requested) return -2;
        ssize_t r = recv(s_ctx.sock, buf + got, n - got, 0);
        if (r > 0) {
            got += (size_t)r;
            continue;
        }
        if (r == 0) {
            set_error("server closed connection");
            return -1;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // Honor stop request even on idle sockets
            continue;
        }
        set_error("recv error: %s (errno=%d)", strerror(errno), errno);
        return -1;
    }
    return 0;
}

static int send_all(const uint8_t *buf, size_t n)
{
    size_t sent = 0;
    while (sent < n) {
        ssize_t s = send(s_ctx.sock, buf + sent, n - sent, 0);
        if (s > 0) { sent += (size_t)s; continue; }
        if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
        set_error("send error: %s", strerror(errno));
        return -1;
    }
    return 0;
}

// -------------------------------------------------------------------------
// Hello → ServerSettings handshake
// -------------------------------------------------------------------------
static int send_hello(void)
{
    uint8_t payload[640];
    size_t payload_len = snap_pack_hello_payload(payload, sizeof(payload),
                                                  s_ctx.cfg.unique_id,
                                                  s_ctx.cfg.hostname,
                                                  s_ctx.cfg.client_name,
                                                  1, "xtensa-esp32s3");
    if (payload_len == 0) {
        set_error("hello payload too large");
        return -1;
    }

    uint8_t header[SNAP_BASE_HEADER_BYTES];
    struct timeval tv;
    gettimeofday(&tv, NULL);
    snap_pack_base_header(header, SNAP_MSG_HELLO,
                          s_ctx.next_msg_id++, 0,
                          (int32_t)tv.tv_sec, (int32_t)tv.tv_usec,
                          (uint32_t)payload_len);

    if (send_all(header, sizeof(header)) < 0) return -1;
    if (send_all(payload, payload_len) < 0) return -1;
    ESP_LOGI(TAG, "Hello sent (%u bytes payload)", (unsigned)payload_len);
    return 0;
}

// -------------------------------------------------------------------------
// One iteration of the receive loop. Reads a base header + payload and
// dispatches it. The actual decoders for ServerSettings / CodecHeader /
// WireChunk are stubs in this iteration.
// -------------------------------------------------------------------------
static int recv_one_message(uint8_t *scratch, size_t scratch_cap)
{
    uint8_t header_bytes[SNAP_BASE_HEADER_BYTES];
    if (recv_exact(header_bytes, sizeof(header_bytes)) < 0) return -1;

    snap_base_header_t hdr;
    if (snap_parse_base_header(header_bytes, &hdr) < 0) {
        set_error("malformed header");
        return -1;
    }

    if (hdr.size > SNAP_MAX_PAYLOAD_BYTES) {
        set_error("payload too large: %u bytes", (unsigned)hdr.size);
        return -1;
    }

    uint8_t *payload = NULL;
    uint8_t inline_buf[256];
    if (hdr.size > 0) {
        if (hdr.size <= sizeof(inline_buf)) {
            payload = inline_buf;
        } else if (hdr.size <= scratch_cap) {
            payload = scratch;
        } else {
            set_error("scratch too small for %u-byte payload", (unsigned)hdr.size);
            return -1;
        }
        if (recv_exact(payload, hdr.size) < 0) return -1;
    }

    switch ((snap_msg_type_t)hdr.type) {
    case SNAP_MSG_SERVER_SETTINGS:
        // TODO(iter 2): parse JSON, apply latency/buffer/volume/muted.
        ESP_LOGI(TAG, "ServerSettings received (%u bytes) — parsing TODO", (unsigned)hdr.size);
        set_state(SNAP_STATE_PLAYING);  // optimistic until codec header lands
        break;
    case SNAP_MSG_CODEC_HEADER:
        // TODO(iter 2): pick decoder based on payload "codec" string.
        ESP_LOGI(TAG, "CodecHeader received (%u bytes) — decoder hookup TODO", (unsigned)hdr.size);
        break;
    case SNAP_MSG_WIRE_CHUNK:
        // TODO(iter 3): feed to active decoder → resampler → I2S.
        ESP_LOGD(TAG, "WireChunk received (%u bytes) — decode TODO", (unsigned)hdr.size);
        break;
    case SNAP_MSG_TIME:
        // TODO(iter 4): compute round-trip and update local clock offset.
        ESP_LOGD(TAG, "TimeMessage received");
        break;
    case SNAP_MSG_STREAM_TAGS:
        ESP_LOGI(TAG, "StreamTags received (%u bytes)", (unsigned)hdr.size);
        break;
    default:
        ESP_LOGW(TAG, "Unknown message type %u (size=%u) — ignoring",
                 (unsigned)hdr.type, (unsigned)hdr.size);
        break;
    }
    return 0;
}

// -------------------------------------------------------------------------
// TCP connect using a numeric IP or hostname (resolved synchronously).
// -------------------------------------------------------------------------
// mDNS browse for _snapcast._tcp. — fill host_out + port_out with the first
// IPv4 responder we find. Returns 0 on success, -1 on failure (with set_error).
static int discover_snapserver(char *host_out, size_t host_cap, uint16_t *port_out)
{
    set_state(SNAP_STATE_DISCOVERING);
    ESP_LOGI(TAG, "mDNS: querying _snapcast._tcp.");

    mdns_result_t *results = NULL;
    esp_err_t err = mdns_query_ptr("_snapcast", "_tcp", SNAP_MDNS_QUERY_MS, 4, &results);
    if (err != ESP_OK) {
        set_error("mDNS query failed: %s", esp_err_to_name(err));
        return -1;
    }
    if (!results) {
        set_error("No _snapcast._tcp. responders on the network");
        return -1;
    }

    int rc = -1;
    for (mdns_result_t *r = results; r != NULL; r = r->next) {
        for (mdns_ip_addr_t *a = r->addr; a != NULL; a = a->next) {
            if (a->addr.type != ESP_IPADDR_TYPE_V4) continue;
            esp_ip4_addr_t v4 = a->addr.u_addr.ip4;
            snprintf(host_out, host_cap, IPSTR, IP2STR(&v4));
            *port_out = r->port ? r->port : 1704;
            ESP_LOGI(TAG, "mDNS: snapserver at %s:%u (hostname=%s)",
                     host_out, (unsigned)*port_out,
                     r->hostname ? r->hostname : "?");
            rc = 0;
            break;
        }
        if (rc == 0) break;
    }
    mdns_query_results_free(results);
    if (rc != 0) {
        set_error("mDNS responders had no usable IPv4 address");
    }
    return rc;
}

static int tcp_connect(void)
{
    char host[64];
    uint16_t port = s_ctx.cfg.port ? s_ctx.cfg.port : 1704;

    if (s_ctx.cfg.host[0] == '\0') {
        if (discover_snapserver(host, sizeof(host), &port) < 0) {
            return -1;
        }
    } else {
        strncpy(host, s_ctx.cfg.host, sizeof(host) - 1);
        host[sizeof(host) - 1] = '\0';
    }

    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

    int gai = getaddrinfo(host, port_str, &hints, &res);
    if (gai != 0 || !res) {
        set_error("DNS lookup failed for %s: %d", host, gai);
        return -1;
    }

    s_ctx.sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s_ctx.sock < 0) {
        freeaddrinfo(res);
        set_error("socket() failed: %s", strerror(errno));
        return -1;
    }

    struct timeval tv = { .tv_sec = SNAP_RECV_TIMEOUT_S, .tv_usec = 0 };
    setsockopt(s_ctx.sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s_ctx.sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    int nodelay = 1;
    setsockopt(s_ctx.sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    if (connect(s_ctx.sock, res->ai_addr, res->ai_addrlen) < 0) {
        set_error("connect to %s:%u failed: %s",
                  host, (unsigned)port, strerror(errno));
        freeaddrinfo(res);
        close_socket();
        return -1;
    }

    ESP_LOGI(TAG, "TCP connected to %s:%u", host, (unsigned)port);
    freeaddrinfo(res);
    return 0;
}

// -------------------------------------------------------------------------
// Main task — connect, handshake, then dispatch incoming messages until
// stop is requested or the connection drops.
// -------------------------------------------------------------------------
static void snapclient_task(void *arg)
{
    uint8_t *scratch = malloc(SNAP_RECV_BUF_BYTES);
    if (!scratch) {
        set_error("OOM allocating scratch");
        set_state(SNAP_STATE_ERROR);
        s_ctx.task = NULL;
        vTaskDelete(NULL);
        return;
    }

    while (!s_ctx.stop_requested) {
        set_state(SNAP_STATE_CONNECTING);
        if (tcp_connect() < 0) {
            set_state(SNAP_STATE_RECONNECTING);
            vTaskDelay(pdMS_TO_TICKS(SNAP_RECONNECT_BACKOFF_MS));
            continue;
        }

        set_state(SNAP_STATE_HANDSHAKING);
        if (send_hello() < 0) {
            close_socket();
            set_state(SNAP_STATE_RECONNECTING);
            vTaskDelay(pdMS_TO_TICKS(SNAP_RECONNECT_BACKOFF_MS));
            continue;
        }

        // Inner loop: read messages until error/disconnect.
        while (!s_ctx.stop_requested) {
            if (recv_one_message(scratch, SNAP_RECV_BUF_BYTES) < 0) break;
        }

        close_socket();
        if (s_ctx.stop_requested) break;

        set_state(SNAP_STATE_RECONNECTING);
        vTaskDelay(pdMS_TO_TICKS(SNAP_RECONNECT_BACKOFF_MS));
    }

    free(scratch);
    set_state(SNAP_STATE_STOPPED);
    s_ctx.task = NULL;
    vTaskDelete(NULL);
}

// -------------------------------------------------------------------------
// Public API
// -------------------------------------------------------------------------
static void ensure_mutex(void)
{
    if (!s_ctx_mutex) s_ctx_mutex = xSemaphoreCreateMutex();
}

esp_err_t snapclient_start(const snapclient_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    ensure_mutex();
    xSemaphoreTake(s_ctx_mutex, portMAX_DELAY);

    // Stop any previous instance first.
    if (s_ctx.task) {
        s_ctx.stop_requested = true;
        // Try to nudge the recv loop awake by closing the socket.
        close_socket();
        for (int i = 0; i < 40 && s_ctx.task; i++) {
            xSemaphoreGive(s_ctx_mutex);
            vTaskDelay(pdMS_TO_TICKS(50));
            xSemaphoreTake(s_ctx_mutex, portMAX_DELAY);
        }
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.sock = -1;
    s_ctx.cfg = *cfg;
    if (s_ctx.cfg.port == 0) s_ctx.cfg.port = 1704;

    BaseType_t ok = xTaskCreatePinnedToCore(snapclient_task, "snapclient",
                                            SNAP_TASK_STACK_BYTES, NULL,
                                            SNAP_TASK_PRIORITY,
                                            &s_ctx.task, SNAP_TASK_CORE);
    if (ok != pdPASS) {
        s_ctx.task = NULL;
        set_state(SNAP_STATE_ERROR);
        set_error("Failed to create snapclient task");
        xSemaphoreGive(s_ctx_mutex);
        return ESP_FAIL;
    }

    xSemaphoreGive(s_ctx_mutex);
    return ESP_OK;
}

esp_err_t snapclient_stop(void)
{
    ensure_mutex();
    xSemaphoreTake(s_ctx_mutex, portMAX_DELAY);

    if (s_ctx.task) {
        s_ctx.stop_requested = true;
        close_socket();   // unblock recv()
    }
    // If no task was ever started, leave fd 0 alone — the struct's
    // default-zero `sock` would otherwise match a legitimate descriptor.

    xSemaphoreGive(s_ctx_mutex);
    return ESP_OK;
}

snapclient_state_t snapclient_get_state(void)
{
    return s_ctx.state;
}

const char *snapclient_get_error(void)
{
    return s_ctx.last_error;
}

void snapclient_set_state_cb(snapclient_state_cb_t cb, void *user)
{
    s_ctx.state_cb = cb;
    s_ctx.state_cb_user = user;
}
