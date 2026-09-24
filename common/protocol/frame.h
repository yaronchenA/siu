/*
 * Frame and TLV encode/decode — cpm_siu_protocol.md §2 and §3.
 * Works on raw (COBS-decoded) frames: header + TLVs + CRC16.
 */
#ifndef FRAME_H
#define FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "proto.h"

typedef struct {
    uint8_t ver;       /* high nibble of byte 0 */
    uint8_t flags;     /* low nibble of byte 0 */
    uint8_t seq;
    uint8_t session;
} proto_hdr_t;

typedef enum {
    FRAME_OK = 0,
    FRAME_TOO_SHORT,
    FRAME_TOO_LONG,
    FRAME_BAD_CRC,
    FRAME_BAD_VER,
    FRAME_MALFORMED_TLV     /* a TLV's length runs past the end of the payload */
} frame_status_t;

/* Validates length, CRC, version and TLV structure; on success fills hdr and the payload span. */
frame_status_t frame_parse(const uint8_t *raw, size_t len, proto_hdr_t *hdr,
                           const uint8_t **payload, size_t *payload_len);

/* ---- TLV reader ------------------------------------------------------------ */

typedef struct {
    const uint8_t *p;
    size_t len;
    size_t pos;
} tlv_reader_t;

void tlv_reader_init(tlv_reader_t *r, const uint8_t *payload, size_t len);

/* Returns false at the end of the payload (or on a malformed TLV — frame_parse rejects those first). */
bool tlv_next(tlv_reader_t *r, uint8_t *type, const uint8_t **val, uint8_t *len);

/* ---- frame builder --------------------------------------------------------- */

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    bool overflow;
} frame_builder_t;

/* cap is the raw buffer size (use PROTO_MAX_RAW); room for the CRC is kept automatically. */
void frame_begin(frame_builder_t *b, uint8_t *buf, size_t cap, const proto_hdr_t *hdr);

/* Appends one TLV. Returns false (and appends nothing) if it doesn't fit. */
bool tlv_put(frame_builder_t *b, uint8_t type, const void *val, uint8_t len);

/* Payload bytes still free for TLVs (including their 2-byte TLV headers). */
size_t frame_space(const frame_builder_t *b);

/* Appends the CRC; returns the raw frame length, or 0 if anything overflowed. */
size_t frame_finish(frame_builder_t *b);

/* ---- little-endian helpers ------------------------------------------------- */

static inline uint16_t get_u16le(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t get_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void put_u16le(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

#endif /* FRAME_H */
