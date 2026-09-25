#include "flash.h"

#include "stm32f0xx.h"

#define FLASH_KEY_1 0x45670123u
#define FLASH_KEY_2 0xCDEF89ABu
#define SR_ERRORS   (FLASH_SR_PGERR | FLASH_SR_WRPRTERR)

static void unlock(void)
{
    if (FLASH->CR & FLASH_CR_LOCK) {
        FLASH->KEYR = FLASH_KEY_1;
        FLASH->KEYR = FLASH_KEY_2;
    }
}

static void lock(void)
{
    FLASH->CR |= FLASH_CR_LOCK;
}

/* Waits for the operation to finish; returns false on a programming / write-protection error. */
static bool wait_done(void)
{
    while (FLASH->SR & FLASH_SR_BSY) {
    }
    bool ok = (FLASH->SR & SR_ERRORS) == 0u;
    FLASH->SR = SR_ERRORS | FLASH_SR_EOP;   /* write 1 to clear */
    return ok;
}

bool flash_erase_page(uint32_t addr)
{
    unlock();
    FLASH->SR = SR_ERRORS | FLASH_SR_EOP;
    FLASH->CR |= FLASH_CR_PER;
    FLASH->AR = addr;
    FLASH->CR |= FLASH_CR_STRT;
    bool ok = wait_done();
    FLASH->CR &= ~FLASH_CR_PER;
    lock();

    /* verify: an erased page reads all 0xFF */
    const volatile uint32_t *p = (const volatile uint32_t *)(addr & ~(1024u - 1u));
    for (unsigned i = 0; ok && i < 1024u / 4u; i++) {
        ok = p[i] == 0xFFFFFFFFu;
    }
    return ok;
}

bool flash_program(uint32_t addr, const uint8_t *data, size_t len)
{
    if ((addr & 1u) != 0u || (len & 1u) != 0u) {
        return false;
    }
    bool ok = true;
    unlock();
    FLASH->SR = SR_ERRORS | FLASH_SR_EOP;
    FLASH->CR |= FLASH_CR_PG;
    for (size_t i = 0; ok && i < len; i += 2u) {
        volatile uint16_t *dst = (volatile uint16_t *)(addr + i);
        uint16_t half = (uint16_t)(data[i] | (data[i + 1u] << 8));
        *dst = half;
        ok = wait_done() && *dst == half;
    }
    FLASH->CR &= ~FLASH_CR_PG;
    lock();
    return ok;
}
