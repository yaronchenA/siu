#include "link_session.h"

#include <stdbool.h>
#include <string.h>

#include "cmd_dispatch.h"
#include "frame.h"
#include "led_ctrl.h"
#include "proto.h"

#define SERIAL_MAX_LEN      24u
#define POLL_MIN_MS         5u
#define POLL_MAX_MS         1000u
#define TIMEOUT_MIN_MS      50u
#define TIMEOUT_MAX_MS      10000u

static struct {
    const siu_identity_t *id;
    link_state_t state;
    uint8_t      session;
    uint16_t     timeout_ms;
    uint16_t     poll_period_ms;
    uint32_t     last_rx_ms;

    /* last response, for answering a repeated SEQ without re-executing it (§5.1) */
    bool         cache_valid;
    uint8_t      cache_seq;
    size_t       cache_len;
    uint8_t      cache[PROTO_MAX_RAW];

    link_stats_t stats;
} s;

static void enter(link_state_t st)
{
    s.state = st;
    s.cache_valid = false;
    /* UNLINKED and HANDSHAKE hold the safe state (§4.1): CP to state F, "no link" LED. */
    led_ctrl_set_override(LED_OVR_NOLINK, st != LINK_ACTIVE);
    if (st != LINK_ACTIVE) {
        cmd_dispatch_reset();
    }
    if (st == LINK_UNLINKED) {
        s.session = PROTO_SESSION_NONE;
        s.timeout_ms = LINK_DEFAULT_TIMEOUT_MS;
    }
}

void link_session_init(const siu_identity_t *id, uint32_t now_ms)
{
    memset(&s, 0, sizeof s);
    s.id = id;
    s.last_rx_ms = now_ms;
    enter(LINK_UNLINKED);
}

link_state_t link_session_state(void) { return s.state; }
uint8_t link_session_id(void) { return s.session; }
const link_stats_t *link_session_stats(void) { return &s.stats; }

void link_session_tick(uint32_t now_ms)
{
    if (s.state != LINK_UNLINKED && (now_ms - s.last_rx_ms) > s.timeout_ms) {
        s.stats.link_losses++;
        enter(LINK_UNLINKED);
    }
}

/* ---- response TLVs ---------------------------------------------------------- */

static void put_error(frame_builder_t *b, uint8_t ref_type, uint8_t code)
{
    const uint8_t v[TLV_LEN_ERROR] = { ref_type, code, 0 };
    (void)tlv_put(b, TLV_ERROR, v, sizeof v);
}

static void put_status_fast(frame_builder_t *b, const siu_status_t *st)
{
    uint8_t v[TLV_LEN_STATUS_FAST];
    v[0] = st->cp_state;
    put_u16le(&v[1], (uint16_t)st->cp_high_mv);
    put_u16le(&v[3], (uint16_t)st->cp_low_mv);
    v[5] = st->pp_state;
    v[6] = st->pp_rating_a;
    v[7] = st->lock_state;
    v[8] = st->estop_loop;
    put_u32le(&v[9], st->fault_flags);
    (void)tlv_put(b, TLV_STATUS_FAST, v, sizeof v);
}

static void put_identity(frame_builder_t *b)
{
    const siu_identity_t *id = s.id;

    (void)tlv_put(b, TLV_SIU_UID, id->uid, sizeof id->uid);

    size_t n = strlen(id->serial);
    (void)tlv_put(b, TLV_SERIAL_NUMBER, id->serial, (uint8_t)(n > SERIAL_MAX_LEN ? SERIAL_MAX_LEN : n));

    uint8_t hw[TLV_LEN_HW_INFO];
    put_u16le(&hw[0], id->hw_model);
    hw[2] = id->hw_rev;
    hw[3] = id->connector_rating_a;
    hw[4] = id->phases;
    hw[5] = id->connector_type;
    (void)tlv_put(b, TLV_HW_INFO, hw, sizeof hw);

    uint8_t fw[TLV_LEN_FW_INFO];
    fw[0] = id->fw_major;
    fw[1] = id->fw_minor;
    fw[2] = id->fw_patch;
    put_u32le(&fw[3], id->build_id);
    fw[7] = id->bootloader_ver;
    (void)tlv_put(b, TLV_FW_INFO, fw, sizeof fw);
}

static void put_hello_info(frame_builder_t *b)
{
    uint8_t v[TLV_LEN_HELLO_INFO];
    v[0] = PROTO_MSG_MAJOR;
    v[1] = PROTO_MSG_MINOR;
    put_u32le(&v[2], s.id->boot_id);
    v[6] = s.id->reset_reason;
    put_u32le(&v[7], s.id->capabilities);
    v[11] = s.id->lifecycle;
    (void)tlv_put(b, TLV_HELLO_INFO, v, sizeof v);
}

/* ---- request handling --------------------------------------------------------- */

/* Finds the first TLV of a type; returns false if absent or shorter than min_len. */
static bool find_tlv(const uint8_t *payload, size_t plen, uint8_t want, uint8_t min_len,
                     const uint8_t **val)
{
    tlv_reader_t r;
    uint8_t type, len;
    tlv_reader_init(&r, payload, plen);
    while (tlv_next(&r, &type, val, &len)) {
        if (type == want) {
            return len >= min_len;
        }
    }
    return false;
}

