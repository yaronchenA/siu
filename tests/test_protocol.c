/*
 * Host unit tests for common/protocol — CRC, COBS, frame/TLV encode and decode.
 * The reference bytes are the worked examples in cpm_siu_protocol.md §10.
 */
#include "cobs.h"
#include "crc16.h"
#include "frame.h"

#include <stdio.h>
#include <string.h>

static int g_failures;
static int g_checks;

#define CHECK(cond)                                                            \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            g_failures++;                                                      \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                      \
    } while (0)

/* §10: CPM -> SIU poll — session 0x7C, SEQ 5, CP_SET PWM 53.3 %, LED_SET Charging, EVENT_ACK 7 */
static const uint8_t k_req_raw[] = {
    0x10, 0x05, 0x7C, 0x40, 0x03, 0x01, 0x15, 0x02, 0x41, 0x02, 0x02, 0x00,
    0x05, 0x02, 0x07, 0x00, 0x92, 0x1C,
};
static const uint8_t k_req_wire[] = {   /* without the 0x00 delimiter */
    0x0C, 0x10, 0x05, 0x7C, 0x40, 0x03, 0x01, 0x15, 0x02, 0x41, 0x02, 0x02,
    0x04, 0x05, 0x02, 0x07, 0x03, 0x92, 0x1C,
};
/* §10: SIU -> CPM response — STATUS_FAST: state C, +6.0 V / -11.9 V, plug 32 A, locked */
static const uint8_t k_rsp_raw[] = {
    0x11, 0x05, 0x7C, 0x60, 0x0D, 0x03, 0x70, 0x17, 0x84, 0xD1, 0x01, 0x20,
    0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9B, 0x32,
};
static const uint8_t k_rsp_wire[] = {
    0x0E, 0x11, 0x05, 0x7C, 0x60, 0x0D, 0x03, 0x70, 0x17, 0x84, 0xD1, 0x01,
    0x20, 0x02, 0x01, 0x01, 0x01, 0x01, 0x03, 0x9B, 0x32,
};

static void test_crc_check_value(void)
{
    CHECK(crc16((const uint8_t *)"123456789", 9) == 0x29B1);
    CHECK(crc16(k_req_raw, sizeof k_req_raw - 2) == 0x1C92);
    CHECK(crc16(k_rsp_raw, sizeof k_rsp_raw - 2) == 0x329B);
}

static void test_cobs_spec_examples(void)
{
    uint8_t enc[64], dec[64];
    size_t n = cobs_encode(k_req_raw, sizeof k_req_raw, enc, sizeof enc);
    CHECK(n == sizeof k_req_wire && memcmp(enc, k_req_wire, n) == 0);
    n = cobs_encode(k_rsp_raw, sizeof k_rsp_raw, enc, sizeof enc);
    CHECK(n == sizeof k_rsp_wire && memcmp(enc, k_rsp_wire, n) == 0);

    size_t m = 0;
    CHECK(cobs_decode(k_req_wire, sizeof k_req_wire, dec, sizeof dec, &m));
    CHECK(m == sizeof k_req_raw && memcmp(dec, k_req_raw, m) == 0);
}

static void test_cobs_roundtrip(void)
{
    static uint8_t src[600], enc[COBS_MAX_ENCODED(600)], dec[600];
    uint32_t x = 12345;
    for (size_t len = 0; len <= sizeof src; len += (len < 300 ? 1 : 37)) {
        for (int pattern = 0; pattern < 3; pattern++) {
            for (size_t i = 0; i < len; i++) {
                x = x * 1103515245u + 12345u;
                /* 0: random, 1: no zeros (tests the 254-byte blocks), 2: mostly zeros */
                src[i] = pattern == 0 ? (uint8_t)(x >> 16)
                       : pattern == 1 ? (uint8_t)(1u + (x >> 16) % 255u)
                       : (uint8_t)(((x >> 16) % 5u) == 0u ? 7u : 0u);
            }
            size_t n = cobs_encode(src, len, enc, sizeof enc);
            int ok = n > 0 && n <= COBS_MAX_ENCODED(len) && memchr(enc, 0, n) == NULL;
            size_t m = 0;
            ok = ok && cobs_decode(enc, n, dec, sizeof dec, &m) && m == len && memcmp(src, dec, len) == 0;
            CHECK(ok);
        }
    }
}

static void test_cobs_rejects_bad_input(void)
{
    uint8_t dec[16];
    size_t m;
    const uint8_t embedded_zero[] = { 0x03, 0x11, 0x00 };
    const uint8_t truncated[]     = { 0x05, 0x11, 0x22 };
    CHECK(!cobs_decode(embedded_zero, sizeof embedded_zero, dec, sizeof dec, &m));
    CHECK(!cobs_decode(truncated, sizeof truncated, dec, sizeof dec, &m));

    uint8_t small[4];
    CHECK(cobs_encode(k_req_raw, sizeof k_req_raw, small, sizeof small) == 0);   /* overflow */
}

static void test_build_matches_spec(void)
{
    uint8_t buf[PROTO_MAX_RAW];
    frame_builder_t b;
    proto_hdr_t h = { PROTO_FRAME_VER, 0, 5, 0x7C };
    frame_begin(&b, buf, sizeof buf, &h);
    const uint8_t cp[] = { CP_MODE_PWM, 0x15, 0x02 };
    const uint8_t led[] = { 2, 0 };
    const uint8_t ack[] = { 7, 0 };
    CHECK(tlv_put(&b, TLV_CP_SET, cp, sizeof cp));
    CHECK(tlv_put(&b, TLV_LED_SET, led, sizeof led));
    CHECK(tlv_put(&b, TLV_EVENT_ACK, ack, sizeof ack));
    size_t n = frame_finish(&b);
    CHECK(n == sizeof k_req_raw && memcmp(buf, k_req_raw, n) == 0);
}

