#!/usr/bin/env python3
"""CPM emulator — drives an SIU over the CPM<->SIU protocol (cpm_siu_protocol.md) from a PC,
through a 3.3 V USB-serial adapter on the SIU's link UART.

Does what a CPM does: HELLO -> SESSION_START handshake, then polls every 20 ms with CP_SET
and LED_SET, retries lost requests, detects link loss and re-handshakes.

    .venv/bin/python tools/cpm_emulator.py [--port /dev/cu.usbserial-0001]

Commands while running (type + Enter):
    led <state> [pattern]   state by name or number, e.g. "led charging", "led 3"; pattern 0-4
    cp f | cp 12v | cp pwm <percent>
    ident                   ask the SIU to resend its identity
    bad                     send an unknown TLV (expect an ERROR back)
    dup                     send the next poll twice with the same SEQ (tests the SIU's duplicate cache)
    stop / go               pause / resume polling (stop > 200 ms: the SIU should show "no link")
    stats                   print link statistics
    quit
"""
from __future__ import annotations

import argparse
import queue
import random
import sys
import threading
import time

import serial

import siu_proto as p

DEFAULT_PORT = "/dev/cu.usbserial-0001"


class Link:
    def __init__(self, port: str, baud: int):
        self.ser = serial.Serial(port, baud, timeout=0.001)
        self.buf = bytearray()

    def send(self, frame: p.Frame) -> None:
        self.ser.write(p.encode_wire(frame))

    def recv(self, deadline: float) -> p.Frame | None:
        """Next valid frame before the deadline, or None."""
        while time.monotonic() < deadline:
            self.buf += self.ser.read(256)
            while b"\x00" in self.buf:
                wire, _, rest = self.buf.partition(b"\x00")
                self.buf = bytearray(rest)
                if not wire:
                    continue
                try:
                    return p.decode_raw(p.cobs_decode(bytes(wire)))
                except ValueError:
                    stats["bad_frames"] += 1
        return None

    def flush(self) -> None:
        self.ser.reset_input_buffer()
        self.buf.clear()


stats = {"polls": 0, "retries": 0, "lost": 0, "bad_frames": 0, "errors": 0, "handshakes": 0, "rtt_sum": 0.0}


class Emulator:
    def __init__(self, link: Link, args):
        self.link = link
        self.args = args
        self.seq = 0
        self.session = p.SESSION_NONE
        self.last_session = p.SESSION_NONE
        self.led = (p.UI_STATES.index("Available"), 0)
        self.cp = p.cp_set(p.CP_MODE_STATE_F)
        self.led_sent_at = 0.0
        self.extra: list[tuple[int, bytes]] = []
        self.dup_next = False
        self.paused = False
        self.last_status: str | None = None

    def next_seq(self) -> int:
        self.seq = (self.seq + 1) & 0xFF
        return self.seq

    def exchange(self, tlvs, session: int, retries: int = 2) -> p.Frame | None:
        """One request with retries (same SEQ, RETRY flag). Returns the matching response."""
        seq = self.next_seq()
        for attempt in range(retries + 1):
            flags = p.FLAG_RETRY if attempt else 0
            t0 = time.monotonic()
            self.link.send(p.Frame(flags, seq, session, list(tlvs)))
            deadline = t0 + self.args.rsp_timeout_ms / 1000
            while (rsp := self.link.recv(deadline)) is not None:
                if rsp.flags & p.FLAG_RSP and rsp.seq == seq:
                    stats["rtt_sum"] += time.monotonic() - t0
                    return rsp
            if attempt < retries:
                stats["retries"] += 1
        return None

    def handshake(self) -> bool:
        stats["handshakes"] += 1
        self.link.flush()
        print("-- handshake: sending HELLO every 100 ms ...")
        while True:
            rsp = self.exchange([p.hello()], p.SESSION_NONE, retries=0)
            if rsp is not None and rsp.find(p.HELLO_INFO) is not None:
                break
            time.sleep(0.1)
        print("-- SIU identity:\n   " + p.describe_identity(rsp).replace("\n", "\n   "))

        session = self.last_session
        while session in (p.SESSION_NONE, self.last_session):
            session = random.randint(1, 255)
        rsp = self.exchange([p.session_start(session, self.args.poll_ms, self.args.siu_timeout_ms)], session)
        if rsp is None or rsp.find(p.SESSION_ACK) != bytes([session]):
            print("-- SESSION_START not acknowledged, retrying handshake")
            return False
        self.session = self.last_session = session
        self.led_sent_at = 0.0                  # full state push on the first poll (§4.2)
        print(f"-- session 0x{session:02X} active (poll {self.args.poll_ms} ms, "
              f"SIU link timeout {self.args.siu_timeout_ms} ms)")
        return True

    def poll(self) -> bool:
        now = time.monotonic()
        tlvs = [self.cp]                                      # every poll (§6.2)
        if now - self.led_sent_at >= 1.0:                    # every ~1 s, or on change
            tlvs.append(p.led_set(*self.led))
            self.led_sent_at = now
        tlvs += self.extra
        self.extra = []

        stats["polls"] += 1
        rsp = self.exchange(tlvs, self.session)
        if rsp is not None and self.dup_next:
            self.dup_next = False
            # Resend the identical request (same SEQ): the SIU must answer from its cache.
            self.link.send(p.Frame(p.FLAG_RETRY, self.seq, self.session, tlvs))
            dup = self.link.recv(time.monotonic() + self.args.rsp_timeout_ms / 1000)
            same = dup is not None and p.encode_raw(dup) == p.encode_raw(rsp)
            print(f"-- duplicate SEQ {self.seq}: {'identical cached response' if same else 'MISMATCH'}")
        if rsp is None:
            return False

        for e in p.describe_errors(rsp):
            stats["errors"] += 1
            print(f"   SIU> {e}")
        if (v := rsp.find(p.STATUS_FAST)) is not None:
            s = str(p.StatusFast.parse(v))
            if s != self.last_status:
                print(f"   SIU> status: {s}")
                self.last_status = s
        if len(rsp.tlvs) > 1 and any(t in (p.SIU_UID, p.HELLO_INFO) for t, _ in rsp.tlvs):
            print("   SIU> " + p.describe_identity(rsp).replace("\n", "\n        "))
        return True

    def command(self, line: str) -> bool:
        words = line.split()
        if not words:
            return True
        cmd = words[0].lower()
        try:
            if cmd == "quit":
                return False
            if cmd == "led":
                name = words[1]
                states = [s.lower() for s in p.UI_STATES]
                state = int(name) if name.isdigit() else states.index(name.lower())
                self.led = (state, int(words[2]) if len(words) > 2 else 0)
                self.led_sent_at = 0.0
                print(f"-- LED_SET {p.UI_STATES[state] if state < len(p.UI_STATES) else state} pattern {self.led[1]}")
            elif cmd == "cp":
                mode = words[1].lower()
                if mode == "f":
                    self.cp = p.cp_set(p.CP_MODE_STATE_F)
                elif mode == "12v":
                    self.cp = p.cp_set(p.CP_MODE_CONST_12V)
                else:
                    self.cp = p.cp_set(p.CP_MODE_PWM, round(float(words[2]) * 10))
                print(f"-- CP_SET {self.cp[1].hex()}")
            elif cmd == "ident":
                self.extra.append((p.IDENT_GET, b""))
            elif cmd == "bad":
                self.extra.append((0x7E, b"\x01\x02"))
            elif cmd == "dup":
                self.dup_next = True
            elif cmd == "stop":
                self.paused = True
                print("-- polling paused")
            elif cmd == "go":
                self.paused = False
                print("-- polling resumed")
            elif cmd == "stats":
                print_stats()
            else:
                print(f"-- unknown command: {line}")
        except (IndexError, ValueError):
            print(f"-- can't parse: {line}")
        return True


