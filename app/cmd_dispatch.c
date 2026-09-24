#include "cmd_dispatch.h"

#include <string.h>

#include "led_ctrl.h"
#include "proto.h"
#include "siu_config.h"
#include "siu_log.h"

#define REQ_HISTORY 8u      /* last action REQ_IDs remembered per session (§5.2) */

typedef struct {
    bool    valid;
    uint8_t req_id;
    uint8_t type;
    uint8_t result;
    uint8_t detail;
} req_record_t;

static cp_setpoint_t s_cp;
static uint8_t       s_led_state = 0xFFu, s_led_pattern = 0xFFu;   /* for logging changes only */
static req_record_t  s_history[REQ_HISTORY];
static uint8_t       s_history_next;

void cmd_dispatch_reset(void)
{
    s_cp.mode = CP_MODE_STATE_F;
    s_cp.duty_0p1pct = 0;
    s_led_state = s_led_pattern = 0xFFu;
    memset(s_history, 0, sizeof s_history);
    s_history_next = 0;
}

cp_setpoint_t cmd_dispatch_cp_setpoint(void)
{
    return s_cp;
}

/* ---- action bookkeeping ------------------------------------------------------- */

static void put_result(frame_builder_t *rsp, uint8_t req_id, uint8_t type, uint8_t result, uint8_t detail)
{
    const uint8_t v[4] = { req_id, type, result, detail };
    (void)tlv_put(rsp, TLV_RESULT, v, sizeof v);
}

static const req_record_t *find_request(uint8_t req_id, uint8_t type)
{
    for (unsigned i = 0; i < REQ_HISTORY; i++) {
        if (s_history[i].valid && s_history[i].req_id == req_id && s_history[i].type == type) {
            return &s_history[i];
        }
    }
    return NULL;
}

/* Records the outcome of an executed action and reports it. */
static void finish_action(frame_builder_t *rsp, uint8_t req_id, uint8_t type, uint8_t result, uint8_t detail)
{
    req_record_t *r = &s_history[s_history_next];
    s_history_next = (uint8_t)((s_history_next + 1u) % REQ_HISTORY);
    r->valid = true;
    r->req_id = req_id;
    r->type = type;
    r->result = result;
    r->detail = detail;
    put_result(rsp, req_id, type, result, detail);
}

/* ---- state commands ------------------------------------------------------------------ */

static uint8_t handle_cp_set(const uint8_t *val, uint8_t len)
{
    if (len < TLV_LEN_CP_SET) {
        return PROTO_ERR_BAD_LENGTH;
    }
    uint8_t mode = val[0];
    uint16_t duty = get_u16le(&val[1]);
    if (mode > CP_MODE_STATE_F) {
        return PROTO_ERR_OUT_OF_RANGE;
    }
    /* Valid PWM duty: 5 % (digital communication) or 8 %..97 % (§8.3). CP unchanged otherwise. */
    if (mode == CP_MODE_PWM && duty != 50u && (duty < 80u || duty > 970u)) {
        return PROTO_ERR_OUT_OF_RANGE;
    }
    uint16_t new_duty = mode == CP_MODE_PWM ? duty : 0u;
    if (mode != s_cp.mode || new_duty != s_cp.duty_0p1pct) {
        siu_log("cp: mode %u duty %u.%u%%", mode, new_duty / 10u, new_duty % 10u);
    }
    s_cp.mode = mode;
    s_cp.duty_0p1pct = new_duty;
    return 0;
}

static uint8_t handle_led_set(const uint8_t *val, uint8_t len)
{
    if (len < TLV_LEN_LED_SET) {
        return PROTO_ERR_BAD_LENGTH;
    }
    if (!led_ctrl_set_state(val[0], val[1])) {
        return PROTO_ERR_OUT_OF_RANGE;
    }
    if (val[0] != s_led_state || val[1] != s_led_pattern) {
        siu_log("led: state %u pattern %u", val[0], val[1]);
        s_led_state = val[0];
        s_led_pattern = val[1];
    }
    return 0;
}

static uint8_t handle_led_raw(const uint8_t *val, uint8_t len, bool service)
{
    if (!service) {
        return PROTO_ERR_SERVICE_REQUIRED;
    }
    if (len < TLV_LEN_LED_RAW) {
        return PROTO_ERR_BAD_LENGTH;
    }
    const rgb_t c = { val[0], val[1], val[2] };
    led_ctrl_set_raw(c);
    siu_log("led: raw %u,%u,%u", c.r, c.g, c.b);
    s_led_state = s_led_pattern = 0xFFu;      /* the next LED_SET is a change again */
    return 0;
}