static bool session_params_valid(const uint8_t *v, uint8_t hdr_session)
{
    uint16_t poll = get_u16le(&v[1]);
    uint16_t timeout = get_u16le(&v[3]);
    return v[0] == hdr_session && v[0] != PROTO_SESSION_NONE &&
           poll >= POLL_MIN_MS && poll <= POLL_MAX_MS &&
           timeout >= TIMEOUT_MIN_MS && timeout <= TIMEOUT_MAX_MS;
}

static size_t answer_hello(const proto_hdr_t *req, uint8_t *out, size_t cap)
{
    frame_builder_t b;
    proto_hdr_t h = { PROTO_FRAME_VER, PROTO_FLAG_RSP, req->seq, PROTO_SESSION_NONE };
    frame_begin(&b, out, cap, &h);
    put_hello_info(&b);
    put_identity(&b);
    return frame_finish(&b);
}

size_t link_session_handle(const uint8_t *raw, size_t len, const siu_status_t *status,
                           uint32_t now_ms, uint8_t *out, size_t cap)
{
    proto_hdr_t hdr;
    const uint8_t *payload;
    size_t plen;
    const uint8_t *v;

    if (frame_parse(raw, len, &hdr, &payload, &plen) != FRAME_OK) {
        s.stats.bad_frames++;
        return 0;
    }
    if (hdr.flags & PROTO_FLAG_RSP) {
        s.stats.wrong_dir++;
        return 0;
    }

    /* Handshake frames carry session 0 and must contain HELLO (§4.2). */
    if (hdr.session == PROTO_SESSION_NONE) {
        if (!find_tlv(payload, plen, TLV_HELLO, TLV_LEN_HELLO, &v)) {
            s.stats.stale_session++;
            return 0;
        }
        s.stats.rx_frames++;
        s.last_rx_ms = now_ms;
        enter(LINK_HANDSHAKE);
        return answer_hello(&hdr, out, cap);
    }

    if (s.state == LINK_HANDSHAKE) {
        /* SESSION_START's frame already carries the new session ID in its header. */
        if (!find_tlv(payload, plen, TLV_SESSION_START, TLV_LEN_SESSION_START, &v) ||
            !session_params_valid(v, hdr.session)) {
            s.stats.stale_session++;
            return 0;
        }
        s.session = hdr.session;
        s.poll_period_ms = get_u16le(&v[1]);
        s.timeout_ms = get_u16le(&v[3]);
        enter(LINK_ACTIVE);
    } else if (s.state != LINK_ACTIVE || hdr.session != s.session) {
        s.stats.stale_session++;
        return 0;
    }

    /* ACTIVE, correct session. */
    s.stats.rx_frames++;
    s.last_rx_ms = now_ms;

    if (s.cache_valid && hdr.seq == s.cache_seq && s.cache_len <= cap) {
        s.stats.dup_requests++;          /* same request again: resend, don't re-execute */
        memcpy(out, s.cache, s.cache_len);
        return s.cache_len;
    }

    frame_builder_t b;
    proto_hdr_t h = { PROTO_FRAME_VER, PROTO_FLAG_RSP, hdr.seq, s.session };
    frame_begin(&b, out, cap, &h);
    put_status_fast(&b, status);         /* first, so it always fits (§6.2) */

    bool end_session = false;
    bool service = (hdr.flags & PROTO_FLAG_SERVICE) != 0u;
    tlv_reader_t r;
    uint8_t type, tlen;
    tlv_reader_init(&r, payload, plen);
    while (tlv_next(&r, &type, &v, &tlen)) {
        switch (type) {
        case TLV_SESSION_START:
            if (tlen < TLV_LEN_SESSION_START) {
                put_error(&b, type, PROTO_ERR_BAD_LENGTH);
            } else if (!session_params_valid(v, s.session)) {
                put_error(&b, type, PROTO_ERR_OUT_OF_RANGE);
            } else {
                s.poll_period_ms = get_u16le(&v[1]);
                s.timeout_ms = get_u16le(&v[3]);
                (void)tlv_put(&b, TLV_SESSION_ACK, &s.session, TLV_LEN_SESSION_ACK);
            }
            break;
        case TLV_SESSION_END:
            end_session = true;
            break;
        case TLV_EVENT_ACK:
            if (tlen < TLV_LEN_EVENT_ACK) {
                put_error(&b, type, PROTO_ERR_BAD_LENGTH);
            }
            /* no events are generated yet — nothing to acknowledge */
            break;
        case TLV_IDENT_GET:
            put_identity(&b);
            break;
        default: {
            uint8_t err = cmd_dispatch_handle(type, v, tlen, service, now_ms, &b);
            if (err != 0u) {
                put_error(&b, type, err);
            }
            break;
        }
        }
    }

    if (end_session) {
        enter(LINK_UNLINKED);
        return 0;
    }

    size_t n = frame_finish(&b);
    if (n > 0u) {
        memcpy(s.cache, out, n);
        s.cache_len = n;
        s.cache_seq = hdr.seq;
        s.cache_valid = true;
    }
    return n;
}
