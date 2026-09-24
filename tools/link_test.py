#!/usr/bin/env python3
"""Link wiring test for the SIU's CPM link UART (temporary text firmware).

Prints every line the SIU sends, and sends a numbered "ping" line every 2 s,
checking that each one comes back as "echo: ping N".

    .venv/bin/python tools/link_test.py [--port /dev/cu.usbserial-0001]

Stop with Ctrl+C. Replaced by the CPM emulator once the protocol is in.
"""
import argparse
import sys
import time

import serial

DEFAULT_PORT = "/dev/cu.usbserial-0001"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=DEFAULT_PORT)
    ap.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args()

    try:
        link = serial.Serial(args.port, args.baud, timeout=0.1)
    except serial.SerialException as e:
        print(f"Can't open {args.port}: {e}", file=sys.stderr)
        return 1

    print(f"Listening on {args.port} at {args.baud} 8N1 — Ctrl+C to stop")
    sent = echoed = alive = 0
    next_ping = time.monotonic() + 1.0
    pending = bytearray()
    try:
        while True:
            now = time.monotonic()
            if now >= next_ping:
                sent += 1
                link.write(f"ping {sent}\r\n".encode())
                next_ping = now + 2.0

            pending += link.read(256)
            while b"\n" in pending:
                raw, _, pending = pending.partition(b"\n")
                line = raw.decode(errors="replace").strip()
                if not line:
                    continue
                if line.startswith("SIU alive"):
                    alive += 1
                if line.startswith("echo: ping"):
                    echoed += 1
                print(f"  SIU> {line}")
    except KeyboardInterrupt:
        pass
    finally:
        link.close()

    print(f"\nalive lines: {alive}, pings sent: {sent}, echoed: {echoed}")
    ok = alive > 0 and echoed >= sent - 1   # the last ping may still be in flight
    print("LINK OK" if ok else "LINK PROBLEM — check TX/RX crossing, GND, and 3.3 V setting")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
