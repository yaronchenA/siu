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
#include "fw_layout.h"
#include "fw_update.h"
#include "led_ctrl.h"
#include "link_task.h"
#include "proto.h"
#include "rgb_led.h"
#include "rs485.h"
#include "siu_config.h"
#include "siu_log.h"
#include "version.h"

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
    volatile uint32_t *boot_info = (volatile uint32_t *)FW_BOOT_INFO_ADDR;
    if (*boot_info == FW_BOOT_INFO_UPDATED) {  /* the bootloader just installed this firmware */
        s_id.reset_reason = RESET_FW_UPDATE;
        *boot_info = 0;
    }
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
    const volatile uint32_t *boot_id = (const volatile uint32_t *)FW_BOOT_ID_ADDR;
    s_id.bootloader_ver = boot_id[0] == FW_BOOT_ID_MAGIC ? (uint8_t)boot_id[1] : 0u;
    s_id.capabilities |= 1u << 9;          /* firmware update supported (§8.1) */
}

static const siu_log_lock_t k_log_lock = { board_critical_enter, board_critical_exit };

static const char *const k_reset_name[] = { "power-on", "brown-out", "watchdog", "software", "fw-update", "pin" };

int main(void)
{
    board_vectors_to_ram();                /* must be first: interrupts need the RAM vector table */
    board_init();
    siu_log_init(&k_log_lock);
    identity_init();
    siu_log("boot: fw %u.%u.%u %08x, boot v%u, %s", FW_MAJOR, FW_MINOR, FW_PATCH,
            (uint32_t)BUILD_ID, s_id.bootloader_ver, k_reset_name[s_id.reset_reason]);

    uint32_t now = board_millis();
    siu_config_init();
    led_ctrl_init(now);
    cmd_dispatch_reset();
    fw_update_init(BOARD_HW_MODEL);
    rs485_init();
    link_task_init(&s_id, now);

    /* Nothing is measured yet: every field reads "unknown" / 0. */
    const siu_status_t status = { 0 };
    link_task_set_status(&status);

    uint32_t last_led_ms = 0;
    for (;;) {
        link_task_tick();
        fw_update_poll();                  /* flash erase/write happens here, never mid-response */

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
