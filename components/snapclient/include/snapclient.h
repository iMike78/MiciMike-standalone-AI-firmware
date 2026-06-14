/**
 * Snapcast client for ESP32-S3 (a.k.a. "Sendspin").
 *
 * Implements just enough of the Snapcast wire protocol
 * (https://github.com/badaix/snapcast) to subscribe to a server's audio
 * stream, decode it (FLAC, PCM; Opus/Ogg planned), and feed it to the
 * device's existing I2S/AIC3204 chain.
 *
 * Usage:
 *   snapclient_config_t cfg = {
 *       .host        = "192.168.1.50",  // or "" for mDNS auto-discovery
 *       .port        = 1704,
 *       .client_name = "MiciMike Kitchen",
 *       .hostname    = "micimike-kitchen",
 *       .unique_id   = "1C:DB:D4:4A:BC:34",
 *   };
 *   snapclient_start(&cfg);
 *   ...
 *   snapclient_stop();
 *
 * The client runs in its own FreeRTOS task; the public functions are
 * non-blocking signals only. Use snapclient_get_state() to observe progress.
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SNAP_STATE_STOPPED = 0,
    SNAP_STATE_DISCOVERING,   // mDNS lookup in progress
    SNAP_STATE_CONNECTING,    // TCP connect in progress
    SNAP_STATE_HANDSHAKING,   // hello / server-settings exchange
    SNAP_STATE_PLAYING,       // receiving audio chunks
    SNAP_STATE_RECONNECTING,  // transient drop, will retry shortly
    SNAP_STATE_ERROR,         // fatal — call snapclient_start again to retry
} snapclient_state_t;

typedef struct {
    /** Server hostname or IP. Empty string → use mDNS to find a server
     *  advertising `_snapcast._tcp.` on the local network. */
    char host[64];

    /** TCP port for the Snapcast stream socket. Default: 1704. */
    uint16_t port;

    /** Human-readable name shown in the Snapcast group UI. */
    char client_name[32];

    /** Network hostname used in the Hello message. */
    char hostname[32];

    /** Stable per-device unique identifier (typically the MAC address as
     *  text, e.g. "1C:DB:D4:4A:BC:34"). Snapcast groups remember our
     *  volume / latency / group membership by this id. */
    char unique_id[18];
} snapclient_config_t;

typedef void (*snapclient_state_cb_t)(snapclient_state_t state, void *user);

/**
 * Spawn the snapclient task with the given config.
 * Replaces any previous instance.
 */
esp_err_t snapclient_start(const snapclient_config_t *cfg);

/**
 * Stop the snapclient and release its task / sockets.
 */
esp_err_t snapclient_stop(void);

/** @return current state — safe to call from any task. */
snapclient_state_t snapclient_get_state(void);

/** @return last error message, or empty string if no error. Pointer is
 *  owned by the client; copy if you need to keep it past the next call. */
const char *snapclient_get_error(void);

/** Register an optional state-change observer (e.g. for the LED ring). */
void snapclient_set_state_cb(snapclient_state_cb_t cb, void *user);

#ifdef __cplusplus
}
#endif