def print_stats() -> None:
    answered = stats["polls"] - stats["lost"]
    rtt = stats["rtt_sum"] / answered * 1000 if answered > 0 else 0
    print(f"-- polls {stats['polls']}, retries {stats['retries']}, lost {stats['lost']}, "
          f"bad frames {stats['bad_frames']}, SIU errors {stats['errors']}, handshakes {stats['handshakes']}, "
          f"avg round trip {rtt:.1f} ms")


def stdin_reader(q: queue.Queue) -> None:
    for line in sys.stdin:
        q.put(line.strip())
    q.put("quit")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=DEFAULT_PORT)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--poll-ms", type=int, default=20, help="poll period (spec: 20)")
    ap.add_argument("--siu-timeout-ms", type=int, default=200, help="SIU link timeout sent in SESSION_START")
    ap.add_argument("--rsp-timeout-ms", type=int, default=30,
                    help="wait per attempt (spec: 10 ms; a PC + USB adapter needs more)")
    ap.add_argument("--cpm-link-timeout-ms", type=int, default=300,
                    help="no valid response for this long = link lost (spec: 100 ms)")
    ap.add_argument("--duration", type=float, default=0, help="run this many seconds, then exit (0 = until quit)")
    ap.add_argument("--commands", default="", help="';'-separated commands run at start, e.g. 'led charging'")
    args = ap.parse_args()

    try:
        link = Link(args.port, args.baud)
    except serial.SerialException as e:
        print(f"Can't open {args.port}: {e}", file=sys.stderr)
        return 1

    emu = Emulator(link, args)
    q: queue.Queue = queue.Queue()
    if args.duration == 0:
        threading.Thread(target=stdin_reader, args=(q,), daemon=True).start()
    for c in filter(None, (c.strip() for c in args.commands.split(";"))):
        q.put(c)

    end = time.monotonic() + args.duration if args.duration else None
    try:
        while not emu.handshake():
            time.sleep(0.1)
        last_ok = time.monotonic()
        next_poll = time.monotonic()
        running = True
        while running and (end is None or time.monotonic() < end):
            while not q.empty():
                running = emu.command(q.get()) and running
            now = time.monotonic()
            if now < next_poll:
                time.sleep(min(next_poll - now, 0.005))
                continue
            next_poll += args.poll_ms / 1000
            if next_poll < now:                      # fell behind (e.g. after a pause): don't burst
                next_poll = now + args.poll_ms / 1000
            if emu.paused:
                last_ok = now
                continue
            if emu.poll():
                last_ok = time.monotonic()
            else:
                stats["lost"] += 1
                if time.monotonic() - last_ok > args.cpm_link_timeout_ms / 1000:
                    print("-- LINK LOST (no valid response) — re-handshaking")
                    while not emu.handshake():
                        time.sleep(0.1)
                    last_ok = time.monotonic()
    except KeyboardInterrupt:
        pass
    finally:
        print_stats()
        link.ser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
