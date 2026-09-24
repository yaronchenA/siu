/*
 * Host unit tests for app/link_session + app/cmd_dispatch — cpm_siu_protocol.md §4–§6, §8.1–§8.3.
 */
#include "cmd_dispatch.h"
#include "frame.h"
#include "led_ctrl.h"
#include "link_session.h"
#include "proto.h"

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

static const siu_identity_t k_id = {
    .uid = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 },
    .boot_id = 0xA1B2C3D4u,
    .reset_reason = RESET_PIN,
    .capabilities = 0,
    .lifecycle = LIFECYCLE_PRODUCTION,
    .serial = "BENCH-0001",
    .hw_model = 0xB001, .hw_rev = 0, .connector_rating_a = 16, .phases = 3, .connector_type = 0,
    .fw_major = 0, .fw_minor = 1, .fw_patch = 0, .build_id = 0x1234ABCDu, .bootloader_ver = 0,
};
static const siu_status_t k_status = { .cp_state = 1, .lock_state = 1 };

/* ---- request builder / response reader ------------------------------------ */

typedef struct {
    uint8_t raw[PROTO_MAX_RAW];
    frame_builder_t b;
} req_t;

static void req_begin(req_t *q, uint8_t flags, uint8_t seq, uint8_t session)
{
    proto_hdr_t h = { PROTO_FRAME_VER, flags, seq, session };
    frame_begin(&q->b, q->raw, sizeof q->raw, &h);
}

static void req_put(req_t *q, uint8_t type, const uint8_t *v, uint8_t len)
{
    CHECK(tlv_put(&q->b, type, v, len));
}

static uint8_t g_rsp[PROTO_MAX_RAW];
static size_t g_rsp_len;

static size_t req_send(req_t *q, uint32_t now)
{
    size_t n = frame_finish(&q->b);
    g_rsp_len = link_session_handle(q->raw, n, &k_status, now, g_rsp, sizeof g_rsp);
    return g_rsp_len;
}

/* Finds a TLV in the last response. */
static const uint8_t *rsp_tlv(uint8_t want, uint8_t *len_out)
{
    proto_hdr_t h;
    const uint8_t *p, *v;
    size_t plen;
    uint8_t type, len;
    if (g_rsp_len == 0 || frame_parse(g_rsp, g_rsp_len, &h, &p, &plen) != FRAME_OK) {
        return NULL;
    }
    tlv_reader_t r;
    tlv_reader_init(&r, p, plen);
    while (tlv_next(&r, &type, &v, &len)) {
        if (type == want) {
            if (len_out) {
                *len_out = len;
            }
            return v;
        }
    }
    return NULL;
}

static proto_hdr_t rsp_hdr(void)
{
    proto_hdr_t h = { 0 };
    const uint8_t *p;
    size_t plen;
    (void)frame_parse(g_rsp, g_rsp_len, &h, &p, &plen);
    return h;
}

static void send_hello(uint8_t seq, uint32_t now)
{
    req_t q;
    uint8_t hello[TLV_LEN_HELLO] = { PROTO_MSG_MAJOR, PROTO_MSG_MINOR };
    req_begin(&q, 0, seq, PROTO_SESSION_NONE);
    req_put(&q, TLV_HELLO, hello, sizeof hello);
    req_send(&q, now);
}

static void send_session_start(uint8_t seq, uint8_t session, uint16_t poll, uint16_t timeout, uint32_t now)
{
    req_t q;
    uint8_t v[TLV_LEN_SESSION_START] = { session };
    put_u16le(&v[1], poll);
    put_u16le(&v[3], timeout);
    req_begin(&q, 0, seq, session);
    req_put(&q, TLV_SESSION_START, v, sizeof v);
    req_send(&q, now);
}

/* Fresh SIU brought to ACTIVE with session 7 at t=1000. */
static void bring_up(void)
{
    led_ctrl_init(0);
    (void)led_ctrl_update(1000);        /* self-test over */
    cmd_dispatch_reset();
    link_session_init(&k_id, 1000);
    send_hello(1, 1000);
    send_session_start(2, 7, 20, 200, 1010);
}

/* ---- tests ------------------------------------------------------------------ */

static void test_unlinked_ignores_polls_and_shows_no_link(void)
{
    led_ctrl_init(0);
    (void)led_ctrl_update(1000);
    link_session_init(&k_id, 1000);
    CHECK(link_session_state() == LINK_UNLINKED);

    req_t q;
    req_begin(&q, 0, 1, 5);
    CHECK(req_send(&q, 1000) == 0);
    CHECK(link_session_stats()->stale_session == 1);

    rgb_t c = led_ctrl_update(1000);
    CHECK(c.r == 255 && c.g == 0 && c.b == 0);          /* red, slow blink = no link */
}

