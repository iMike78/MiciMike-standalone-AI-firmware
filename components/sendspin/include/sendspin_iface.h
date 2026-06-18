/**
 * Sendspin Player — C wrapper around the C++ sendspin-cpp library.
 *
 * Why this wrapper exists:
 *   - sendspin-cpp is C++17, the rest of the firmware is C
 *   - lets us expose a tiny, stable C API to main.c / settings_server.c
 *   - hides the C++ player listener + network provider behind a pcm callback
 *
 * Threading:
 *   sendspin_iface_start() spawns a FreeRTOS task that owns the
 *   SendspinClient instance and drives its loop(). Public functions only
 *   touch flags / config snapshots; they never touch the C++ client object.
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SENDSPIN_STATE_STOPPED = 0,
    SENDSPIN_STATE_STARTING,
    SENDSPIN_STATE_LISTENING,   // WS server up, advertised on mDNS, no server yet
    SENDSPIN_STATE_CONNECTED,   // a Sendspin server has connected, handshake done
    SENDSPIN_STATE_PLAYING,     // active stream
    SENDSPIN_STATE_ERROR,
} sendspin_state_t;

typedef struct {
    /** Human-readable name shown in Music Assistant. */
    char name[32];

    /** Stable per-device unique identifier. */
    char client_id[32];

    /** Optional Sendspin server URL (e.g. "ws://192.168.1.174:8927/sendspin").
     *  If non-empty, we actively connect to the server instead of waiting
     *  for it to discover us via mDNS. Useful as a fallback when mDNS
     *  doesn't propagate across the LAN. */
    char server_url[128];

    /** Initial player volume reported to the Sendspin server, 0-100. */
    uint8_t initial_volume;
} sendspin_iface_config_t;

/** PCM write callback. Called from the library's background sync task
 *  with decoded interleaved samples. Must return the number of bytes
 *  consumed. Returning 0 signals a write failure to the library. */
typedef size_t (*sendspin_pcm_cb_t)(const uint8_t *data, size_t length,
                                    uint32_t timeout_ms, void *user);
typedef void (*sendspin_volume_cb_t)(uint8_t volume, void *user);
typedef void (*sendspin_mute_cb_t)(bool muted, void *user);

void sendspin_iface_set_pcm_cb(sendspin_pcm_cb_t cb, void *user);
void sendspin_iface_set_volume_cb(sendspin_volume_cb_t cb, void *user);
void sendspin_iface_set_mute_cb(sendspin_mute_cb_t cb, void *user);

/** Tell Sendspin that a local, non-Sendspin media source was started.
 *  The wrapper sends a controller STOP command to the server when connected
 *  and suppresses any already-running stream until the next stream/start. */
void sendspin_iface_external_media_started(void);

/** Start the Sendspin player. Idempotent — calling it again with a
 *  different config restarts the player. */
esp_err_t sendspin_iface_start(const sendspin_iface_config_t *cfg);

/** Stop the player. Safe to call when not started. */
esp_err_t sendspin_iface_stop(void);

sendspin_state_t sendspin_iface_get_state(void);

/** Last error message (owned by the wrapper; copy if you need to keep it). */
const char *sendspin_iface_get_error(void);

#ifdef __cplusplus
}
#endif
