"""CPM <-> SIU protocol codec (cpm_siu_protocol.md v0.2) — Python mirror of common/protocol.

Used by the CPM emulator and bench tools. Keep the constants in sync with common/protocol/proto.h.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field

# ---- frame (§2) ---------------------------------------------------------------

FRAME_VER = 1
MSG_MAJOR, MSG_MINOR = 1, 0
HDR_LEN, CRC_LEN = 3, 2
MAX_PAYLOAD = 240

FLAG_RSP, FLAG_RETRY, FLAG_EVT_PENDING, FLAG_SERVICE = 0x1, 0x2, 0x4, 0x8
SESSION_NONE = 0x00

# ---- TLV types (§8) ----------------------------------------------------------

HELLO, HELLO_INFO, SESSION_START, SESSION_ACK = 0x01, 0x02, 0x03, 0x04
EVENT_ACK, ERROR, RESULT, SESSION_END, TIME_SYNC = 0x05, 0x06, 0x07, 0x08, 0x09
SIU_UID, SERIAL_NUMBER, HW_INFO, FW_INFO, IDENT_GET, SIU_CERT = 0x20, 0x21, 0x22, 0x23, 0x24, 0x25
CP_SET, LED_SET, LED_RAW, LOCK_CMD, BUZZER = 0x40, 0x41, 0x42, 0x43, 0x44
AUTH_FEEDBACK, RFID_CTRL, TELEMETRY_CFG, SELF_TEST, SIU_RESET = 0x45, 0x46, 0x47, 0x48, 0x49
STATUS_FAST, TEMPERATURES, VOLTAGES, PP_DETAIL, FAULTS, AC_SENSE = 0x60, 0x61, 0x62, 0x63, 0x64, 0x65
EVENT = 0x80
CONFIG_GET, CONFIG_SET, CONFIG_VALUE, FACTORY_COMPLETE = 0xC0, 0xC1, 0xC2, 0xC3
LOG_TEXT = 0xF0

TLV_NAMES = {v: k for k, v in globals().items() if k.isupper() and isinstance(v, int) and 0 < v < 0xFF
             and k not in ("FRAME_VER", "MSG_MAJOR", "MSG_MINOR", "HDR_LEN", "CRC_LEN", "MAX_PAYLOAD")
             and not k.startswith("FLAG_")}

ERROR_NAMES = {1: "UNKNOWN_TLV", 2: "BAD_LENGTH", 3: "OUT_OF_RANGE", 4: "NOT_ALLOWED_IN_STATE",
               5: "SERVICE_REQUIRED", 6: "SAFETY_REJECT", 7: "CAPABILITY_MISSING"}

CP_MODE_CONST_12V, CP_MODE_PWM, CP_MODE_STATE_F = 0, 1, 2

UI_STATES = ["Available", "SuspendedEVSE", "Charging", "Faulted", "Reserved", "Stopped", "Updating",
             "Authorizing", "SuspendedEV", "Preparing", "Finishing", "Unavailable", "PendingApproval"]

RESET_REASONS = {0: "power-on", 1: "brown-out", 2: "watchdog", 3: "software", 4: "fw-update", 5: "pin"}
CP_STATES = {0: "unknown", 1: "A", 2: "B", 3: "C", 4: "D", 5: "E", 6: "F"}
LOCK_STATES = {0: "unknown", 1: "unlocked", 2: "locked", 3: "moving", 4: "fault"}


# ---- CRC-16/CCITT-FALSE -------------------------------------------------------

def crc16(data: bytes, crc: int = 0xFFFF) -> int:
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


# ---- COBS -------------------------------------------------------------------------

def cobs_encode(data: bytes) -> bytes:
    """Encodes without the trailing 0x00 delimiter."""
    out = bytearray()
    block = bytearray()
    for b in data:
        if b == 0:
            out.append(len(block) + 1)
            out += block
            block = bytearray()
        else:
            block.append(b)
            if len(block) == 254:
                out.append(0xFF)
                out += block
                block = bytearray()
    out.append(len(block) + 1)
    out += block
    return bytes(out)


def cobs_decode(data: bytes) -> bytes:
    """Decodes one frame (without delimiter). Raises ValueError on malformed input."""
    out = bytearray()
    i = 0
    while i < len(data):
        code = data[i]
        i += 1
        if code == 0 or i + code - 1 > len(data):
            raise ValueError("malformed COBS")
        chunk = data[i:i + code - 1]
        if 0 in chunk:
            raise ValueError("zero inside COBS frame")
        out += chunk
        i += code - 1
        if code != 0xFF and i < len(data):
            out.append(0)
    return bytes(out)


# ---- frames and TLVs ----------------------------------------------------------------

@dataclass
class Frame:
    flags: int
    seq: int
    session: int
    tlvs: list[tuple[int, bytes]] = field(default_factory=list)
    ver: int = FRAME_VER

    def find(self, tlv_type: int) -> bytes | None:
        return next((v for t, v in self.tlvs if t == tlv_type), None)

    def find_all(self, tlv_type: int) -> list[bytes]:
        return [v for t, v in self.tlvs if t == tlv_type]


def encode_raw(frame: Frame) -> bytes:
    body = bytearray([(frame.ver << 4) | (frame.flags & 0x0F), frame.seq & 0xFF, frame.session & 0xFF])
    payload_len = 0
    for t, v in frame.tlvs:
        body += bytes([t, len(v)]) + v
        payload_len += 2 + len(v)
    if payload_len > MAX_PAYLOAD:
        raise ValueError(f"payload {payload_len} > {MAX_PAYLOAD} bytes")
    return bytes(body) + struct.pack("<H", crc16(body))


def encode_wire(frame: Frame) -> bytes:
    return cobs_encode(encode_raw(frame)) + b"\x00"


def decode_raw(raw: bytes) -> Frame:
    """Raises ValueError for anything the receive rules (§2.3) would drop."""
    if len(raw) < HDR_LEN + CRC_LEN:
        raise ValueError("too short")
    body, crc = raw[:-2], struct.unpack("<H", raw[-2:])[0]
    if crc16(body) != crc:
        raise ValueError("bad CRC")
    ver, flags = body[0] >> 4, body[0] & 0x0F
    if ver != FRAME_VER:
        raise ValueError(f"unknown frame version {ver}")
    tlvs, p, pos = [], body[HDR_LEN:], 0
    while pos < len(p):
        if len(p) - pos < 2 or len(p) - pos - 2 < p[pos + 1]:
            raise ValueError("malformed TLV")
        t, n = p[pos], p[pos + 1]
        tlvs.append((t, bytes(p[pos + 2:pos + 2 + n])))
        pos += 2 + n
    return Frame(flags, body[1], body[2], tlvs, ver)


# ---- TLV value helpers ---------------------------------------------------------------

def hello(cpm_uid: bytes = bytes(12)) -> tuple[int, bytes]:
    return HELLO, bytes([MSG_MAJOR, MSG_MINOR]) + cpm_uid[:12].ljust(12, b"\0")


def session_start(session: int, poll_ms: int, timeout_ms: int) -> tuple[int, bytes]:
    return SESSION_START, struct.pack("<BHH", session, poll_ms, timeout_ms)


def cp_set(mode: int, duty_0p1pct: int = 0) -> tuple[int, bytes]:
    return CP_SET, struct.pack("<BH", mode, duty_0p1pct)


def led_set(ui_state: int, pattern: int = 0) -> tuple[int, bytes]:
    return LED_SET, bytes([ui_state, pattern])


def event_ack(last_evt_seq: int) -> tuple[int, bytes]:
    return EVENT_ACK, struct.pack("<H", last_evt_seq)


def led_raw(r: int, g: int, b: int) -> tuple[int, bytes]:
    return LED_RAW, bytes([r, g, b])


AUTH_RESULTS = {"accepted": 0, "rejected": 1, "pending": 2, "expired": 3}


def auth_feedback(req_id: int, result: int) -> tuple[int, bytes]:
    return AUTH_FEEDBACK, bytes([req_id, result])


CFG_LED_BRIGHTNESS = 0x01


def config_set(req_id: int, key: int, value: bytes) -> tuple[int, bytes]:
    return CONFIG_SET, bytes([req_id, key]) + value


def config_get(key: int) -> tuple[int, bytes]:
    return CONFIG_GET, bytes([key])


RESULT_NAMES = {0: "OK", 1: "REJECTED", 2: "BUSY", 3: "FAILED", 4: "IN_PROGRESS"}


@dataclass
class StatusFast:
    cp_state: int
    cp_high_mv: int
    cp_low_mv: int
    pp_state: int
    pp_rating_a: int
    lock_state: int
    estop_loop: int
    fault_flags: int

    @classmethod
    def parse(cls, v: bytes) -> "StatusFast":
        return cls(*struct.unpack_from("<BhhBBBBI", v))

    def __str__(self) -> str:
        return (f"CP {CP_STATES.get(self.cp_state, self.cp_state)} "
                f"({self.cp_high_mv / 1000:+.1f}/{self.cp_low_mv / 1000:+.1f} V), "
                f"PP {self.pp_state}/{self.pp_rating_a} A, lock {LOCK_STATES.get(self.lock_state, self.lock_state)}, "
                f"E-stop {'OPEN' if self.estop_loop else 'ok'}, faults 0x{self.fault_flags:08X}")


def describe_identity(f: Frame) -> str:
    lines = []
    if (v := f.find(HELLO_INFO)) is not None:
        major, minor, boot_id, reset, caps, life = struct.unpack_from("<BBIBIB", v)
        lines.append(f"protocol {major}.{minor}, boot_id 0x{boot_id:08X}, reset: {RESET_REASONS.get(reset, reset)}, "
                     f"capabilities 0x{caps:08X}, lifecycle: {'production' if life else 'FACTORY'}")
    if (v := f.find(SIU_UID)) is not None:
        lines.append(f"UID {v.hex().upper()}")
    if (v := f.find(SERIAL_NUMBER)) is not None:
        lines.append(f"serial {v.decode(errors='replace')}")
    if (v := f.find(HW_INFO)) is not None:
        model, rev, rating, phases, ctype = struct.unpack_from("<HBBBB", v)
        lines.append(f"hardware model 0x{model:04X} rev {rev}, {rating} A, {phases}-phase, "
                     f"{'socket' if ctype == 0 else 'tethered'}")
    if (v := f.find(FW_INFO)) is not None:
        ma, mi, pa, build, boot = struct.unpack_from("<BBBIB", v)
        lines.append(f"firmware {ma}.{mi}.{pa} build {build:08x}, bootloader {boot}")
    return "\n".join(lines)


def describe_results(f: Frame) -> list[str]:
    out = []
    for v in f.find_all(RESULT):
        req_id, ref, result, detail = v[:4]
        out.append(f"RESULT req {req_id} {TLV_NAMES.get(ref, f'0x{ref:02X}')}: "
                   f"{RESULT_NAMES.get(result, result)} (detail {detail})")
    for v in f.find_all(CONFIG_VALUE):
        out.append(f"CONFIG_VALUE key 0x{v[0]:02X} = {v[1:].hex()}")
    return out


def describe_errors(f: Frame) -> list[str]:
    out = []
    for v in f.find_all(ERROR):
        ref, code = v[0], v[1]
        out.append(f"ERROR {ERROR_NAMES.get(code, code)} for {TLV_NAMES.get(ref, f'0x{ref:02X}')}")
    return out


# ---- human-readable decoding (for --trace and tests) ----------------------------------------

CFG_LOG_ENABLE = 0x10
CFG_KEY_NAMES = {CFG_LED_BRIGHTNESS: "LED brightness", CFG_LOG_ENABLE: "log enable"}
CP_MODE_NAMES = {CP_MODE_CONST_12V: "CONST_12V", CP_MODE_PWM: "PWM", CP_MODE_STATE_F: "STATE_F"}
PATTERN_NAMES = {0: "default", 1: "solid", 2: "blink", 3: "flicker", 4: "breathe"}
AUTH_NAMES = {v: k for k, v in AUTH_RESULTS.items()}
FLAG_NAMES = [(FLAG_RSP, "RSP"), (FLAG_RETRY, "RETRY"), (FLAG_EVT_PENDING, "EVT_PENDING"), (FLAG_SERVICE, "SERVICE")]


def tlv_name(t: int) -> str:
    return TLV_NAMES.get(t, f"UNKNOWN_0x{t:02X}")


def describe_tlv(t: int, v: bytes) -> str:
    """One-line meaning of a TLV value; falls back to '' if it can't be decoded."""
    try:
        if t == HELLO:
            return f"CPM protocol {v[0]}.{v[1]}, CPM UID {v[2:14].hex().upper()}"
        if t == HELLO_INFO:
            major, minor, boot, reset, caps, life = struct.unpack_from("<BBIBIB", v)
            return (f"protocol {major}.{minor}, boot_id 0x{boot:08X}, reset {RESET_REASONS.get(reset, reset)}, "
                    f"caps 0x{caps:08X}, {'production' if life else 'FACTORY'}")
        if t == SESSION_START:
            sid, poll, tmo = struct.unpack_from("<BHH", v)
            return f"session 0x{sid:02X}, poll {poll} ms, SIU link timeout {tmo} ms"
        if t == SESSION_ACK:
            return f"session 0x{v[0]:02X}"
        if t == EVENT_ACK:
            return f"events acknowledged up to #{struct.unpack_from('<H', v)[0]}"
        if t == ERROR:
            return f"{ERROR_NAMES.get(v[1], v[1])} for {tlv_name(v[0])}" + (f" (req {v[2]})" if v[2] else "")
        if t == RESULT:
            return f"req {v[0]} {tlv_name(v[1])}: {RESULT_NAMES.get(v[2], v[2])}, detail {v[3]}"
        if t == SESSION_END:
            return f"reason {v[0]}"
        if t == SIU_UID:
            return v.hex().upper()
        if t == SERIAL_NUMBER:
            return repr(v.decode(errors="replace"))
        if t == HW_INFO:
            model, rev, rating, phases, ctype = struct.unpack_from("<HBBBB", v)
            return f"model 0x{model:04X} rev {rev}, {rating} A, {phases}-phase, {'socket' if ctype == 0 else 'tethered'}"
        if t == FW_INFO:
            ma, mi, pa, build, boot = struct.unpack_from("<BBBIB", v)
            return f"firmware {ma}.{mi}.{pa} build {build:08x}, bootloader {boot}"
        if t == CP_SET:
            mode, duty = struct.unpack_from("<BH", v)
            return CP_MODE_NAMES.get(mode, str(mode)) + (f" {duty / 10:.1f} %" if mode == CP_MODE_PWM else "")
        if t == LED_SET:
            state = UI_STATES[v[0]] if v[0] < len(UI_STATES) else f"state {v[0]}"
            return f"{state}, pattern {PATTERN_NAMES.get(v[1], v[1])}"
        if t == LED_RAW:
            return f"R {v[0]} G {v[1]} B {v[2]}"
        if t == AUTH_FEEDBACK:
            return f"req {v[0]}, {AUTH_NAMES.get(v[1], f'result {v[1]}')}"
        if t == CONFIG_SET:
            return f"req {v[0]}, {CFG_KEY_NAMES.get(v[1], f'key 0x{v[1]:02X}')} = {v[2:].hex()}"
        if t in (CONFIG_GET, CONFIG_VALUE):
            val = f" = {v[1:].hex()}" if t == CONFIG_VALUE else ""
            return f"{CFG_KEY_NAMES.get(v[0], f'key 0x{v[0]:02X}')}{val}"
        if t == STATUS_FAST:
            return str(StatusFast.parse(v))
        if t == LOG_TEXT:
            return " | ".join(v.decode(errors="replace").splitlines())
    except (IndexError, struct.error):
        return "(too short to decode)"
    return ""


def format_frame(direction: str, f: Frame) -> str:
    """Multi-line dump: header, then every TLV with its raw bytes and meaning."""
    flags = " ".join(n for bit, n in FLAG_NAMES if f.flags & bit) or "-"
    raw = encode_raw(f)
    lines = [f"{direction} seq {f.seq:3d} session 0x{f.session:02X} flags {flags:<11} "
             f"({len(raw)} bytes raw, {len(encode_wire(f))} on the wire)"]
    for t, v in f.tlvs:
        meaning = describe_tlv(t, v)
        hexv = v.hex(" ").upper() if len(v) <= 16 else v[:16].hex(" ").upper() + " ..."
        lines.append(f"      0x{t:02X} {tlv_name(t):<14} len {len(v):3d}: {hexv:<50} {meaning}")
    return "\n".join(lines)


def log_lines(f: Frame) -> list[str]:
    """Debug text lines the SIU sent in LOG_TEXT TLVs."""
    out = []
    for v in f.find_all(LOG_TEXT):
        out += v.decode(errors="replace").splitlines()
    return out