static void test_hello_returns_identity(void)
{
    led_ctrl_init(0);
    link_session_init(&k_id, 1000);
    send_hello(9, 1000);
    CHECK(g_rsp_len > 0);
    CHECK(link_session_state() == LINK_HANDSHAKE);

    proto_hdr_t h = rsp_hdr();
    CHECK(h.flags == PROTO_FLAG_RSP && h.seq == 9 && h.session == PROTO_SESSION_NONE);

    uint8_t len;
    const uint8_t *v = rsp_tlv(TLV_HELLO_INFO, &len);
    CHECK(v && len == TLV_LEN_HELLO_INFO && v[0] == PROTO_MSG_MAJOR);
    CHECK(v && get_u32le(&v[2]) == 0xA1B2C3D4u && v[6] == RESET_PIN && v[11] == LIFECYCLE_PRODUCTION);
    v = rsp_tlv(TLV_SIU_UID, &len);
    CHECK(v && len == 12 && memcmp(v, k_id.uid, 12) == 0);
    v = rsp_tlv(TLV_SERIAL_NUMBER, &len);
    CHECK(v && len == 10 && memcmp(v, "BENCH-0001", 10) == 0);
    v = rsp_tlv(TLV_HW_INFO, &len);
    CHECK(v && get_u16le(v) == 0xB001 && v[3] == 16 && v[4] == 3);
    v = rsp_tlv(TLV_FW_INFO, &len);
    CHECK(v && v[1] == 1 && get_u32le(&v[3]) == 0x1234ABCDu);
}

static void test_session_start_activates(void)
{
    bring_up();
    CHECK(link_session_state() == LINK_ACTIVE && link_session_id() == 7);
    proto_hdr_t h = rsp_hdr();
    CHECK(h.session == 7 && h.seq == 2);
    uint8_t len;
    const uint8_t *v = rsp_tlv(TLV_SESSION_ACK, &len);
    CHECK(v && v[0] == 7);
    CHECK(rsp_tlv(TLV_STATUS_FAST, &len) != NULL && len == TLV_LEN_STATUS_FAST);
}

static void test_session_start_needs_matching_header(void)
{
    led_ctrl_init(0);
    link_session_init(&k_id, 1000);
    send_hello(1, 1000);

    req_t q;
    uint8_t v[TLV_LEN_SESSION_START] = { 7 };
    put_u16le(&v[1], 20);
    put_u16le(&v[3], 200);
    req_begin(&q, 0, 2, 8);                               /* header says 8, TLV says 7 */
    req_put(&q, TLV_SESSION_START, v, sizeof v);
    CHECK(req_send(&q, 1010) == 0);
    CHECK(link_session_state() == LINK_HANDSHAKE);

    send_session_start(3, 7, 1, 200, 1020);               /* poll period out of range */
    CHECK(g_rsp_len == 0 && link_session_state() == LINK_HANDSHAKE);
}

static void test_led_set_and_status(void)
{
    bring_up();
    req_t q;
    const uint8_t led[] = { UI_CHARGING, 0 };
    req_begin(&q, 0, 3, 7);
    req_put(&q, TLV_LED_SET, led, sizeof led);
    CHECK(req_send(&q, 1030) > 0);
    CHECK(rsp_tlv(TLV_ERROR, NULL) == NULL);

    uint8_t len;
    const uint8_t *st = rsp_tlv(TLV_STATUS_FAST, &len);
    CHECK(st && st[0] == 1 && st[7] == 1);                 /* from k_status */

    rgb_t c = led_ctrl_update(1030);
    CHECK(c.r == 0 && c.g == 255 && c.b == 0);             /* charging: green */
}

static void test_duplicate_seq_not_reexecuted(void)
{
    bring_up();
    req_t q;
    const uint8_t charging[] = { UI_CHARGING, 0 };
    req_begin(&q, 0, 3, 7);
    req_put(&q, TLV_LED_SET, charging, sizeof charging);
    req_send(&q, 1030);
    uint8_t first[PROTO_MAX_RAW];
    size_t first_len = g_rsp_len;
    memcpy(first, g_rsp, first_len);

    /* same SEQ again (as a retry) — even with different content, it must not be applied */
    const uint8_t faulted[] = { UI_FAULTED, 0 };
    req_begin(&q, PROTO_FLAG_RETRY, 3, 7);
    req_put(&q, TLV_LED_SET, faulted, sizeof faulted);
    req_send(&q, 1040);
    CHECK(g_rsp_len == first_len && memcmp(g_rsp, first, first_len) == 0);
    CHECK(link_session_stats()->dup_requests == 1);
    rgb_t c = led_ctrl_update(1040);
    CHECK(c.g == 255 && c.r == 0);                         /* still charging green */
}

static void test_wrong_session_and_direction_dropped(void)
{
    bring_up();
    req_t q;
    req_begin(&q, 0, 3, 8);                                 /* stale session */
    CHECK(req_send(&q, 1030) == 0);
    req_begin(&q, PROTO_FLAG_RSP, 4, 7);                    /* a response, e.g. our own echo */
    CHECK(req_send(&q, 1030) == 0);
    CHECK(link_session_stats()->wrong_dir == 1);
}

