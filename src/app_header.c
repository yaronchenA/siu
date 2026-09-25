/*
 * Image header at offset 0xC0 of the application (right after the vector table) — common/fw_image.h.
 * image_size is filled in and the CRC trailer appended by tools/mkimage.py after linking.
 */
#include "board.h"
#include "fw_image.h"
#include "version.h"

__attribute__((section(".app_header"), used))
const fw_image_header_t g_app_header = {
    .magic      = FW_IMAGE_MAGIC,
    .header_ver = FW_IMAGE_HDR_VER,
    .hw_model   = BOARD_HW_MODEL,
    .image_size = 0xFFFFFFFFu,          /* patched by mkimage */
    .fw_major   = FW_MAJOR,
    .fw_minor   = FW_MINOR,
    .fw_patch   = FW_PATCH,
    .build_id   = BUILD_ID,
};
