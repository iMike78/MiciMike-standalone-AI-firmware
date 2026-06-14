/**
 * Snapcast wire-protocol helpers.
 *
 * Just enough serialization to send a Hello and to parse the base headers
 * the server sends. The big payload-specific parsers (server settings,
 * codec header, wire chunk) will land in follow-up changes.
 */

#include "snap_proto.h"

#include <stdio.h>
#include <string.h>

// JSON template for the Hello payload. Real Snapcast clients send a JSON
// blob describing themselves — we keep it minimal but complete enough to
// be accepted by snapserver. The macro args are: MAC, Hostname, ClientName,
// Instance (always 1 for us), Architecture.
//
// This intentionally uses double-quotes so the format-string can be a single
// concatenated C literal even with snprintf.
#define SNAP_HELLO_JSON_FMT \
    "{" \
        "\"Arch\":\"%s\"," \
        "\"ClientName\":\"%s\"," \
        "\"HostName\":\"%s\"," \
        "\"ID\":\"%s\"," \
        "\"Instance\":%d," \
        "\"MAC\":\"%s\"," \
        "\"OS\":\"esp-idf\"," \
        "\"SnapStreamProtocolVersion\":2," \
        "\"Version\":\"0.1\"" \
    "}"

static inline void put_u16_le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static inline void put_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static inline uint16_t get_u16_le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t get_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static inline int32_t get_i32_le(const uint8_t *p)
{
    return (int32_t)get_u32_le(p);
}

void snap_pack_base_header(uint8_t *out26,
                           snap_msg_type_t type,
                           uint16_t id, uint16_t refers_to,
                           int32_t sent_sec, int32_t sent_usec,
                           uint32_t payload_size)
{
    put_u16_le(out26 + 0,  (uint16_t)type);
    put_u16_le(out26 + 2,  id);
    put_u16_le(out26 + 4,  refers_to);
    put_u32_le(out26 + 6,  (uint32_t)sent_sec);
    put_u32_le(out26 + 10, (uint32_t)sent_usec);
    // recv timestamps are server-side; we send zeros.
    put_u32_le(out26 + 14, 0);
    put_u32_le(out26 + 18, 0);
    put_u32_le(out26 + 22, payload_size);
}

int snap_parse_base_header(const uint8_t *in26, snap_base_header_t *hdr)
{
    if (!in26 || !hdr) return -1;
    hdr->type      = get_u16_le(in26 + 0);
    hdr->id        = get_u16_le(in26 + 2);
    hdr->refers_to = get_u16_le(in26 + 4);
    hdr->sent_sec  = get_i32_le(in26 + 6);
    hdr->sent_usec = get_i32_le(in26 + 10);
    hdr->recv_sec  = get_i32_le(in26 + 14);
    hdr->recv_usec = get_i32_le(in26 + 18);
    hdr->size      = get_u32_le(in26 + 22);
    return 0;
}

// Snapcast payload strings are length-prefixed: uint32 LE length, then bytes
// without a trailing NUL. The Hello payload is a single such string holding
// a JSON blob.
size_t snap_pack_hello_payload(uint8_t *out, size_t out_cap,
                               const char *mac, const char *hostname,
                               const char *client_name, int instance,
                               const char *arch)
{
    if (!out || out_cap < 5) return 0;

    // Build the JSON in a scratch buffer first so we know its exact length.
    char json[512];
    int json_len = snprintf(json, sizeof(json), SNAP_HELLO_JSON_FMT,
                            arch ? arch : "xtensa-esp32s3",
                            client_name ? client_name : "MiciMike",
                            hostname ? hostname : "micimike",
                            mac ? mac : "00:00:00:00:00:00",
                            instance,
                            mac ? mac : "00:00:00:00:00:00");
    if (json_len <= 0 || (size_t)json_len >= sizeof(json)) return 0;
    if ((size_t)json_len + 4 > out_cap) return 0;

    put_u32_le(out, (uint32_t)json_len);
    memcpy(out + 4, json, (size_t)json_len);
    return (size_t)json_len + 4;
}
