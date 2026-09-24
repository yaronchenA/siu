/*
 * Status LED logic — decides what colour the SIU's RGB LED shows at any moment.
 *
 * Implements siu_detailed_design.md §6.1:
 *  - the status commanded by the CPM (LED_SET, cpm_siu_protocol.md §8.3),
 *  - SIU-local overrides, shown by priority (§6.1.3),
 *  - short feedback flashes on top of the commanded status (§6.1.4),
 *  - pattern timing (§6.1.5).
 *
 * Pure logic: no hardware access. Time comes in as an argument, a colour comes out,
 * so this module is unit-tested on the PC (tests/test_led_ctrl.c).
 */
#ifndef LED_CTRL_H
#define LED_CTRL_H

#include <stdbool.h>
#include <stdint.h>

#include "rgb.h"

/* LED_SET.ui_state — values are fixed by the protocol. */
typedef enum {
    UI_AVAILABLE        = 0,
    UI_SUSPENDED_EVSE   = 1,
    UI_CHARGING         = 2,
    UI_FAULTED          = 3,
    UI_RESERVED         = 4,
    UI_STOPPED          = 5,
    UI_UPDATING         = 6,
    UI_AUTHORIZING      = 7,
    UI_SUSPENDED_EV     = 8,
    UI_PREPARING        = 9,
    UI_FINISHING        = 10,
    UI_UNAVAILABLE      = 11,
    UI_PENDING_APPROVAL = 12,
    UI_STATE_COUNT
} ui_state_t;

/* LED_SET.pattern — 0 means "the default for this state". */
typedef enum {
    PAT_DEFAULT = 0,
    PAT_SOLID   = 1,
    PAT_BLINK   = 2,   /* 1 Hz, 50 % */
    PAT_FLICKER = 3,   /* 4 Hz, 50 % */
    PAT_BREATHE = 4,   /* 2 s fade up/down */
    PAT_PROTOCOL_COUNT,
    /* internal only, never sent over the protocol */
    PAT_ALTERNATE = PAT_PROTOCOL_COUNT, /* two colours, 500 ms each */
    PAT_SELFTEST                        /* red, green, blue, 300 ms each */
} led_pattern_t;

/* SIU-local overrides, highest priority first (siu_detailed_design.md §6.1.3). */
typedef enum {
    LED_OVR_SELFTEST = 0,  /* power-on, clears itself after 900 ms */
    LED_OVR_FACTORY,       /* lifecycle = factory mode */
    LED_OVR_BOOTLOADER,    /* firmware update in progress */
    LED_OVR_ESTOP,         /* stop button loop open */
    LED_OVR_OVERTEMP,      /* connector over-temperature trip */
    LED_OVR_NOLINK,        /* no valid frame from the CPM within the link timeout */
    LED_OVR_COUNT
} led_override_t;

typedef enum {
    LED_FB_ACCEPTED = 0,   /* 2 green flashes */
    LED_FB_REJECTED,       /* 3 red flashes */
    LED_FB_CARD_IGNORED    /* 1 red flash */
} led_feedback_t;

/* Starts the power-on self-test. Nothing is commanded yet. */
void led_ctrl_init(uint32_t now_ms);

/* From LED_SET. Returns false (and changes nothing) if a value is out of range. */
bool led_ctrl_set_state(uint8_t ui_state, uint8_t pattern);

/* From LED_RAW (service only): show this exact colour, solid, until the next led_ctrl_set_state().
 * Shown above every override except the power-on self-test — production test runs in factory mode
 * and must still be able to check each colour channel (manufacturing_procedures.md §6, S14).
 * A stop, over-temperature or no-link override that becomes active cancels it. */
void led_ctrl_set_raw(rgb_t c);

/* Activate / clear a local override. When ESTOP, OVERTEMP or NOLINK clears, its look is
 * held until the next led_ctrl_set_state() — never falls back to a stale commanded status. */
void led_ctrl_set_override(led_override_t ovr, bool active);

/* Plays over the commanded status only; ignored while any override or hold is showing. */
void led_ctrl_feedback(led_feedback_t fb, uint32_t now_ms);

/* Call regularly (e.g. every 10 ms). Returns the colour to show now. */
rgb_t led_ctrl_update(uint32_t now_ms);

#endif /* LED_CTRL_H */
