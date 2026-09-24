#include "crc16.h"

/* Nibble table: 32 bytes of flash, ~4x faster than bit-by-bit — a full 245-byte frame
 * takes well under 0.1 ms on a 48 MHz Cortex-M0, which matters for the 1 ms turnaround. */
static const uint16_t k_nibble[16] = {
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7,
    0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF,
};

uint16_t crc16_update(uint16_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        crc = (uint16_t)((crc << 4) ^ k_nibble[(crc >> 12) ^ (b >> 4)]);
        crc = (uint16_t)((crc << 4) ^ k_nibble[(crc >> 12) ^ (b & 0x0Fu)]);
    }
    return crc;
}
