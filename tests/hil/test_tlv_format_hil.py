"""Hardware-in-the-loop: the exact TLV layouts the SIU sends (cpm_siu_protocol.md §8), and its debug log.
"""
from __future__ import annotations

import struct
import time

from conftest import p

# Value lengths from the spec tables (§8.1-§8.4, §8.7).
SPEC_LEN = {p.HELLO_INFO: 12, p.SIU_UID: 12, p.HW_INFO: 6, p.FW_INFO: 8, p.SESSION_ACK: 1,
            p.STATUS_FAST: 13, p.ERROR: 3, p.RESULT: 4}
LOG_RESPONSE_MAX = 90       # PROTO_LOG_RESPONSE_MAX: keeps a response inside the CPM's 10 ms timeout


def check_lengths(frame: p.Frame) -> None:
    for t, v in frame.tlvs:
        if t in SPEC_LEN:
            assert len(v) == SPEC_LEN[t], f"{p.tlv_name(t)} is {len(v)} bytes, spec says {SPEC_LEN[t]}"


def check_reencodes(siu) -> None:
    """The frame we decoded re-encodes to exactly the bytes that came off the wire."""
    raw = siu.link.last_raw
    assert p.encode_raw(p.decode_raw(raw)) == raw


# ---- layouts ---------------------------------------------------------------------------------

def test_hello_response_layout(fresh):
    rsp = fresh.hello()
    check_reencodes(fresh)
    types = [t for t, _ in rsp.tlvs]
    assert types == [p.HELLO_INFO, p.SIU_UID, p.SERIAL_NUMBER, p.HW_INFO, p.FW_INFO]
    check_lengths(rsp)
    assert 1 <= len(rsp.find(p.SERIAL_NUMBER)) <= 24


def test_session_ack_layout(fresh):
    rsp = fresh.open_session()
    check_reencodes(fresh)
    assert rsp.tlvs[0][0] == p.STATUS_FAST
    assert rsp.find(p.SESSION_ACK) == bytes([fresh.session])
    check_lengths(rsp)


def test_status_fast_layout(siu):
    v = siu.poll().find(p.STATUS_FAST)
    assert len(v) == 13
    cp_state, hi, lo, pp_state, pp_rating, lock, estop, faults = struct.unpack("<BhhBBBBI", v)
    assert cp_state in p.CP_STATES and lock in p.LOCK_STATES and estop in (0, 1) and pp_state in (0, 1, 2)


def test_error_layout(siu):
    rsp = siu.poll([(0x7E, b"\x01\x02")])
    check_reencodes(siu)
    check_lengths(rsp)
    assert rsp.find(p.ERROR) == bytes([0x7E, 1, 0])      # ref_type, UNKNOWN_TLV, ref_req_id 0


def test_result_layout(siu):
    req = siu.next_req_id()
    rsp = siu.poll([p.auth_feedback(req, 0)])
    check_lengths(rsp)
    assert rsp.find(p.RESULT) == bytes([req, p.AUTH_FEEDBACK, 0, 0])


def test_config_value_layout(siu):
    rsp = siu.poll([p.config_get(p.CFG_LED_BRIGHTNESS)])
    assert rsp.find(p.CONFIG_VALUE) == bytes([p.CFG_LED_BRIGHTNESS, 100])


def test_ident_get_layout(siu):
    rsp = siu.poll([(p.IDENT_GET, b"")])
    check_lengths(rsp)
    assert [t for t, _ in rsp.tlvs if t != p.LOG_TEXT] == \
        [p.STATUS_FAST, p.SIU_UID, p.SERIAL_NUMBER, p.HW_INFO, p.FW_INFO]


def test_every_response_reencodes_exactly(siu):
    for tlvs in ([], [p.led_set(2)], [(0x7E, b"")], [p.config_get(1)], [(p.IDENT_GET, b"")]):
        siu.poll(tlvs)
        check_reencodes(siu)


# ---- debug log (LOG_TEXT, §8.8) ------------------------------------------------------------

def test_log_off_by_default_after_boot_config(fresh):
    fresh.open_session()
    fresh.poll([p.config_set(fresh.next_req_id(), p.CFG_LOG_ENABLE, b"\x00")])
    rsp = fresh.poll([p.led_set(3)])
    assert rsp.find(p.LOG_TEXT) is None


def test_log_is_whole_lines_and_within_budget(siu):
    siu.poll([p.led_set(2), (0x7E, b""), p.cp_set(p.CP_MODE_PWM, 533)])   # makes several log lines
    for _ in range(10):
        rsp = siu.poll()
        text = rsp.find(p.LOG_TEXT)
        if text is not None:
            assert text.endswith(b"\n") and text.isascii()
            assert len(siu.link.last_raw) <= LOG_RESPONSE_MAX


def test_log_reports_commands(siu):
    siu.poll([p.led_set(2)])
    siu.wait_log("led: state 2 pattern 0")
    siu.poll([p.cp_set(p.CP_MODE_PWM, 267)])
    siu.wait_log("cp: mode 1 duty 26.7%")
    siu.poll([p.config_set(siu.next_req_id(), p.CFG_LED_BRIGHTNESS, bytes([55]))])
    siu.wait_log("config: key 0x01 = 55 ok")


def test_log_reports_errors_and_duplicates(siu):
    siu.poll([(0x7E, b"")])
    siu.wait_log("err: TLV 0x7e -> error 1")
    req = siu.next_req_id()
    siu.poll([p.auth_feedback(req, 0)])
    siu.poll([p.auth_feedback(req, 0)])
    siu.wait_log(f"dup: REQ_ID {req}")


def test_log_reports_dropped_frames(siu):
    raw = bytearray(siu.raw_frame(0x10, siu.next_seq(), siu.session, b""))
    raw[-1] ^= 0xFF
    siu.link.send_raw(bytes(raw))
    siu.expect_silence()
    siu.wait_log("rx: dropped frame")


def test_log_reports_link_timeout(siu):
    time.sleep(0.3)                          # let the session time out
    siu.open_session()
    siu.enable_log()
    siu.wait_log("link: timeout")
