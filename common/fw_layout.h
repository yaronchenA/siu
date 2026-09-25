/*
 * Flash and RAM layout for firmware update — STM32F030R8: 64 KB flash in 1 KB pages, 8 KB RAM.
 * Shared by the bootloader, the application and the tools (tools/mkimage.py mirrors these values).
 *
 *   0x08000000  bootloader        8 KB
 *   0x08002000  app slot         26 KB   the running firmware
 *   0x08008800  staging slot     26 KB   a new image downloads here; kept as a backup copy afterwards
 *   0x0800F000  reserved          4 KB   configuration storage (later)
 *
 *   0x20000000  app vector table 192 B   copied from flash (Cortex-M0 has no VTOR, ARCHITECTURE.md §5)
 *   0x200000C0  boot info          4 B   bootloader -> app: "I just installed an update"
 *   0x20000100  normal RAM use
 */
#ifndef FW_LAYOUT_H
#define FW_LAYOUT_H

#define FW_FLASH_PAGE_SIZE      1024u

#define FW_BOOT_BASE            0x08000000u
#define FW_BOOT_SIZE            (8u * 1024u)
#define FW_APP_BASE             0x08002000u
#define FW_SLOT_SIZE            (26u * 1024u)
#define FW_STAGING_BASE         (FW_APP_BASE + FW_SLOT_SIZE)          /* 0x08008800 */
#define FW_SLOT_PAGES           (FW_SLOT_SIZE / FW_FLASH_PAGE_SIZE)

/* Image = binary + 4-byte CRC-32 trailer. The last 4 bytes of the staging slot hold the
 * activation marker, so an image can be at most slot size - 4 bytes (trailer included). */
#define FW_IMAGE_MAX            (FW_SLOT_SIZE - 4u)
#define FW_ACTIVATE_ADDR        (FW_STAGING_BASE + FW_SLOT_SIZE - 4u)
#define FW_ACTIVATE_MAGIC       0x41435456u    /* "VTCA" */

#define FW_VECTOR_TABLE_SIZE    0xC0u          /* 48 vectors x 4 bytes on the STM32F030x8 */
#define FW_HEADER_OFFSET        FW_VECTOR_TABLE_SIZE

#define FW_RAM_VECTORS          0x20000000u
#define FW_BOOT_INFO_ADDR       0x200000C0u
#define FW_BOOT_INFO_UPDATED    0x55504454u    /* "TDPU": set by the bootloader after an install */

/* The bootloader carries a small ID at offset 0xC0 of its own flash, so the app can report its version. */
#define FW_BOOT_ID_ADDR         (FW_BOOT_BASE + FW_HEADER_OFFSET)
#define FW_BOOT_ID_MAGIC        0x314C4253u    /* "SBL1" */

#endif /* FW_LAYOUT_H */
