"""Serial link to an SIU: sends and receives COBS frames over the CPM link UART.

Shared by the CPM emulator and the hardware-in-the-loop tests.
"""
from __future__ import annotations

import time

import serial

import siu_proto as p

DEFAULT_PORT = "/dev/cu.usbserial-0001"
DEFAULT_BAUD = 115200


class Link:
    def __init__(self, port: str = DEFAULT_PORT, baud: int = DEFAULT_BAUD):
        self.ser = serial.Serial(port, baud, timeout=0.001)
        self.buf = bytearray()
        self.bad_frames = 0
        self.trace = False          # print every frame, TLV by TLV
        self.on_log = None          # callable(line) for the SIU's LOG_TEXT lines
        self.last_raw = b""         # raw bytes of the last valid frame received
        # A lone delimiter flushes whatever the SIU's receiver holds (e.g. a glitch byte from the
        # adapter when the port opened), so the first real frame isn't glued to it.
        self.ser.write(b"\x00")
        time.sleep(0.002)

    def close(self) -> None:
        self.ser.close()

    # ---- sending ------------------------------------------------------------

    def send(self, frame: p.Frame) -> None:
        if self.trace:
            print(p.format_frame("TX ->", frame))
        self.ser.write(p.encode_wire(frame))

    def send_raw(self, raw: bytes) -> None:
        """A raw (un-encoded) frame — lets tests send broken CRCs, bad versions, etc."""
        if self.trace:
            print(f"TX -> raw {raw.hex(' ').upper()}")
        self.ser.write(p.cobs_encode(raw) + b"\x00")

    def send_bytes(self, data: bytes) -> None:
        """Bytes straight onto the wire, no framing — for noise and resync tests."""
        self.ser.write(data)

    # ---- receiving ------------------------------------------------------------

    def recv(self, deadline: float) -> p.Frame | None:
        """Next valid frame before the deadline (time.monotonic()), or None."""
        while time.monotonic() < deadline:
            self.buf += self.ser.read(256)
            while b"\x00" in self.buf:
                wire, _, rest = self.buf.partition(b"\x00")
                self.buf = bytearray(rest)
                if not wire:
                    continue
                try:
                    raw = p.cobs_decode(bytes(wire))
                    frame = p.decode_raw(raw)
                    self.last_raw = raw
                except ValueError as e:
                    self.bad_frames += 1
                    if self.trace:
                        print(f"RX <- BAD FRAME ({e}): {bytes(wire).hex(' ').upper()}")
                    continue
                if self.trace:
                    print(p.format_frame("RX <-", frame))
                if self.on_log:
                    for line in p.log_lines(frame):
                        self.on_log(line)
                return frame
        return None

    def recv_wire(self, deadline: float) -> bytes | None:
        """Next complete wire frame (COBS bytes, without delimiter) before the deadline, or None."""
        while time.monotonic() < deadline:
            self.buf += self.ser.read(256)
            if b"\x00" in self.buf:
                wire, _, rest = self.buf.partition(b"\x00")
                self.buf = bytearray(rest)
                if wire:
                    return bytes(wire)
        return None

    def flush(self) -> None:
        time.sleep(0.005)
        self.ser.reset_input_buffer()
        self.buf.clear()
