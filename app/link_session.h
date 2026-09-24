/*
 * CPM link session — cpm_siu_protocol.md §4 (handshake), §5 (duplicates), §6 (timing).
 *
 *   UNLINKED --HELLO--> HANDSHAKE --SESSION_START--> ACTIVE
 *      ^                    |                          |
 *      +---- link timeout --+---- timeout / SESSION_END+     (new HELLO: back to HANDSHAKE)
 *
 * Takes one decoded raw frame at a time and produces the raw response frame (or nothing).
 * Pure logic: time, identity and status come in as arguments — host-tested in tests/.
 */
#ifndef LINK_SESSION_H
#define LINK_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t     uid[12];          /* STM32 96-bit unique ID */
    uint32_t    boot_id;          /* random per boot */
    uint8_t     reset_reason;     /* RESET_* */
    uint32_t    capabilities;     /* HELLO_INFO capability bits */
    uint8_t     lifecycle;        /* LIFECYCLE_* */
    const char *serial;           /* 1..24 ASCII chars */
    uint16_t    hw_model;
    uint8_t     hw_rev;
    uint8_t     connector_rating_a;
    uint8_t     phases;
    uint8_t     connector_type;
    uint8_t     fw_major, fw_minor, fw_patch;
    uint32_t    build_id;
    uint8_t     bootloader_ver;
} siu_identity_t;

/* STATUS_FAST contents (§8.4). */
typedef struct {
    uint8_t  cp_state;
    int16_t  cp_high_mv;
    int16_t  cp_low_mv;
    uint8_t  pp_state;
    uint8_t  pp_rating_a;
    uint8_t  lock_state;
    uint8_t  estop_loop;
    uint32_t fault_flags;
} siu_status_t;

typedef enum {
    LINK_UNLINKED = 0,
    LINK_HANDSHAKE,
    LINK_ACTIVE
} link_state_t;

typedef struct {
    uint32_t rx_frames;       /* valid frames handled */
    uint32_t bad_frames;      /* failed CRC / version / TLV structure */
    uint32_t wrong_dir;       /* a response arrived (echo, or another SIU) */
    uint32_t stale_session;   /* session ID didn't match */
    uint32_t dup_requests;    /* same SEQ again — answered from the cache */
    uint32_t link_losses;     /* timeouts out of HANDSHAKE / ACTIVE */
} link_stats_t;

#define LINK_DEFAULT_TIMEOUT_MS  200u   /* until SESSION_START sets it (§6.1) */

void link_session_init(const siu_identity_t *id, uint32_t now_ms);

/* One COBS-decoded frame in; returns the raw response length written to out (0 = no response). */
size_t link_session_handle(const uint8_t *raw, size_t len, const siu_status_t *status,
                           uint32_t now_ms, uint8_t *out, size_t cap);

/* Call regularly: detects the link timeout. */
void link_session_tick(uint32_t now_ms);

link_state_t link_session_state(void);
uint8_t link_session_id(void);
const link_stats_t *link_session_stats(void);

#endif /* LINK_SESSION_H */
