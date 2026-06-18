# Local patches to managed components

These edits live under `managed_components/` and will be overwritten if the
component manager re-downloads the package (e.g. after a clean checkout). If
a build fails or the symptoms below come back, re-apply the patches.

## sendspin/sendspin-cpp 0.6.1

### `src/esp/client_connection.cpp` — bump websocket task stack

The library passes a default-constructed `esp_websocket_client_config_t` to
`esp_websocket_client_init`, which leaves `task_stack=0`. The IDF websocket
client then falls back to `WEBSOCKET_TASK_STACK = 4 KiB`, which overflows as
soon as a FLAC/PCM frame arrives and the sync task hands it through our PCM
callback. The patched line sets `config.task_stack = 12 * 1024`.

Look for the `PATCH (MiciMike)` marker comment.

If the patch goes missing, the symptom is a `stack overflow in task
websocket_task` panic on the very first Music Assistant "play" command.

### `src/esp/client_connection.cpp` - relax websocket send timeout

The ESP websocket client serializes sends and receives with an internal lock.
The upstream 10 ms send timeout is too short during Sendspin's initial binary
audio burst, so `client/time` messages can fail with:

`websocket_client: Could not lock ws-client within 10 timeout`

When that happens the client never becomes time-synced, the sync task waits,
and the audio ring buffer fills until `Failed to send audio chunk` repeats.
The patched timeout is 1000 ms: still bounded, but long enough for the receive
burst and an ESP32-S3 decode/write interval to yield.

Firmware-side format selection currently advertises PCM only. ESP32-S3 does
not have a usable hardware FLAC decode path in this stack; the micro-flac
software decoder fell behind and Music Assistant reported multi-second
`Slow send_bytes` stalls with tens of seconds of buffered audio.

### `src/protocol.cpp` - add `device_info.model` compatibility alias

Sendspin's spec field for the hardware model is `device_info.product_name`.
Music Assistant still showed `Unknown model` during testing, so the formatter
now keeps `product_name` and also emits `device_info.model` with the same
value. Look for the `PATCH (MiciMike)` marker comment.

### `include/sendspin/config.h` + `src/client.cpp` - optional local WS server

Explicit outbound mode (`connect_to(ws://MA:8927/sendspin)`) must not also
start the ESP-side WebSocket server. Music Assistant may keep a cached
`ws://device:8928/sendspin` URL and reconnect inbound; the two sockets then
replace each other for the same `client_id`, causing `unknown-client` close
1006 churn and unstable playback. The patch adds `start_ws_server`, default
true, and skips `ConnectionManager::init_server()` when false.
