"""Hardware-in-the-loop protocol tests: the real SIU firmware against cpm_siu_protocol.md.

Each test names the spec section it checks. Needs the SIU on the USB-serial adapter (see conftest.py).
"""
from __future__ import annotations

import struct
import time

import pytest

from conftest import errors, results, p


# ---- §4 handshake and identity ------------------------------------------------------

def test_hello_returns_identity(fresh):
    rsp = fresh.hello()
    assert rsp.flags == p.FLAG_RSP and rsp.session == p.SESSION_NONE

    major, minor, boot_id, reset, caps, lifecycle = struct.unpack("<BBIBIB", rsp.find(p.HELLO_INFO))
    assert (major, minor) == (1, 0)
    assert reset in p.RESET_REASONS
    assert lifecycle in (0, 1)

    assert len(rsp.find(p.SIU_UID)) == 12
    serial = rsp.find(p.SERIAL_NUMBER)
    assert 1 <= len(serial) <= 24 and serial.isascii()
    model, rev, rating, phases, ctype = struct.unpack("<HBBBB", rsp.find(p.HW_INFO))
    assert rating in (16, 32) and phases in (1, 3) and ctype in (0, 1)
    assert len(rsp.find(p.FW_INFO)) >= 8


def test_boot_id_stable_without_reboot(fresh):
    a = fresh.hello().find(p.HELLO_INFO)[2:6]
    b = fresh.hello().find(p.HELLO_INFO)[2:6]
    assert a == b


def test_session_start_ack_and_status(fresh):
    rsp = fresh.open_session()
    assert rsp.session == fresh.session
    st = rsp.find(p.STATUS_FAST)
    assert st is not None and len(st) == 13


def test_session_start_header_must_match(fresh):
    fresh.hello()
    rsp = fresh.request([p.session_start(7, 20, 200)], session=8)      # header 8, TLV 7
    assert rsp is None


@pytest.mark.parametrize("poll_ms,timeout_ms", [(1, 200), (20, 10), (20, 60000)])
def test_session_start_rejects_bad_parameters(fresh, poll_ms, timeout_ms):
    fresh.hello()
    assert fresh.request([p.session_start(9, poll_ms, timeout_ms)], session=9) is None


def test_unlinked_siu_ignores_polls(fresh):
    time.sleep(0.3)                                    # let any earlier session time out
    assert fresh.request([], session=5) is None


def test_session_zero_needs_hello(fresh):
    assert fresh.request([p.led_set(0)], session=p.SESSION_NONE) is None


def test_ident_get_in_session(siu):
    rsp = siu.poll([(p.IDENT_GET, b"")])
    assert rsp.find(p.SIU_UID) is not None and rsp.find(p.SERIAL_NUMBER) is not None


# ---- §2 framing and receive rules ---------------------------------------------------

def test_every_response_has_header_and_status(siu):
    for _ in range(20):
        seq = siu.next_seq()
        rsp = siu.poll(seq=seq)
        assert rsp.flags == p.FLAG_RSP and rsp.seq == seq and rsp.session == siu.session
        assert rsp.tlvs[0][0] == p.STATUS_FAST        # STATUS_FAST first, always


def test_wrong_session_dropped(siu):
    other = (siu.session % 255) + 1
    assert siu.request([], session=other) is None
    siu.poll()                                          # real session still fine


def test_bad_crc_dropped(siu):
    raw = bytearray(siu.raw_frame(0x10, siu.next_seq(), siu.session, b""))
    raw[-1] ^= 0xFF
    siu.link.send_raw(bytes(raw))
    siu.expect_silence()
    siu.poll()


def test_wrong_version_dropped(siu):
    siu.link.send_raw(siu.raw_frame(0x20, siu.next_seq(), siu.session, b""))   # VER 2
    siu.expect_silence()
    siu.poll()


def test_malformed_tlv_dropped(siu):
    payload = bytes([p.LED_SET, 5, 1])                  # claims 5 bytes, has 1
    siu.link.send_raw(siu.raw_frame(0x10, siu.next_seq(), siu.session, payload))
    siu.expect_silence()
    siu.poll()


def test_response_flag_frame_ignored(siu):
    assert siu.request([], flags=p.FLAG_RSP) is None   # e.g. our own echo on an RS485 bus
    siu.poll()


def test_oversize_frame_dropped(siu):
    payload = bytes([0x7E, 239]) + bytes(239)           # 241-byte payload: 1 over the limit
    siu.link.send_raw(siu.raw_frame(0x10, siu.next_seq(), siu.session, payload))
    siu.expect_silence()
    siu.poll()


def test_max_size_frame_accepted(siu):
    rsp = siu.poll([(0x7E, bytes(238))])                # exactly 240-byte payload
    assert errors(rsp) == [(0x7E, 1)]                   # UNKNOWN_TLV, but the frame was processed


def test_resync_after_noise_with_delimiter(siu):
    siu.link.send_bytes(b"\x55\xAA\x12\x34\x00")
    siu.poll()


def test_noise_without_delimiter_costs_one_frame(siu):
    # Noise glued to the front of a frame corrupts that frame (COBS), the next one is fine.
    siu.link.send_bytes(b"\x55\xAA\x12")
    assert siu.request([]) is None
    siu.poll()


# ---- §5 duplicates -----------------------------------------------------------------------

