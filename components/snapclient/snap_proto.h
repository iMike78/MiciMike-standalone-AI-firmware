/**
 * Snapcast wire-protocol message layouts (internal to the component).
 *
 * Reference: https://github.com/badaix/snapcast/blob/develop/doc/binary_protocol.md
 *
 * All multi-byte integers are LITTLE-ENDIAN. Strings are length-prefixed
 * with a uint32_t (NOT NUL-terminated on the wire).
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// -------------------------------------------------------------------------
// Message type IDs (uint16, "type" field of base header).
// -------------------------------------------------------------------------
typedef enum {
    SNAP_MSG_BASE                = 0,
    SNAP_MSG_CODEC_HEADER        = 1,
    SNAP_MSG_WIRE_CHUNK          = 2,
    SNAP_MSG_SERVER_SETTINGS     = 3,
    SNAP_MSG_TIME                = 4,
    SNAP_MSG_HELLO               = 5,
    SNAP_MSG_STREAM_TAGS         = 6,
    SNAP_MSG_CLIENT_INFO         = 7,
} snap_msg_type_t;

// -------------------------------------------------------------------------
// Base header that prefixes every message on the wire.
// Total size on the wire: 26 bytes.
// -------------------------------------------------------------------------
typedef struct __attribute__((packed)) {
    uint16_t type;
    uint16_t id;            // request-id; the server echoes it in refers_to for responses
    uint16_t refers_to;     // id of the message this is a response to (0 if unsolicited)
    int32_t  sent_sec;      // wall time when sender enqueued this message
    int32_t  sent_usec;
    int32_t  recv_sec;      // wall time when receiver got it (filled in by server)
    int32_t  recv_usec;
    uint32_t size;          // payload bytes following this header
} snap_base_header_t;

// -------------------------------------------------------------------------
// SAMPLE_FORMAT inside Codec Header (fixed layout: rate/bits/channels).
// Snapcast sends this for "pcm" and the FLAC stream-info header as the
// payload for "flac". We extract rate/bits/channels for both.
// -------------------------------------------------------------------------
typedef struct {
    uint32_t sample_rate;   // e.g. 48000
    uint16_t bits;          // e.g. 16
    uint16_t channels;      // e.g. 2
} snap_sample_format_t;

// -------------------------------------------------------------------------
// Maximum sizes we are willing to receive. Snapcast wire chunks are
// typically 26 + ~4..16 KiB depending on codec and chunk size; we reject
// anything pathologically large to keep memory predictable.
// -------------------------------------------------------------------------
#define SNAP_MAX_PAYLOAD_BYTES   (64 * 1024)

// -------------------------------------------------------------------------
// Time-sync arithmetic helpers.
// -------------------------------------------------------------------------
typedef struct {
    int64_t client_to_server_us;   // sample offset added to local clock to estimate server clock
} snap_time_sync_t;

// -------------------------------------------------------------------------
// Wire-helpers (LE serialization). Defined in snap_proto.c.
// -------------------------------------------------------------------------
size_t snap_pack_hello_payload(uint8_t *out, size_t out_cap,
                               const char *mac, const char *hostname,
                               const char *client_name, int instance,
                               const char *arch);
void   snap_pack_base_header(uint8_t *out26,
                             snap_msg_type_t type,
                             uint16_t id, uint16_t refers_to,
                             int32_t sent_sec, int32_t sent_usec,
                             uint32_t payload_size);
int    snap_parse_base_header(const uint8_t *in26, snap_base_header_t *hdr);

#ifdef __cplusplus
}
#endif
