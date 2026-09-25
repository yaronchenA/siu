#include "fw_update.h"

#include <string.h>

#include "crc32.h"
#include "fw_image.h"
#include "fw_layout.h"
#include "proto.h"
#include "siu_log.h"

#define MIN_IMAGE_SIZE (FW_HEADER_OFFSET + sizeof(fw_image_header_t) + 4u)

static struct {
    uint16_t hw_model;
    volatile uint8_t state;         /* fwu_state_t */
    volatile uint8_t error;         /* fwu_error_t */
    volatile uint32_t gen;          /* bumped by FW_BEGIN / abort: stale main-loop results are dropped */

    uint32_t size;                  /* file size: image + CRC trailer */
    uint32_t crc;                   /* the image's CRC-32 (= its trailer): identifies this exact image */
    volatile uint32_t next_offset;

    volatile bool erase_req;
    volatile bool verify_req;
    volatile bool activate_req;

    volatile bool chunk_pending;    /* chunk buffered by the link task, not yet written */
    uint32_t chunk_offset;
    uint8_t  chunk_len;
    uint8_t  chunk[FWU_CHUNK_MAX];
} s;

static const char *const k_state_name[] = { "idle", "erasing", "receiving", "verifying", "verified", "error" };

void fw_update_init(uint16_t hw_model)
{
    memset(&s, 0, sizeof s);
    s.hw_model = hw_model;
}

fwu_state_t fw_update_state(void)
{
    return (fwu_state_t)s.state;
}

static void set_state(uint8_t st, uint8_t err)
{
    if (st != s.state) {
        siu_log("fw: %s%s", k_state_name[st], err ? " (error)" : "");
    }
    s.state = st;
    s.error = err;
}

/* ---- actions (link task) --------------------------------------------------------------- */

uint8_t fw_update_begin(const uint8_t *val, uint8_t len, bool charging, uint8_t *detail)
{
    (void)len;   /* length checked by the dispatcher */
    uint32_t size = (uint32_t)val[1] | ((uint32_t)val[2] << 8) | ((uint32_t)val[3] << 16) | ((uint32_t)val[4] << 24);
    uint32_t crc = (uint32_t)val[5] | ((uint32_t)val[6] << 8) | ((uint32_t)val[7] << 16) | ((uint32_t)val[8] << 24);

    if (charging) {
        *detail = FWU_REJ_CHARGING;
        return PROTO_RESULT_REJECTED;
    }
    if (size < MIN_IMAGE_SIZE || size > FW_IMAGE_MAX || (size & 3u) != 0u) {
        *detail = FWU_REJ_SIZE;
        return PROTO_RESULT_REJECTED;
    }
    s.gen++;
    s.size = size;
    s.crc = crc;
    s.next_offset = 0;
    s.chunk_pending = false;
    s.verify_req = false;
    s.activate_req = false;
    s.erase_req = true;
    set_state(FWU_ERASING, FWU_ERR_NONE);
    siu_log("fw: begin %u bytes, v%u.%u.%u", size, val[9], val[10], val[11]);
    *detail = 0;
    return PROTO_RESULT_IN_PROGRESS;
}

uint8_t fw_update_chunk(const uint8_t *val, uint8_t len, uint8_t *detail)
{
    uint32_t offset = (uint32_t)val[1] | ((uint32_t)val[2] << 8) | ((uint32_t)val[3] << 16) | ((uint32_t)val[4] << 24);
    uint8_t dlen = (uint8_t)(len - 5u);

    *detail = 0;
    if (s.state == FWU_ERASING || (s.state == FWU_RECEIVING && s.chunk_pending)) {
        return PROTO_RESULT_BUSY;                 /* try again next poll */
    }
    if (s.state != FWU_RECEIVING) {
        *detail = FWU_REJ_STATE;
        return PROTO_RESULT_REJECTED;
    }
    if (offset != s.next_offset) {
        *detail = FWU_REJ_OFFSET;                 /* FW_STATUS tells the CPM where to continue */
        return PROTO_RESULT_REJECTED;
    }
    if (dlen == 0u || dlen > FWU_CHUNK_MAX || (dlen & 1u) != 0u || offset + dlen > s.size) {
        *detail = FWU_REJ_CHUNK;
        return PROTO_RESULT_REJECTED;
    }
    memcpy(s.chunk, &val[5], dlen);
    s.chunk_offset = offset;
    s.chunk_len = dlen;
    s.chunk_pending = true;                       /* written by the main loop */
    return PROTO_RESULT_OK;
}

uint8_t fw_update_end(uint8_t *detail)
{
    *detail = 0;
    if (s.state == FWU_RECEIVING && s.chunk_pending) {
        return PROTO_RESULT_BUSY;
    }
    if (s.state == FWU_VERIFYING || s.state == FWU_VERIFIED) {
        return s.state == FWU_VERIFIED ? PROTO_RESULT_OK : PROTO_RESULT_IN_PROGRESS;
    }
    if (s.state != FWU_RECEIVING) {
        *detail = FWU_REJ_STATE;
        return PROTO_RESULT_REJECTED;
    }
    if (s.next_offset != s.size) {
        *detail = FWU_REJ_OFFSET;                 /* not all data received yet */
        return PROTO_RESULT_REJECTED;
    }
    s.verify_req = true;
    set_state(FWU_VERIFYING, FWU_ERR_NONE);
    return PROTO_RESULT_IN_PROGRESS;
}

