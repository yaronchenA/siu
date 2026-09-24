/*
 * Command TLVs (CPM -> SIU, cpm_siu_protocol.md §8.3): validates each one and applies it.
 * Session-level TLVs (HELLO, SESSION_START, ...) are handled by link_session, not here.
 */
#ifndef CMD_DISPATCH_H
#define CMD_DISPATCH_H

#include <stdbool.h>
#include <stdint.h>

#include "frame.h"

typedef struct {
    uint8_t  mode;          /* CP_MODE_* */
    uint16_t duty_0p1pct;   /* only meaningful in CP_MODE_PWM */
} cp_setpoint_t;

/* Back to defaults: CP setpoint = state F (safe). */
void cmd_dispatch_reset(void);

/* Handles one command TLV. May append reply TLVs (RESULT) to rsp.
 * Returns 0 if handled, otherwise a PROTO_ERR_* code for the ERROR TLV. */
uint8_t cmd_dispatch_handle(uint8_t type, const uint8_t *val, uint8_t len,
                            bool service, uint32_t now_ms, frame_builder_t *rsp);

/* Latest CP setpoint from the CPM (applied to hardware once the CP driver exists). */
cp_setpoint_t cmd_dispatch_cp_setpoint(void);

#endif /* CMD_DISPATCH_H */
