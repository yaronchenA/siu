/*
 * Firmware image format — what the bootloader and the update code accept.
 *
 *   offset 0x00  vector table (192 bytes)
 *   offset 0xC0  fw_image_header_t (32 bytes, below)
 *   ...          code and data
 *   offset N     CRC-32 of bytes [0, N), little-endian       N = header.image_size
 *
 * The build fills image_size and appends the CRC (tools/mkimage.py); the file sent over
 * FW_BEGIN/FW_CHUNK is exactly N + 4 bytes.
 */
#ifndef FW_IMAGE_H
#define FW_IMAGE_H

#include <stddef.h>
#include <stdint.h>

#define FW_IMAGE_MAGIC      0x31554953u    /* "SIU1" */
#define FW_IMAGE_HDR_VER    1u

typedef struct {
    uint32_t magic;
    uint16_t header_ver;
    uint16_t hw_model;          /* must match the board (BOARD_HW_MODEL) */
    uint32_t image_size;        /* bytes covered by the CRC, i.e. everything before the trailer */
    uint8_t  fw_major, fw_minor, fw_patch, reserved0;
    uint32_t build_id;
    uint32_t reserved[3];
} fw_image_header_t;            /* 32 bytes */

typedef enum {
    FW_IMG_OK = 0,
    FW_IMG_BAD_MAGIC,
    FW_IMG_BAD_HW_MODEL,
    FW_IMG_BAD_SIZE,
    FW_IMG_BAD_CRC
} fw_image_status_t;

/* Checks an image in memory (a flash slot, or a buffer on the host).
 * avail: how many bytes of the slot may belong to the image (trailer included).
 * On success, *hdr_out (if not NULL) points at the header and *crc_out holds the trailer CRC. */
fw_image_status_t fw_image_check(const uint8_t *base, size_t avail, uint16_t hw_model,
                                 const fw_image_header_t **hdr_out, uint32_t *crc_out);

#endif /* FW_IMAGE_H */
