/**
 * Minimal internet radio / playback validator.
 *
 * First pass supports HTTP(S) WAV/PCM streams so we can validate the local
 * playback chain independently from the Realtime API.
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RADIO_STATE_STOPPED = 0,
    RADIO_STATE_CONNECTING,
    RADIO_STATE_PLAYING,
    RADIO_STATE_ERROR,
} radio_state_t;

esp_err_t media_radio_start(const char *url);
esp_err_t media_radio_stop(void);
radio_state_t media_radio_get_state(void);
const char *media_radio_get_error(void);

/**
 * Copy the currently-playing track title (as advertised by the stream's
 * Shoutcast/ICY metadata) into `out`. Empty string if the stream does
 * not embed metadata or none has been received yet. Always NUL-terminated.
 */
void media_radio_get_track_title(char *out, size_t out_size);

/**
 * Pause playback for a wake-word/voice session.
 * If radio is currently PLAYING (or CONNECTING) the URL is remembered and
 * playback is stopped; a later media_radio_resume() will pick it back up.
 * If no playback is active, this is a no-op.
 */
void media_radio_pause_for_session(void);

/**
 * Resume playback that was paused by media_radio_pause_for_session().
 * Returns true if playback was resumed, false if nothing was pending.
 */
bool media_radio_resume(void);

/**
 * Schedule playback for the next media_radio_resume() call.
 * This lets voice-command radio starts wait until the assistant response is
 * finished, avoiding speaker-path contention.
 */
void media_radio_schedule_resume(const char *url);

/**
 * Drop any pending resume state (call after an explicit user stop so we
 * don't unexpectedly restart playback on the next session-end).
 */
void media_radio_clear_pending(void);

#ifdef __cplusplus
}
#endif
