#include "frame.h"

#include "crc16.h"

frame_status_t frame_parse(const uint8_t *raw, size_t len, proto_hdr_t *hdr,
                           const uint8_t **payload, size_t *payload_len)
{
    if (len < PROTO_MIN_RAW) {
        return FRAME_TOO_SHORT;
    }
    if (len > PROTO_MAX_RAW) {
        return FRAME_TOO_LONG;
    }
    size_t body = len - PROTO_CRC_LEN;
    if (crc16(raw, body) != get_u16le(&raw[body])) {
        return FRAME_BAD_CRC;
    }

    hdr->ver = (uint8_t)(raw[0] >> 4);
    hdr->flags = (uint8_t)(raw[0] & 0x0Fu);
    hdr->seq = raw[1];
    hdr->session = raw[2];
    if (hdr->ver != PROTO_FRAME_VER) {
        return FRAME_BAD_VER;
    }

    const uint8_t *p = &raw[PROTO_HDR_LEN];
    size_t plen = body - PROTO_HDR_LEN;
    for (size_t pos = 0; pos < plen;) {          /* structure check before anything is acted on */
        if (plen - pos < 2u || plen - pos - 2u < p[pos + 1]) {
            return FRAME_MALFORMED_TLV;
        }
        pos += 2u + p[pos + 1];
    }
    *payload = p;
    *payload_len = plen;
    return FRAME_OK;
}

void tlv_reader_init(tlv_reader_t *r, const uint8_t *payload, size_t len)
{
    r->p = payload;
    r->len = len;
    r->pos = 0;
}

bool tlv_next(tlv_reader_t *r, uint8_t *type, const uint8_t **val, uint8_t *len)
{
    if (r->len - r->pos < 2u) {
        return false;
    }
    uint8_t l = r->p[r->pos + 1];
    if (r->len - r->pos - 2u < l) {
        return false;
    }
    *type = r->p[r->pos];
    *len = l;
    *val = &r->p[r->pos + 2];
    r->pos += 2u + l;
    return true;
}

void frame_begin(frame_builder_t *b, uint8_t *buf, size_t cap, const proto_hdr_t *hdr)
{
    b->buf = buf;
    b->cap = cap;
    b->len = 0;
    b->overflow = cap < PROTO_MIN_RAW;
    if (!b->overflow) {
        buf[0] = (uint8_t)((hdr->ver << 4) | (hdr->flags & 0x0Fu));
        buf[1] = hdr->seq;
        buf[2] = hdr->session;
        b->len = PROTO_HDR_LEN;
    }
}

size_t frame_space(const frame_builder_t *b)
{
    size_t limit = b->cap < PROTO_MAX_RAW ? b->cap : PROTO_MAX_RAW;
    size_t used = b->len + PROTO_CRC_LEN;
    return (b->overflow || used > limit) ? 0u : limit - used;
}

bool tlv_put(frame_builder_t *b, uint8_t type, const void *val, uint8_t len)
{
    if (frame_space(b) < 2u + (size_t)len) {
        return false;
    }
    const uint8_t *v = (const uint8_t *)val;
    b->buf[b->len++] = type;
    b->buf[b->len++] = len;
    for (uint8_t i = 0; i < len; i++) {
        b->buf[b->len++] = v[i];
    }
    return true;
}

size_t frame_finish(frame_builder_t *b)
{
    if (b->overflow || b->len + PROTO_CRC_LEN > b->cap) {
        return 0;
    }
    put_u16le(&b->buf[b->len], crc16(b->buf, b->len));
    b->len += PROTO_CRC_LEN;
    return b->len;
}
