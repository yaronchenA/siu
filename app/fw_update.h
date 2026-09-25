/*
 * Firmware update, receiving side — cpm_siu_protocol.md §8.6.
 *
 *   FW_BEGIN ──> ERASING ──(main loop erases the staging slot)──> RECEIVING
 *   FW_CHUNK x N (in order)                                        RECEIVING
 *   FW_END  ──> VERIFYING ──(main loop checks header + CRC)──> VERIFIED | ERROR
 *   FW_ACTIVATE ──> (main loop marks the image and resets; the bootloader installs it)
 *
 * The protocol actions run in the link task and only record requests; everything that stalls
 * the CPU (flash erase/program) or takes long (CRC over the slot) runs in fw_update_poll() from
 * the main loop, and only once the previous response has left the UART.
 *
 * Pure logic: flash, reset and locking go through the fw_port_* functions — implemented for the
 * chip in src/fw_port.c and with a RAM "flash" in tests/test_fw_update.c.
 */
#ifndef FW_UPDATE_H
#define FW_UPDATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* FW_STATUS.state */
typedef enum {
    FWU_IDLE = 0,
    FWU_ERASING,
    FWU_RECEIVING,
    FWU_VERIFYING,
    FWU_VERIFIED,
    FWU_ERROR
} fwu_state_t;

/* FW_STATUS.error */
typedef enum {
    FWU_ERR_NONE = 0,
    FWU_ERR_ERASE,          /* staging erase failed */
    FWU_ERR_WRITE,          /* programming a chunk failed */
    FWU_ERR_CRC,            /* image CRC doesn't match the one announced in FW_BEGIN */
    FWU_ERR_HEADER,         /* image header / trailer CRC invalid */
    FWU_ERR_HW_MODEL        /* image built for different hardware */
} fwu_error_t;

/* RESULT.detail for a REJECTED FW_* action */
enum {
    FWU_REJ_OFFSET      = 1,    /* chunk offset != next expected offset, or FW_END before all data */
    FWU_REJ_STATE       = 2,    /* not allowed in the current update state */
    FWU_REJ_CHUNK       = 3,    /* odd length, or runs past the image size */
    FWU_REJ_SIZE        = 6,    /* image size out of range */
    FWU_REJ_CHARGING    = 7     /* not while CP is offering current (PWM) */
};

#define FWU_CHUNK_MAX 200u

/* ---- port: provided by the firmware (src/fw_port.c) or the host test ---------------------- */
bool           fw_port_erase_staging(void);
bool           fw_port_write(uint32_t offset, const uint8_t *data, size_t len);
const uint8_t *fw_port_staging(void);
bool           fw_port_link_idle(void);          /* last response fully sent */
void           fw_port_activate_and_reset(void); /* marks the staged image, resets (doesn't return on target) */
void           fw_port_after_stall(void);        /* a flash operation just stalled the CPU (restart the link timer) */
uint32_t       fw_port_lock(void);
void           fw_port_unlock(uint32_t state);

/* ---- API ----------------------------------------------------------------------------------- */
void fw_update_init(uint16_t hw_model);

/* Action handlers (link task). Return a PROTO_RESULT_* code and set *detail. */
uint8_t fw_update_begin(const uint8_t *val, uint8_t len, bool charging, uint8_t *detail);
uint8_t fw_update_chunk(const uint8_t *val, uint8_t len, uint8_t *detail);
uint8_t fw_update_end(uint8_t *detail);
uint8_t fw_update_activate(bool charging, uint8_t *detail);

/* FW_STATUS value: state, next_offset (u32 LE), error. Returns false while idle. */
bool fw_update_status(uint8_t out[6]);

/* Link lost: forget the transfer (the CPM starts again with FW_BEGIN). */
void fw_update_abort(void);

/* Main loop: does the pending erase / write / verify / activate. */
void fw_update_poll(void);

fwu_state_t fw_update_state(void);

#endif /* FW_UPDATE_H */
