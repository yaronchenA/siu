/* CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no final XOR (check "123456789" = 0x29B1). */
#ifndef CRC16_H
#define CRC16_H

#include <stddef.h>
#include <stdint.h>

#define CRC16_INIT 0xFFFFu

uint16_t crc16_update(uint16_t crc, const uint8_t *data, size_t len);

static inline uint16_t crc16(const uint8_t *data, size_t len)
{
    return crc16_update(CRC16_INIT, data, len);
}

#endif /* CRC16_H */
