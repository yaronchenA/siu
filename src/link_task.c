#include "link_task.h"

#include "board.h"
#include "cobs.h"
#include "proto.h"
#include "rs485.h"
#include "stm32f0xx.h"

/* Everything below is touched only in PendSV, except where noted. */
static uint8_t      s_acc[PROTO_MAX_WIRE];     /* COBS bytes of the frame being received */
static size_t       s_acc_len;
static bool         s_acc_overflow;
static uint8_t      s_raw[PROTO_MAX_RAW];
static uint8_t      s_rsp_raw[PROTO_MAX_RAW];
static uint8_t      s_rsp_wire[PROTO_MAX_WIRE];
static siu_status_t s_status;                  /* written by the main loop under a critical section */

void link_task_init(const siu_identity_t *id, uint32_t now_ms)
{
    link_session_init(id, now_ms);
    rs485_set_frame_end_hook(board_link_task_pend);
    NVIC_SetPriority(PendSV_IRQn, 3);          /* lowest: UART and tick interrupts preempt it */
}

void link_task_set_status(const siu_status_t *status)
{
    uint32_t cs = board_critical_enter();
    s_status = *status;
    board_critical_exit(cs);
}

void link_task_tick(void)
{
    uint32_t cs = board_critical_enter();
    link_session_tick(board_millis());
    board_critical_exit(cs);
}

static void handle_frame(const uint8_t *wire, size_t len)
{
    size_t raw_len;
    if (!cobs_decode(wire, len, s_raw, sizeof s_raw, &raw_len)) {
        return;                                /* garbage — frame_parse would reject it anyway */
    }
    size_t n = link_session_handle(s_raw, raw_len, &s_status, board_millis(),
                                   s_rsp_raw, sizeof s_rsp_raw);
    if (n == 0u) {
        return;
    }
    size_t w = cobs_encode(s_rsp_raw, n, s_rsp_wire, sizeof s_rsp_wire - 1u);
    if (w > 0u) {
        s_rsp_wire[w++] = 0x00u;
        (void)rs485_write(s_rsp_wire, w);
    }
}

void PendSV_Handler(void)
{
    uint8_t chunk[32];
    size_t n;
    while ((n = rs485_read(chunk, sizeof chunk)) > 0u) {
        for (size_t i = 0; i < n; i++) {
            uint8_t b = chunk[i];
            if (b == 0x00u) {
                if (!s_acc_overflow && s_acc_len > 0u) {
                    handle_frame(s_acc, s_acc_len);
                }
                s_acc_len = 0;
                s_acc_overflow = false;
            } else if (s_acc_len < sizeof s_acc) {
                s_acc[s_acc_len++] = b;
            } else {
                s_acc_overflow = true;         /* too long to be a valid frame: drop it */
            }
        }
    }
}
