/*
 * SIU firmware — start-up and main loop.
 *
 * The CPM link (protocol) runs in the PendSV link task; the main loop does the slow work:
 * link timeout check and LED animation. More modules (lock, CP, RFID, ...) join the loop
 * as non-blocking xxx_poll() calls.
 */
#include <stdbool.h>
#include <stdint.h>

#include "board.h"
#include "cmd_dispatch.h"
#include "led_ctrl.h"
#include "link_task.h"
#include "proto.h"
#include "rgb_led.h"
#include "rs485.h"
#include "siu_config.h"
#include "siu_log.h"

#ifndef BUILD_ID
#define BUILD_ID 0u   /* set by the Makefile to the short git hash */
#endif

#define FW_MAJOR 0u
#define FW_MINOR 4u
#define FW_PATCH 0u

#define LED_UPDATE_MS 10u

/* Serial number and lifecycle come from flash provisioning (manufacturing_procedures.md §6,
 * step S15/S20) once the config storage exists. Until then the bench unit reports a fixed
 * bench serial as a production unit, so the factory-mode LED override doesn't hide LED_SET. */
#define BENCH_SERIAL "BENCH-F0308-01"

static siu_identity_t s_id;

static void identity_init(void)
{
    board_uid(s_id.uid);
    s_id.boot_id = board_random32();
    s_id.reset_reason = board_reset_reason();
    s_id.capabilities = 0;                 /* no RFID, lock, CP ... drivers yet */
    s_id.lifecycle = LIFECYCLE_PRODUCTION;
    s_id.serial = BENCH_SERIAL;
    s_id.hw_model = BOARD_HW_MODEL;
    s_id.hw_rev = BOARD_HW_REV;
    s_id.connector_rating_a = BOARD_CONNECTOR_RATING_A;
    s_id.phases = BOARD_PHASES;
    s_id.connector_type = BOARD_CONNECTOR_TYPE;
    s_id.fw_major = FW_MAJOR;
    s_id.fw_minor = FW_MINOR;
    s_id.fw_patch = FW_PATCH;
    s_id.build_id = BUILD_ID;
    s_id.bootloader_ver = 0;               /* no bootloader yet */
}

static const siu_log_lock_t k_log_lock = { board_critical_enter, board_critical_exit };

static const char *const k_reset_name[] = { "power-on", "brown-out", "watchdog", "software", "fw-update", "pin" };

int main(void)
{
    board_init();
    siu_log_init(&k_log_lock);
    identity_init();
    siu_log("boot: SIU fw %u.%u.%u build %08x, reset: %s", FW_MAJOR, FW_MINOR, FW_PATCH,
            (uint32_t)BUILD_ID, k_reset_name[s_id.reset_reason]);

    uint32_t now = board_millis();
    siu_config_init();
    led_ctrl_init(now);
    cmd_dispatch_reset();
    rs485_init();
    link_task_init(&s_id, now);

    /* Nothing is measured yet: every field reads "unknown" / 0. */
    const siu_status_t status = { 0 };
    link_task_set_status(&status);

    uint32_t last_led_ms = 0;
    for (;;) {
        link_task_tick();

        now = board_millis();
        if ((now - last_led_ms) >= LED_UPDATE_MS) {
            last_led_ms = now;
            /* LED_SET / AUTH_FEEDBACK arrive in the link task: read the clock inside the critical
             * section so it's never older than a feedback start time the link task recorded. */
            uint32_t cs = board_critical_enter();
            rgb_t c = led_ctrl_update(board_millis());
            uint8_t brightness = siu_config_led_brightness();
            board_critical_exit(cs);
            rgb_led_set_brightness(brightness);
            rgb_led_show(c);
        }
    }
}
