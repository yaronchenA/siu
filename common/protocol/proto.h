/*
 * CPM <-> SIU protocol — constants and message types (cpm_siu_protocol.md v0.2).
 * Shared by the SIU and CPM firmware; the Python tools mirror these values (tools/siu_proto.py).
 */
#ifndef PROTO_H
#define PROTO_H

/* ---- frame (§2) ---------------------------------------------------------- */

#define PROTO_FRAME_VER     1u      /* header VER nibble */
#define PROTO_MSG_MAJOR     1u      /* message-level version, negotiated in HELLO (§8.1) */
#define PROTO_MSG_MINOR     0u

#define PROTO_HDR_LEN       3u      /* VER|FLAGS, SEQ, SESSION_ID */
#define PROTO_CRC_LEN       2u
#define PROTO_MAX_PAYLOAD   240u
#define PROTO_MAX_RAW       (PROTO_HDR_LEN + PROTO_MAX_PAYLOAD + PROTO_CRC_LEN)   /* 245 */
#define PROTO_MAX_WIRE      248u    /* COBS-encoded max frame + 0x00 delimiter (§2.2) */
#define PROTO_MIN_RAW       (PROTO_HDR_LEN + PROTO_CRC_LEN)

#define PROTO_FLAG_RSP          0x1u    /* response (SIU -> CPM) */
#define PROTO_FLAG_RETRY        0x2u    /* retransmitted request */
#define PROTO_FLAG_EVT_PENDING  0x4u    /* SIU has more events queued */
#define PROTO_FLAG_SERVICE      0x8u    /* service/test commands allowed */

#define PROTO_SESSION_NONE  0x00u       /* handshake only */

/* ---- TLV types (§8) -------------------------------------------------------- */

enum {
    /* link & session (§8.1) */
    TLV_HELLO          = 0x01,
    TLV_HELLO_INFO     = 0x02,
    TLV_SESSION_START  = 0x03,
    TLV_SESSION_ACK    = 0x04,
    TLV_EVENT_ACK      = 0x05,
    TLV_ERROR          = 0x06,
    TLV_RESULT         = 0x07,
    TLV_SESSION_END    = 0x08,
    TLV_TIME_SYNC      = 0x09,
    TLV_AUTH_CHALLENGE = 0x0A,
    TLV_AUTH_RESPONSE  = 0x0B,

    /* identity (§8.2) */
    TLV_SIU_UID        = 0x20,
    TLV_SERIAL_NUMBER  = 0x21,
    TLV_HW_INFO        = 0x22,
    TLV_FW_INFO        = 0x23,
    TLV_IDENT_GET      = 0x24,
    TLV_SIU_CERT       = 0x25,

    /* commands (§8.3) */
    TLV_CP_SET         = 0x40,
    TLV_LED_SET        = 0x41,
    TLV_LED_RAW        = 0x42,
    TLV_LOCK_CMD       = 0x43,
    TLV_BUZZER         = 0x44,
    TLV_AUTH_FEEDBACK  = 0x45,
    TLV_RFID_CTRL      = 0x46,
    TLV_TELEMETRY_CFG  = 0x47,
    TLV_SELF_TEST      = 0x48,
    TLV_SIU_RESET      = 0x49,

    /* status & telemetry (§8.4) */
    TLV_STATUS_FAST    = 0x60,
    TLV_TEMPERATURES   = 0x61,
    TLV_VOLTAGES       = 0x62,
    TLV_PP_DETAIL      = 0x63,
    TLV_FAULTS         = 0x64,
    TLV_AC_SENSE       = 0x65,

    /* events (§8.5) */
    TLV_EVENT          = 0x80,

    /* firmware update (§8.6) */
    TLV_FW_BEGIN       = 0xA0,
    TLV_FW_CHUNK       = 0xA1,
    TLV_FW_END         = 0xA2,
    TLV_FW_ACTIVATE    = 0xA3,
    TLV_FW_STATUS      = 0xA8,

    /* configuration (§8.7) */
    TLV_CONFIG_GET       = 0xC0,
    TLV_CONFIG_SET       = 0xC1,
    TLV_CONFIG_VALUE     = 0xC2,
    TLV_FACTORY_COMPLETE = 0xC3,

    /* diagnostics & debug (§8.8) */
    TLV_DIAG_GET       = 0xE0,
    TLV_DIAG_COUNTERS  = 0xE1,
    TLV_FAULT_CLEAR    = 0xE2,
    TLV_LOG_TEXT       = 0xF0
};

/* Fixed value lengths (minimums — longer values are accepted, §3). */
#define TLV_LEN_HELLO           14u
#define TLV_LEN_HELLO_INFO      12u
#define TLV_LEN_SESSION_START    5u
#define TLV_LEN_SESSION_ACK      1u
#define TLV_LEN_EVENT_ACK        2u
#define TLV_LEN_ERROR            3u
#define TLV_LEN_SESSION_END      1u
#define TLV_LEN_HW_INFO          6u
#define TLV_LEN_FW_INFO          8u
#define TLV_LEN_CP_SET           3u
#define TLV_LEN_LED_SET          2u
#define TLV_LEN_LED_RAW          3u
#define TLV_LEN_AUTH_FEEDBACK    2u
#define TLV_LEN_RESULT           4u
#define TLV_LEN_CONFIG_GET       1u
#define TLV_LEN_CONFIG_SET_MIN   3u      /* req_id, key, >= 1 value byte */
#define TLV_LEN_STATUS_FAST     13u

/* ERROR.code (§8.10) */
enum {
    PROTO_ERR_UNKNOWN_TLV          = 1,
    PROTO_ERR_BAD_LENGTH           = 2,
    PROTO_ERR_OUT_OF_RANGE         = 3,
    PROTO_ERR_NOT_ALLOWED_IN_STATE = 4,
    PROTO_ERR_SERVICE_REQUIRED     = 5,
    PROTO_ERR_SAFETY_REJECT        = 6,
    PROTO_ERR_CAPABILITY_MISSING   = 7
};

/* RESULT.result (§8.1) */
enum {
    PROTO_RESULT_OK          = 0,
    PROTO_RESULT_REJECTED    = 1,
    PROTO_RESULT_BUSY        = 2,
    PROTO_RESULT_FAILED      = 3,
    PROTO_RESULT_IN_PROGRESS = 4
};

/* CP_SET.mode (§8.3) */
enum {
    CP_MODE_CONST_12V = 0,
    CP_MODE_PWM       = 1,
    CP_MODE_STATE_F   = 2
};

/* HELLO_INFO.reset_reason (§8.1) */
enum {
    RESET_POWER_ON  = 0,
    RESET_BROWN_OUT = 1,
    RESET_WATCHDOG  = 2,
    RESET_SOFTWARE  = 3,
    RESET_FW_UPDATE = 4,
    RESET_PIN       = 5
};

/* HELLO_INFO.lifecycle (§8.7.1) */
enum {
    LIFECYCLE_FACTORY    = 0,
    LIFECYCLE_PRODUCTION = 1
};

#endif /* PROTO_H */
