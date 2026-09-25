/* CRC-32 (IEEE 802.3 / zlib): reflected poly 0xEDB88320, init and final XOR 0xFFFFFFFF.
 * Same result as Python's zlib.crc32(). Check value for "123456789" = 0xCBF43926. */
#ifndef CRC32_H
#define CRC32_H

#include <stddef.h>
#include <stdint.h>

/* Incremental use: crc = crc32_update(0, a, n); crc = crc32_update(crc, b, m); */
uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len);

static inline uint32_t crc32(const uint8_t *data, size_t len)
{
    return crc32_update(0u, data, len);
}

#endif /* CRC32_H */
