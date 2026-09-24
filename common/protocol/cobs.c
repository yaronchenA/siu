#include "cobs.h"

size_t cobs_encode(const uint8_t *src, size_t len, uint8_t *dst, size_t cap)
{
    if (cap == 0u) {
        return 0;
    }
    size_t out = 1;          /* dst[0] reserved for the first code byte */
    size_t code_idx = 0;
    uint8_t code = 1;

    for (size_t i = 0; i < len; i++) {
        if (src[i] == 0u) {
            dst[code_idx] = code;
            if (out >= cap) {
                return 0;
            }
            code_idx = out++;
            code = 1;
        } else {
            if (out >= cap) {
                return 0;
            }
            dst[out++] = src[i];
            if (++code == 0xFFu) {           /* block of 254 non-zero bytes is full */
                dst[code_idx] = code;
                if (out >= cap) {
                    return 0;
                }
                code_idx = out++;
                code = 1;
            }
        }
    }
    dst[code_idx] = code;
    return out;
}

bool cobs_decode(const uint8_t *src, size_t len, uint8_t *dst, size_t cap, size_t *out_len)
{
    size_t in = 0, out = 0;

    while (in < len) {
        uint8_t code = src[in++];
        if (code == 0u) {
            return false;                     /* 0x00 can't appear inside a frame */
        }
        for (uint8_t k = 1; k < code; k++) {
            if (in >= len || src[in] == 0u || out >= cap) {
                return false;
            }
            dst[out++] = src[in++];
        }
        if (code != 0xFFu && in < len) {      /* a code < 0xFF implies a zero, except at the end */
            if (out >= cap) {
                return false;
            }
            dst[out++] = 0;
        }
    }
    *out_len = out;
    return true;
}
