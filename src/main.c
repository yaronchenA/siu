/*
 * SIU firmware — main loop.
 *
 * Current stage: bench demo (no protocol yet).
 *   LED:  short press of the user button steps to the next status (LED_SET ui_state 0..12);
 *         long press (>= 600 ms) steps through the demo actions below.
 *   Link: temporary text test on the CPM link UART (115200 8N1) — an "alive" line every
 *         second, and every line received is echoed back with a green LED flash.
 *         Replaced by the CPM<->SIU protocol in the next stage.
 */
#include <stdbool.h>
#include <stdint.h>

#include "board.h"
#include "led_ctrl.h"
#include "rgb_led.h"
#include "rs485.h"

#define LED_UPDATE_MS   10u
#define DEBOUNCE_MS     30u
#define LONG_PRESS_MS   600u
#define ALIVE_MS        1000u
#define LINE_MAX        64u

typedef enum {
    DEMO_FEEDBACK_ACCEPTED,
    DEMO_FEEDBACK_REJECTED,
    DEMO_ESTOP_ON,
    DEMO_ESTOP_OFF,       /* holds the E-stop look until the next short press */
    DEMO_NOLINK_ON,
    DEMO_NOLINK_OFF,
    DEMO_FACTORY_ON,
    DEMO_FACTORY_OFF,
    DEMO_COUNT
} demo_action_t;

static void run_demo_action(demo_action_t a, uint32_t now)
{
    switch (a) {
    case DEMO_FEEDBACK_ACCEPTED: led_ctrl_feedback(LED_FB_ACCEPTED, now); break;
    case DEMO_FEEDBACK_REJECTED: led_ctrl_feedback(LED_FB_REJECTED, now); break;
    case DEMO_ESTOP_ON:          led_ctrl_set_override(LED_OVR_ESTOP, true); break;
    case DEMO_ESTOP_OFF:         led_ctrl_set_override(LED_OVR_ESTOP, false); break;
    case DEMO_NOLINK_ON:         led_ctrl_set_override(LED_OVR_NOLINK, true); break;
    case DEMO_NOLINK_OFF:        led_ctrl_set_override(LED_OVR_NOLINK, false); break;
    case DEMO_FACTORY_ON:        led_ctrl_set_override(LED_OVR_FACTORY, true); break;
    case DEMO_FACTORY_OFF:       led_ctrl_set_override(LED_OVR_FACTORY, false); break;
    default: break;
    }
}

/* ---- temporary link test ------------------------------------------------ */

static void send_str(const char *str)
{
    size_t n = 0;
    while (str[n] != '\0') {
        n++;
    }
    (void)rs485_write((const uint8_t *)str, n);   /* dropped if the TX buffer is full — fine for a test */
}

static void send_u32(uint32_t v)
{
    char buf[11];
    char *p = &buf[sizeof buf - 1];
    *p = '\0';
    do {
        *--p = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u);
    send_str(p);
}

static void link_test_poll(uint32_t now)
{
    static uint32_t last_alive_ms;
    static char     line[LINE_MAX];
    static size_t   line_len;

    if ((now - last_alive_ms) >= ALIVE_MS) {
        last_alive_ms = now;
        send_str("SIU alive, uptime ");
        send_u32(now / 1000u);
        send_str(" s, rx dropped ");
        send_u32(rs485_rx_dropped());
        send_str("\r\n");
    }

    uint8_t buf[16];
    size_t n = rs485_read(buf, sizeof buf);
    for (size_t i = 0; i < n; i++) {
        char c = (char)buf[i];
        if (c == '\r' || c == '\n') {
            if (line_len > 0u) {
                line[line_len] = '\0';
                send_str("echo: ");
                send_str(line);
                send_str("\r\n");
                led_ctrl_feedback(LED_FB_ACCEPTED, now);
                line_len = 0;
            }
        } else if (line_len < LINE_MAX - 1u) {
            line[line_len++] = c;
        }
    }
}

/* -------------------------------------------------------------------------- */

int main(void)
{
    board_init();
    rs485_init();
    led_ctrl_init(board_millis());
    led_ctrl_set_state(UI_AVAILABLE, PAT_DEFAULT);   /* shown once the self-test ends */

    uint8_t  ui_state = UI_AVAILABLE;
    uint8_t  demo = 0;
    bool     pressed = false;
    uint32_t edge_ms = 0;
    uint32_t press_start_ms = 0;
    uint32_t last_led_ms = 0;

    for (;;) {
        uint32_t now = board_millis();

        /* Button: debounce, then act on release (short vs. long press). */
        bool level = board_button_pressed();
        if (level != pressed && (now - edge_ms) >= DEBOUNCE_MS) {
            pressed = level;
            edge_ms = now;
            if (pressed) {
                press_start_ms = now;
            } else if ((now - press_start_ms) >= LONG_PRESS_MS) {
                run_demo_action((demo_action_t)demo, now);
                demo = (uint8_t)((demo + 1u) % DEMO_COUNT);
            } else {
                ui_state = (uint8_t)((ui_state + 1u) % UI_STATE_COUNT);
                led_ctrl_set_state(ui_state, PAT_DEFAULT);
            }
        }

        link_test_poll(now);

        if ((now - last_led_ms) >= LED_UPDATE_MS) {
            last_led_ms = now;
            rgb_led_show(led_ctrl_update(now));
        }
    }
}