static void test_parse_spec_frames(void)
{
    proto_hdr_t h;
    const uint8_t *p;
    size_t plen;

    CHECK(frame_parse(k_req_raw, sizeof k_req_raw, &h, &p, &plen) == FRAME_OK);
    CHECK(h.ver == 1 && h.flags == 0 && h.seq == 5 && h.session == 0x7C && plen == 13);

    tlv_reader_t r;
    uint8_t type, len;
    const uint8_t *val;
    tlv_reader_init(&r, p, plen);
    CHECK(tlv_next(&r, &type, &val, &len) && type == TLV_CP_SET && len == 3 && get_u16le(&val[1]) == 533);
    CHECK(tlv_next(&r, &type, &val, &len) && type == TLV_LED_SET && val[0] == 2);
    CHECK(tlv_next(&r, &type, &val, &len) && type == TLV_EVENT_ACK && get_u16le(val) == 7);
    CHECK(!tlv_next(&r, &type, &val, &len));

    CHECK(frame_parse(k_rsp_raw, sizeof k_rsp_raw, &h, &p, &plen) == FRAME_OK);
    CHECK(h.flags == PROTO_FLAG_RSP);
    tlv_reader_init(&r, p, plen);
    CHECK(tlv_next(&r, &type, &val, &len) && type == TLV_STATUS_FAST && len == TLV_LEN_STATUS_FAST);
    CHECK(val[0] == 3 && (int16_t)get_u16le(&val[1]) == 6000 && (int16_t)get_u16le(&val[3]) == -11900);
}

/* Builds a frame with an arbitrary payload and a correct CRC. */
static size_t make_raw(uint8_t *buf, uint8_t byte0, const uint8_t *payload, size_t plen)
{
    buf[0] = byte0;
    buf[1] = 1;
    buf[2] = 9;
    memcpy(&buf[3], payload, plen);
    put_u16le(&buf[3 + plen], crc16(buf, 3 + plen));
    return 3 + plen + 2;
}

static void test_parse_rejects(void)
{
    uint8_t buf[PROTO_MAX_RAW + 8];
    proto_hdr_t h;
    const uint8_t *p;
    size_t plen;

    CHECK(frame_parse(k_req_raw, 4, &h, &p, &plen) == FRAME_TOO_SHORT);

    memcpy(buf, k_req_raw, sizeof k_req_raw);
    buf[6] ^= 0x01;
    CHECK(frame_parse(buf, sizeof k_req_raw, &h, &p, &plen) == FRAME_BAD_CRC);

    size_t n = make_raw(buf, 0x20, NULL, 0);                 /* VER 2 */
    CHECK(frame_parse(buf, n, &h, &p, &plen) == FRAME_BAD_VER);

    const uint8_t overrun[] = { TLV_LED_SET, 5, 1, 2 };      /* says 5 bytes, has 2 */
    n = make_raw(buf, 0x10, overrun, sizeof overrun);
    CHECK(frame_parse(buf, n, &h, &p, &plen) == FRAME_MALFORMED_TLV);

    const uint8_t dangling[] = { TLV_LED_SET, 2, 1, 2, 0x41 }; /* stray type byte, no length */
    n = make_raw(buf, 0x10, dangling, sizeof dangling);
    CHECK(frame_parse(buf, n, &h, &p, &plen) == FRAME_MALFORMED_TLV);

    n = make_raw(buf, 0x10, NULL, 0);                        /* empty poll is valid */
    CHECK(frame_parse(buf, n, &h, &p, &plen) == FRAME_OK && plen == 0);
}

static void test_builder_limits(void)
{
    uint8_t buf[PROTO_MAX_RAW];
    uint8_t big[238] = { 0 };
    frame_builder_t b;
    proto_hdr_t h = { PROTO_FRAME_VER, PROTO_FLAG_RSP, 1, 1 };

    frame_begin(&b, buf, sizeof buf, &h);
    CHECK(frame_space(&b) == PROTO_MAX_PAYLOAD);
    CHECK(tlv_put(&b, TLV_LOG_TEXT, big, sizeof big));       /* 2 + 238 = 240: exactly full */
    CHECK(frame_space(&b) == 0);
    CHECK(!tlv_put(&b, TLV_SESSION_ACK, big, 0));
    CHECK(frame_finish(&b) == PROTO_MAX_RAW);

    proto_hdr_t hp;
    const uint8_t *p;
    size_t plen;
    CHECK(frame_parse(buf, PROTO_MAX_RAW, &hp, &p, &plen) == FRAME_OK && plen == PROTO_MAX_PAYLOAD);
}

int main(void)
{
    struct { const char *name; void (*fn)(void); } tests[] = {
        { "crc_check_value",         test_crc_check_value },
        { "cobs_spec_examples",      test_cobs_spec_examples },
        { "cobs_roundtrip",          test_cobs_roundtrip },
        { "cobs_rejects_bad_input",  test_cobs_rejects_bad_input },
        { "build_matches_spec",      test_build_matches_spec },
        { "parse_spec_frames",       test_parse_spec_frames },
        { "parse_rejects",           test_parse_rejects },
        { "builder_limits",          test_builder_limits },
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        int before = g_failures;
        tests[i].fn();
        printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", tests[i].name);
    }
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