static void test_errors_for_bad_tlvs(void)
{
    bring_up();
    req_t q;
    const uint8_t junk[] = { 1, 2 };
    const uint8_t bad_led[] = { 99, 0 };
    const uint8_t short_cp[] = { 1 };
    req_begin(&q, 0, 3, 7);
    req_put(&q, 0x7E, junk, sizeof junk);                   /* unknown type */
    req_put(&q, TLV_LED_SET, bad_led, sizeof bad_led);
    req_put(&q, TLV_CP_SET, short_cp, sizeof short_cp);
    CHECK(req_send(&q, 1030) > 0);

    /* three ERROR TLVs, in request order */
    proto_hdr_t h;
    const uint8_t *p, *v;
    size_t plen;
    uint8_t type, len, codes[3][2];
    int n = 0;
    CHECK(frame_parse(g_rsp, g_rsp_len, &h, &p, &plen) == FRAME_OK);
    tlv_reader_t r;
    tlv_reader_init(&r, p, plen);
    while (tlv_next(&r, &type, &v, &len)) {
        if (type == TLV_ERROR && n < 3) {
            codes[n][0] = v[0];
            codes[n][1] = v[1];
            n++;
        }
    }
    CHECK(n == 3);
    CHECK(codes[0][0] == 0x7E && codes[0][1] == PROTO_ERR_UNKNOWN_TLV);
    CHECK(codes[1][0] == TLV_LED_SET && codes[1][1] == PROTO_ERR_OUT_OF_RANGE);
    CHECK(codes[2][0] == TLV_CP_SET && codes[2][1] == PROTO_ERR_BAD_LENGTH);
}

static void test_cp_set_validation(void)
{
    bring_up();
    CHECK(cmd_dispatch_cp_setpoint().mode == CP_MODE_STATE_F);

    req_t q;
    uint8_t cp[3] = { CP_MODE_PWM };
    put_u16le(&cp[1], 533);
    req_begin(&q, 0, 3, 7);
    req_put(&q, TLV_CP_SET, cp, sizeof cp);
    req_send(&q, 1030);
    CHECK(cmd_dispatch_cp_setpoint().mode == CP_MODE_PWM && cmd_dispatch_cp_setpoint().duty_0p1pct == 533);

    put_u16le(&cp[1], 990);                                 /* 99 %: invalid, CP unchanged */
    req_begin(&q, 0, 4, 7);
    req_put(&q, TLV_CP_SET, cp, sizeof cp);
    req_send(&q, 1040);
    CHECK(rsp_tlv(TLV_ERROR, NULL) != NULL);
    CHECK(cmd_dispatch_cp_setpoint().duty_0p1pct == 533);
}

static void test_link_timeout_goes_safe(void)
{
    bring_up();
    req_t q;
    uint8_t cp[3] = { CP_MODE_PWM };
    put_u16le(&cp[1], 533);
    req_begin(&q, 0, 3, 7);
    req_put(&q, TLV_CP_SET, cp, sizeof cp);
    req_send(&q, 1030);

    link_session_tick(1030 + 200);                          /* exactly the timeout: still OK */
    CHECK(link_session_state() == LINK_ACTIVE);
    link_session_tick(1030 + 201);
    CHECK(link_session_state() == LINK_UNLINKED);
    CHECK(link_session_stats()->link_losses == 1);
    CHECK(cmd_dispatch_cp_setpoint().mode == CP_MODE_STATE_F);   /* safe state */

    req_begin(&q, 0, 4, 7);                                  /* old session no longer accepted */
    CHECK(req_send(&q, 1300) == 0);
}

static void test_session_end_and_new_hello(void)
{
    bring_up();
    req_t q;
    const uint8_t reason[] = { 0 };
    req_begin(&q, 0, 3, 7);
    req_put(&q, TLV_SESSION_END, reason, sizeof reason);
    CHECK(req_send(&q, 1030) == 0);
    CHECK(link_session_state() == LINK_UNLINKED);

    bring_up();
    send_hello(10, 1050);                                     /* CPM rebooted */
    CHECK(g_rsp_len > 0 && link_session_state() == LINK_HANDSHAKE);
}

int main(void)
{
    struct { const char *name; void (*fn)(void); } tests[] = {
        { "unlinked_ignores_polls_and_shows_no_link", test_unlinked_ignores_polls_and_shows_no_link },
        { "hello_returns_identity",                   test_hello_returns_identity },
        { "session_start_activates",                  test_session_start_activates },
        { "session_start_needs_matching_header",      test_session_start_needs_matching_header },
        { "led_set_and_status",                       test_led_set_and_status },
        { "duplicate_seq_not_reexecuted",             test_duplicate_seq_not_reexecuted },
        { "wrong_session_and_direction_dropped",      test_wrong_session_and_direction_dropped },
        { "errors_for_bad_tlvs",                      test_errors_for_bad_tlvs },
        { "cp_set_validation",                        test_cp_set_validation },
        { "link_timeout_goes_safe",                   test_link_timeout_goes_safe },
        { "session_end_and_new_hello",                test_session_end_and_new_hello },
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        int before = g_failures;
        tests[i].fn();
        printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", tests[i].name);
    }
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