uint8_t fw_update_activate(bool charging, uint8_t *detail)
{
    *detail = 0;
    if (charging) {
        *detail = FWU_REJ_CHARGING;
        return PROTO_RESULT_REJECTED;
    }
    if (s.state != FWU_VERIFIED) {
        *detail = FWU_REJ_STATE;
        return PROTO_RESULT_REJECTED;
    }
    s.activate_req = true;
    siu_log("fw: activating, resetting");
    return PROTO_RESULT_OK;
}

bool fw_update_status(uint8_t out[6])
{
    if (s.state == FWU_IDLE) {
        return false;
    }
    uint32_t off = s.next_offset;
    out[0] = s.state;
    out[1] = (uint8_t)off;
    out[2] = (uint8_t)(off >> 8);
    out[3] = (uint8_t)(off >> 16);
    out[4] = (uint8_t)(off >> 24);
    out[5] = s.error;
    return true;
}

void fw_update_abort(void)
{
    if (s.state != FWU_IDLE) {
        s.gen++;
        s.erase_req = s.verify_req = s.activate_req = false;
        s.chunk_pending = false;
        set_state(FWU_IDLE, FWU_ERR_NONE);
        siu_log("fw: transfer aborted");
    }
}

/* ---- main loop ------------------------------------------------------------------------------- */

/* Applies a main-loop result only if no FW_BEGIN / abort happened meanwhile. */
static bool still_current(uint32_t gen, uint8_t expected_state)
{
    return s.gen == gen && s.state == expected_state;
}

static void finish(uint32_t gen, uint8_t expected_state, uint8_t new_state, uint8_t err)
{
    uint32_t lk = fw_port_lock();
    if (still_current(gen, expected_state)) {
        set_state(new_state, err);
    }
    fw_port_unlock(lk);
}

void fw_update_poll(void)
{
    if (s.erase_req && fw_port_link_idle()) {
        uint32_t lk = fw_port_lock();
        uint32_t gen = s.gen;
        s.erase_req = false;
        fw_port_unlock(lk);
        bool ok = fw_port_erase_staging();       /* the CPU stalls here for up to ~1 s */
        fw_port_after_stall();
        finish(gen, FWU_ERASING, ok ? FWU_RECEIVING : FWU_ERROR, ok ? FWU_ERR_NONE : FWU_ERR_ERASE);
    }

    if (s.chunk_pending && fw_port_link_idle()) {
        uint32_t lk = fw_port_lock();
        uint32_t gen = s.gen;
        fw_port_unlock(lk);
        bool ok = fw_port_write(s.chunk_offset, s.chunk, s.chunk_len);   /* ~5 ms stall */
        fw_port_after_stall();
        lk = fw_port_lock();
        if (still_current(gen, FWU_RECEIVING)) {
            if (ok) {
                s.next_offset = s.chunk_offset + s.chunk_len;
            } else {
                set_state(FWU_ERROR, FWU_ERR_WRITE);
            }
            s.chunk_pending = false;
        }
        fw_port_unlock(lk);
    }

    if (s.verify_req) {
        uint32_t lk = fw_port_lock();
        uint32_t gen = s.gen;
        s.verify_req = false;
        fw_port_unlock(lk);

        const uint8_t *img = fw_port_staging();
        uint8_t err = FWU_ERR_NONE;
        uint32_t trailer = (uint32_t)img[s.size - 4u] | ((uint32_t)img[s.size - 3u] << 8) |
                           ((uint32_t)img[s.size - 2u] << 16) | ((uint32_t)img[s.size - 1u] << 24);
        /* Not a CRC over the whole file: data + its own CRC always gives the same residue
         * (0x2144DF1C), which can't tell one image from another. */
        if (trailer != s.crc || crc32(img, s.size - 4u) != s.crc) {
            err = FWU_ERR_CRC;
        } else {
            const fw_image_header_t *hdr = NULL;
            switch (fw_image_check(img, s.size, s.hw_model, &hdr, NULL)) {
            case FW_IMG_OK:
                if (hdr->image_size + 4u != s.size) {   /* header must describe exactly this file */
                    err = FWU_ERR_HEADER;
                }
                break;
            case FW_IMG_BAD_HW_MODEL: err = FWU_ERR_HW_MODEL; break;
            default:                  err = FWU_ERR_HEADER; break;
            }
        }
        finish(gen, FWU_VERIFYING, err == FWU_ERR_NONE ? FWU_VERIFIED : FWU_ERROR, err);
    }

    if (s.activate_req && fw_port_link_idle()) {
        s.activate_req = false;
        fw_port_activate_and_reset();
    }
}
