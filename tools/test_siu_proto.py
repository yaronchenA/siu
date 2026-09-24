#!/usr/bin/env python3
"""Checks tools/siu_proto.py against the worked examples in cpm_siu_protocol.md §10 —
the same reference bytes tests/test_protocol.c checks the C codec against."""
import os
import random
import sys

sys.path.insert(0, os.path.dirname(__file__))
import siu_proto as p  # noqa: E402

REQ_RAW = bytes.fromhex("10 05 7C 40 03 01 15 02 41 02 02 00 05 02 07 00 92 1C")
REQ_WIRE = bytes.fromhex("0C 10 05 7C 40 03 01 15 02 41 02 02 04 05 02 07 03 92 1C 00")
RSP_RAW = bytes.fromhex("11 05 7C 60 0D 03 70 17 84 D1 01 20 02 00 00 00 00 00 9B 32")
RSP_WIRE = bytes.fromhex("0E 11 05 7C 60 0D 03 70 17 84 D1 01 20 02 01 01 01 01 03 9B 32 00")

failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print(f"  FAIL {what}")


check(p.crc16(b"123456789") == 0x29B1, "CRC check value")

req = p.Frame(0, 5, 0x7C, [p.cp_set(p.CP_MODE_PWM, 533), p.led_set(2), p.event_ack(7)])
check(p.encode_raw(req) == REQ_RAW, "request raw bytes match §10")
check(p.encode_wire(req) == REQ_WIRE, "request wire bytes match §10")

rsp = p.decode_raw(p.cobs_decode(RSP_WIRE[:-1]))
check(rsp.flags == p.FLAG_RSP and rsp.seq == 5 and rsp.session == 0x7C, "response header")
st = p.StatusFast.parse(rsp.find(p.STATUS_FAST))
check((st.cp_state, st.cp_high_mv, st.cp_low_mv, st.pp_rating_a, st.lock_state) == (3, 6000, -11900, 32, 2),
      "STATUS_FAST fields")
check(p.encode_raw(rsp) == RSP_RAW, "response re-encodes to §10 bytes")

rng = random.Random(1)
for n in list(range(0, 600)) + [253, 254, 255, 508, 509]:
    for gen in (lambda: rng.randrange(256), lambda: rng.randrange(1, 256), lambda: rng.choice([0, 0, 0, 7])):
        data = bytes(gen() for _ in range(n))
        enc = p.cobs_encode(data)
        if 0 in enc or p.cobs_decode(enc) != data:
            check(False, f"COBS roundtrip len {n}")
            break

for bad in (b"\x03\x11\x00", b"\x05\x11\x22"):
    try:
        p.cobs_decode(bad)
        check(False, f"COBS should reject {bad.hex()}")
    except ValueError:
        pass

print("python codec:", "ok" if failures == 0 else f"{failures} FAILURES")
sys.exit(1 if failures else 0)
