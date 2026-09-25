/* Firmware-update port for the STM32F030 (see app/fw_update.h). */
#include "board.h"
#include "flash.h"
#include "link_task.h"
#include "fw_layout.h"
#include "fw_update.h"
#include "rs485.h"
#include "stm32f0xx.h"

bool fw_port_erase_staging(void)
{
    for (uint32_t p = 0; p < FW_SLOT_PAGES; p++) {
        if (!flash_erase_page(FW_STAGING_BASE + p * FW_FLASH_PAGE_SIZE)) {
            return false;
        }
    }
    return true;
}

bool fw_port_write(uint32_t offset, const uint8_t *data, size_t len)
{
    return offset + len <= FW_IMAGE_MAX && flash_program(FW_STAGING_BASE + offset, data, len);
}

const uint8_t *fw_port_staging(void)
{
    return (const uint8_t *)FW_STAGING_BASE;
}

bool fw_port_link_idle(void)
{
    return rs485_tx_idle();
}

void fw_port_activate_and_reset(void)
{
    const uint32_t magic = FW_ACTIVATE_MAGIC;
    (void)flash_program(FW_ACTIVATE_ADDR, (const uint8_t *)&magic, sizeof magic);
    NVIC_SystemReset();   /* the bootloader installs the staged image */
}

void fw_port_after_stall(void)
{
    link_task_touch();
}

uint32_t fw_port_lock(void)
{
    return board_critical_enter();
}

void fw_port_unlock(uint32_t state)
{
    board_critical_exit(state);
}
