/*
 * COBS (Consistent Overhead Byte Stuffing) — cpm_siu_protocol.md §2.2.
 * Encoded data never contains 0x00, so 0x00 can mark the end of each frame on the wire.
 * These functions don't add or expect the delimiter; the caller handles it.
 */
#ifndef COBS_H
#define COBS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Worst-case encoded size for n input bytes. */
#define COBS_MAX_ENCODED(n) ((n) + ((n) / 254u) + 1u)

/* Returns the encoded length, or 0 if dst is too small. */
size_t cobs_encode(const uint8_t *src, size_t len, uint8_t *dst, size_t cap);

/* Decodes one frame (without its delimiter). Returns false on malformed input or overflow. */
bool cobs_decode(const uint8_t *src, size_t len, uint8_t *dst, size_t cap, size_t *out_len);

#endif /* COBS_H */
