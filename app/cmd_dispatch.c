#include "cmd_dispatch.h"

#include "led_ctrl.h"
#include "proto.h"

static cp_setpoint_t s_cp;

void cmd_dispatch_reset(void)
{
    s_cp.mode = CP_MODE_STATE_F;
    s_cp.duty_0p1pct = 0;
}

cp_setpoint_t cmd_dispatch_cp_setpoint(void)
{
    return s_cp;
}

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
    s_cp.mode = mode;
    s_cp.duty_0p1pct = mode == CP_MODE_PWM ? duty : 0u;
    return 0;
}

static uint8_t handle_led_set(const uint8_t *val, uint8_t len)
{
    if (len < TLV_LEN_LED_SET) {
        return PROTO_ERR_BAD_LENGTH;
    }
    return led_ctrl_set_state(val[0], val[1]) ? 0u : (uint8_t)PROTO_ERR_OUT_OF_RANGE;
}

uint8_t cmd_dispatch_handle(uint8_t type, const uint8_t *val, uint8_t len,
                            bool service, uint32_t now_ms, frame_builder_t *rsp)
{
    (void)service;
    (void)now_ms;
    (void)rsp;

    switch (type) {
    case TLV_CP_SET:  return handle_cp_set(val, len);
    case TLV_LED_SET: return handle_led_set(val, len);
    default:          return PROTO_ERR_UNKNOWN_TLV;   /* not implemented in this firmware yet */
    }
}
