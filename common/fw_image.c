#include "fw_image.h"

#include <string.h>

#include "crc32.h"
#include "fw_layout.h"

_Static_assert(sizeof(fw_image_header_t) == 32, "image header must be 32 bytes");

fw_image_status_t fw_image_check(const uint8_t *base, size_t avail, uint16_t hw_model,
                                 const fw_image_header_t **hdr_out, uint32_t *crc_out)
{
    if (avail < FW_HEADER_OFFSET + sizeof(fw_image_header_t) + 4u) {
        return FW_IMG_BAD_SIZE;
    }
    fw_image_header_t hdr;
    memcpy(&hdr, base + FW_HEADER_OFFSET, sizeof hdr);   /* avoid alignment assumptions */

    if (hdr.magic != FW_IMAGE_MAGIC || hdr.header_ver != FW_IMAGE_HDR_VER) {
        return FW_IMG_BAD_MAGIC;
    }
    if (hdr.hw_model != hw_model) {
        return FW_IMG_BAD_HW_MODEL;
    }
    if (hdr.image_size < FW_HEADER_OFFSET + sizeof hdr || hdr.image_size > avail - 4u ||
        (hdr.image_size & 3u) != 0u) {
        return FW_IMG_BAD_SIZE;
    }
    uint32_t trailer;
    memcpy(&trailer, base + hdr.image_size, sizeof trailer);
    if (crc32(base, hdr.image_size) != trailer) {
        return FW_IMG_BAD_CRC;
    }
    if (hdr_out != NULL) {
        *hdr_out = (const fw_image_header_t *)(const void *)(base + FW_HEADER_OFFSET);
    }
    if (crc_out != NULL) {
        *crc_out = trailer;
    }
    return FW_IMG_OK;
}
