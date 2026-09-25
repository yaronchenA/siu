/*
 * Internal flash erase / program (STM32F0: 1 KB pages, 16-bit programming).
 *
 * While the flash is erased or programmed, the CPU stalls on every flash access — including
 * interrupt handlers, which live in flash. A page erase takes ~20-40 ms, a halfword ~50 us.
 * Callers must plan for that silence (cpm_siu_protocol.md §8.6).
 */
#ifndef FLASH_H
#define FLASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Erases the 1 KB page containing addr. */
bool flash_erase_page(uint32_t addr);

/* Programs len bytes (len even, addr halfword-aligned) into erased flash, then verifies them. */
bool flash_program(uint32_t addr, const uint8_t *data, size_t len);

#endif /* FLASH_H */