def test_duplicate_seq_returns_cached_response_without_reexecuting(siu):
    seq = siu.next_seq()
    first = siu.poll([p.config_set(siu.next_req_id(), p.CFG_LED_BRIGHTNESS, bytes([30]))], seq=seq)
    # Same SEQ, different content: must get the cached response, and 60 must NOT be applied.
    again = siu.poll([p.config_set(siu.next_req_id(), p.CFG_LED_BRIGHTNESS, bytes([60]))],
                     seq=seq, flags=p.FLAG_RETRY)
    assert p.encode_raw(again) == p.encode_raw(first)
    value = siu.poll([p.config_get(p.CFG_LED_BRIGHTNESS)]).find(p.CONFIG_VALUE)
    assert value == bytes([p.CFG_LED_BRIGHTNESS, 30])


def test_duplicate_req_id_reported_not_reexecuted(siu):
    req = siu.next_req_id()
    r1 = siu.poll([p.config_set(req, p.CFG_LED_BRIGHTNESS, bytes([30]))])
    r2 = siu.poll([p.config_set(req, p.CFG_LED_BRIGHTNESS, bytes([60]))])     # new SEQ, same REQ_ID
    assert results(r1) == results(r2) == [(req, p.CONFIG_SET, 0, 0)]
    value = siu.poll([p.config_get(p.CFG_LED_BRIGHTNESS)]).find(p.CONFIG_VALUE)
    assert value[1] == 30


def test_req_id_zero_rejected(siu):
    rsp = siu.poll([p.auth_feedback(0, 0)])
    assert errors(rsp) == [(p.AUTH_FEEDBACK, 3)]        # OUT_OF_RANGE


# ---- §6 timing and link loss ---------------------------------------------------------------

def test_sustained_polling_20ms(siu):
    n, lost, worst = 250, 0, 0.0
    next_t = time.monotonic()
    for _ in range(n):
        next_t += 0.020
        t0 = time.monotonic()
        if siu.request([p.cp_set(p.CP_MODE_STATE_F)]) is None:
            lost += 1
        worst = max(worst, time.monotonic() - t0)
        time.sleep(max(0.0, next_t - time.monotonic()))
    print(f"\n  {n} polls, {lost} lost, worst round trip {worst * 1000:.1f} ms (includes USB latency)")
    assert lost == 0
    assert worst < 0.030


def test_link_timeout_ends_session(siu):
    time.sleep(0.15)
    siu.poll()                                          # 150 ms < 200 ms: still active
    time.sleep(0.30)
    assert siu.request([]) is None                      # timed out: session gone
    siu.open_session()                                  # and a new handshake works


def test_link_timeout_is_taken_from_session_start(fresh):
    fresh.open_session(link_timeout_ms=600)
    time.sleep(0.35)
    fresh.poll()                                        # would have expired with the 200 ms default


def test_session_end(siu):
    siu.link.send(p.Frame(0, siu.next_seq(), siu.session, [(p.SESSION_END, b"\x00")]))
    siu.expect_silence()                                # no response to SESSION_END
    assert siu.request([]) is None                      # session is gone


def test_new_hello_replaces_session(siu):
    old = siu.session
    siu.hello()
    assert siu.request([], session=old) is None


# ---- §8 commands and errors --------------------------------------------------------------------

def test_led_set_accepted(siu):
    for state in range(len(p.UI_STATES)):
        assert errors(siu.poll([p.led_set(state)])) == []


def test_error_codes_in_request_order(siu):
    rsp = siu.poll([(0x7E, b"\x01"),                    # unknown type
                    (p.LED_SET, b"\x02"),               # too short
                    p.led_set(99),                      # out of range
                    p.cp_set(p.CP_MODE_PWM, 990),       # 99 %: not a valid duty
                    p.led_raw(1, 2, 3)])                # needs SERVICE
    assert errors(rsp) == [(0x7E, 1), (p.LED_SET, 2), (p.LED_SET, 3), (p.CP_SET, 3), (p.LED_RAW, 5)]


@pytest.mark.parametrize("duty", [50, 80, 533, 970])
def test_cp_set_valid_duties(siu, duty):
    assert errors(siu.poll([p.cp_set(p.CP_MODE_PWM, duty)])) == []


@pytest.mark.parametrize("duty", [0, 79, 971, 1000])
def test_cp_set_invalid_duties(siu, duty):
    assert errors(siu.poll([p.cp_set(p.CP_MODE_PWM, duty)])) == [(p.CP_SET, 3)]


def test_led_raw_with_service_flag(siu):
    assert errors(siu.poll([p.led_raw(1, 2, 3)], flags=p.FLAG_SERVICE)) == []


@pytest.mark.parametrize("name,code,expected", [("accepted", 0, 0), ("rejected", 1, 0), ("pending", 2, 0),
                                                ("expired", 3, 0), ("unknown", 9, 1)])
def test_auth_feedback_results(siu, name, code, expected):
    req = siu.next_req_id()
    assert results(siu.poll([p.auth_feedback(req, code)])) == [(req, p.AUTH_FEEDBACK, expected, expected)]


def test_config_brightness_roundtrip(siu):
    req = siu.next_req_id()
    assert results(siu.poll([p.config_set(req, p.CFG_LED_BRIGHTNESS, bytes([40]))])) == [(req, p.CONFIG_SET, 0, 0)]
    assert siu.poll([p.config_get(p.CFG_LED_BRIGHTNESS)]).find(p.CONFIG_VALUE) == bytes([1, 40])


@pytest.mark.parametrize("key,value", [(p.CFG_LED_BRIGHTNESS, 101), (0x7F, 1)])
def test_config_set_rejects(siu, key, value):
    req = siu.next_req_id()
    assert results(siu.poll([p.config_set(req, key, bytes([value]))])) == [(req, p.CONFIG_SET, 1, 1)]


def test_config_get_unknown_key(siu):
    assert errors(siu.poll([p.config_get(0x7F)])) == [(p.CONFIG_GET, 3)]