static uint8_t handle_config_get(const uint8_t *val, uint8_t len, frame_builder_t *rsp)
{
    if (len < TLV_LEN_CONFIG_GET) {
        return PROTO_ERR_BAD_LENGTH;
    }
    uint8_t out[1 + 16];
    out[0] = val[0];
    uint8_t n = siu_config_get(val[0], &out[1], (uint8_t)(sizeof out - 1u));
    if (n == 0u) {
        return PROTO_ERR_OUT_OF_RANGE;          /* unknown key */
    }
    (void)tlv_put(rsp, TLV_CONFIG_VALUE, out, (uint8_t)(1u + n));
    return 0;
}

/* ---- action commands (REQ_ID + RESULT) --------------------------------------------------- */

static void do_auth_feedback(const uint8_t *val, uint32_t now_ms, uint8_t *result, uint8_t *detail)
{
    /* result: 0 accepted, 1 rejected, 2 pending, 3 expired/blocked (§8.3) */
    switch (val[1]) {
    case 0: led_ctrl_feedback(LED_FB_ACCEPTED, now_ms); break;
    case 1:
    case 3: led_ctrl_feedback(LED_FB_REJECTED, now_ms); break;
    case 2: break;          /* still checking: the Authorizing status already shows it */
    default:
        *result = PROTO_RESULT_REJECTED;
        *detail = 1;        /* unknown result code */
        return;
    }
    *result = PROTO_RESULT_OK;
    *detail = 0;
    siu_log("auth: feedback %u", val[1]);
}

static void do_config_set(const uint8_t *val, uint8_t len, uint8_t *result, uint8_t *detail)
{
    bool ok = siu_config_set(val[1], &val[2], (uint8_t)(len - 2u));
    *result = ok ? PROTO_RESULT_OK : PROTO_RESULT_REJECTED;
    *detail = ok ? 0u : 1u;  /* 1 = unknown key or invalid value */
    siu_log("config: key 0x%02x = %u %s", val[1], len > 2u ? val[2] : 0u, ok ? "ok" : "REJECTED");
}

static uint8_t handle_action(uint8_t type, const uint8_t *val, uint8_t len, uint8_t min_len,
                             uint32_t now_ms, frame_builder_t *rsp)
{
    if (len < min_len) {
        return PROTO_ERR_BAD_LENGTH;
    }
    uint8_t req_id = val[0];
    if (req_id == 0u) {
        return PROTO_ERR_OUT_OF_RANGE;          /* REQ_ID 0 is reserved */
    }
    const req_record_t *seen = find_request(req_id, type);
    if (seen != NULL) {                         /* duplicate: report again, don't re-execute */
        siu_log("dup: REQ_ID %u (TLV 0x%02x) not re-executed", req_id, type);
        put_result(rsp, req_id, type, seen->result, seen->detail);
        return 0;
    }

    uint8_t result = PROTO_RESULT_FAILED, detail = 0;
    switch (type) {
    case TLV_AUTH_FEEDBACK: do_auth_feedback(val, now_ms, &result, &detail); break;
    case TLV_CONFIG_SET:    do_config_set(val, len, &result, &detail); break;
    default: break;
    }
    finish_action(rsp, req_id, type, result, detail);
    return 0;
}

uint8_t cmd_dispatch_handle(uint8_t type, const uint8_t *val, uint8_t len,
                            bool service, uint32_t now_ms, frame_builder_t *rsp)
{
    switch (type) {
    case TLV_CP_SET:        return handle_cp_set(val, len);
    case TLV_LED_SET:       return handle_led_set(val, len);
    case TLV_LED_RAW:       return handle_led_raw(val, len, service);
    case TLV_CONFIG_GET:    return handle_config_get(val, len, rsp);
    case TLV_AUTH_FEEDBACK: return handle_action(type, val, len, TLV_LEN_AUTH_FEEDBACK, now_ms, rsp);
    case TLV_CONFIG_SET:    return handle_action(type, val, len, TLV_LEN_CONFIG_SET_MIN, now_ms, rsp);
    default:                return PROTO_ERR_UNKNOWN_TLV;   /* not implemented in this firmware yet */
    }
}
