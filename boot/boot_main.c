/*
 * SIU bootloader — 8 KB at 0x08000000 (common/fw_layout.h).
 *
 * On every reset:
 *   1. Check the app slot and the staging slot (header, hardware model, CRC-32).
 *   2. Install staging -> app if the staged image was activated (FW_ACTIVATE) and differs from the
 *      app, or if the app is invalid and staging holds a valid image (recovery, e.g. power lost
 *      during an earlier install — the staging copy is never erased by an install).
 *   3. Start the app if it's valid; otherwise blink red and wait (recovery needs SWD).
 *
 * It has no protocol stack of its own: images arrive through the running app (app/fw_update).
 */
#include <stdbool.h>
#include <stdint.h>

#include "board.h"
#include "flash.h"
#include "fw_image.h"
#include "fw_layout.h"
#include "stm32f0xx.h"

#define BOOT_VERSION 1u

/* Lets the app report the bootloader version (FW_INFO.bootloader_ver). Placed at offset 0xC0. */
__attribute__((section(".boot_id"), used))
const uint32_t g_boot_id[2] = { FW_BOOT_ID_MAGIC, BOOT_VERSION };

static void led(uint16_t r, uint16_t g, uint16_t b)
{
    board_led_pwm_write(r, g, b);
}

static bool install(const fw_image_header_t *hdr)
{
    uint32_t len = hdr->image_size + 4u;          /* image + CRC trailer */
    led(0, BOARD_LED_PWM_MAX, BOARD_LED_PWM_MAX); /* cyan: updating (can't blink — erase stalls the CPU) */
    for (uint32_t p = 0; p < FW_SLOT_PAGES; p++) {
        if (!flash_erase_page(FW_APP_BASE + p * FW_FLASH_PAGE_SIZE)) {
            return false;
        }
    }
    for (uint32_t off = 0; off < len; off += 256u) {
        uint32_t n = (len - off) < 256u ? (len - off) : 256u;
        if (!flash_program(FW_APP_BASE + off, (const uint8_t *)(FW_STAGING_BASE + off), n)) {
            return false;
        }
    }
    return true;
}

static void start_app(void)
{
    const uint32_t *vectors = (const uint32_t *)FW_APP_BASE;
    board_deinit();
    __disable_irq();                              /* the app enables them once its vectors are in RAM */
    __set_MSP(vectors[0]);
    ((void (*)(void))vectors[1])();
}

int main(void)
{
    board_init();

    const fw_image_header_t *stg_hdr = NULL;
    uint32_t app_crc = 0, stg_crc = 0;
    bool app_ok = fw_image_check((const uint8_t *)FW_APP_BASE, FW_SLOT_SIZE - 4u, BOARD_HW_MODEL,
                                 NULL, &app_crc) == FW_IMG_OK;
    bool stg_ok = fw_image_check((const uint8_t *)FW_STAGING_BASE, FW_IMAGE_MAX, BOARD_HW_MODEL,
                                 &stg_hdr, &stg_crc) == FW_IMG_OK;
    bool activated = *(const volatile uint32_t *)FW_ACTIVATE_ADDR == FW_ACTIVATE_MAGIC;

    if (stg_ok && ((activated && (!app_ok || app_crc != stg_crc)) || !app_ok)) {
        if (install(stg_hdr)) {
            app_ok = fw_image_check((const uint8_t *)FW_APP_BASE, FW_SLOT_SIZE - 4u, BOARD_HW_MODEL,
                                    NULL, NULL) == FW_IMG_OK;
            if (app_ok) {
                *(volatile uint32_t *)FW_BOOT_INFO_ADDR = FW_BOOT_INFO_UPDATED;
            }
        } else {
            app_ok = false;
        }
    }

    if (app_ok) {
        start_app();
    }

    /* No valid firmware: fast red blink forever. */
    for (;;) {
        uint32_t t = board_millis();
        led((t % 250u) < 125u ? BOARD_LED_PWM_MAX : 0u, 0, 0);
    }
}
