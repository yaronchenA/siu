"""Hardware-in-the-loop test fixtures: a real SIU on a USB-serial adapter.

    make hil                                   # or:
    .venv/bin/python -m pytest tests/hil -v --port /dev/cu.usbserial-0001

All tests are skipped if the port can't be opened.
"""
from __future__ import annotations

import os
import random
import struct
import sys
import time

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools"))
import siu_proto as p  # noqa: E402
from siu_link import DEFAULT_PORT, Link  # noqa: E402

RSP_TIMEOUT_S = 0.05      # PC + USB adapter latency; the SIU itself answers within ~1 ms
SILENCE_S = 0.08          # how long "no response" is waited for


def pytest_addoption(parser):
    parser.addoption("--port", default=os.environ.get("SIU_PORT", DEFAULT_PORT), help="SIU link serial port")
    parser.addoption("--trace-frames", action="store_true", help="print every frame, TLV by TLV")


class Siu:
    """A minimal, explicit CPM: every test controls exactly what goes on the wire."""

    def __init__(self, link: Link):
        self.link = link
        self.log: list[str] = []          # SIU log lines received during this test
        link.on_log = self._on_log
        self.seq = random.randint(0, 255)
        self.session = p.SESSION_NONE
        self.last_session = p.SESSION_NONE
        self.req = 0

    def _on_log(self, line: str) -> None:
        self.log.append(line)
        print(f"SIU log> {line}")         # captured by pytest, shown when a test fails

    def enable_log(self) -> None:
        rsp = self.poll([p.config_set(self.next_req_id(), p.CFG_LOG_ENABLE, b"\x01")])
        assert results(rsp)[0][2] == 0

    def wait_log(self, text: str, polls: int = 20) -> str:
        """Polls until a log line containing text arrives; returns it."""
        for _ in range(polls):
            for line in self.log:
                if text in line:
                    return line
            self.poll()
        raise AssertionError(f"SIU never logged {text!r}; got: {self.log}")

    # ---- ids ------------------------------------------------------------------

    def next_seq(self) -> int:
        self.seq = (self.seq + 1) & 0xFF
        return self.seq

    def next_req_id(self) -> int:
        self.req = self.req % 255 + 1
        return self.req

    def new_session_id(self) -> int:
        s = self.last_session
        while s in (p.SESSION_NONE, self.last_session):
            s = random.randint(1, 255)
        return s

    # ---- exchanges ------------------------------------------------------------

    def request(self, tlvs=(), session: int | None = None, flags: int = 0, seq: int | None = None,
                timeout: float = RSP_TIMEOUT_S) -> p.Frame | None:
        """Sends one request; returns the response with the matching SEQ, or None."""
        seq = self.next_seq() if seq is None else seq
        session = self.session if session is None else session
        self.link.send(p.Frame(flags, seq, session, list(tlvs)))
        deadline = time.monotonic() + timeout
        while (rsp := self.link.recv(deadline)) is not None:
            if rsp.flags & p.FLAG_RSP and rsp.seq == seq:
                return rsp
        return None

    def expect_silence(self, timeout: float = SILENCE_S) -> None:
        rsp = self.link.recv(time.monotonic() + timeout)
        assert rsp is None, f"expected no response, got {rsp}"

    def hello(self, attempts: int = 3) -> p.Frame:
        """Like a real CPM: HELLO every 100 ms until answered (§4.2)."""
        for _ in range(attempts):
            rsp = self.request([p.hello()], session=p.SESSION_NONE)
            if rsp is not None:
                return rsp
            time.sleep(0.1)
        raise AssertionError(f"no answer to HELLO after {attempts} attempts")

    def open_session(self, poll_ms: int = 20, link_timeout_ms: int = 200) -> p.Frame:
        self.link.flush()
        self.hello()
        sid = self.new_session_id()
        rsp = self.request([p.session_start(sid, poll_ms, link_timeout_ms)], session=sid)
        assert rsp is not None, "no answer to SESSION_START"
        assert rsp.find(p.SESSION_ACK) == bytes([sid])
        self.session = self.last_session = sid
        return rsp

    def poll(self, tlvs=(), **kw) -> p.Frame:
        rsp = self.request(tlvs, **kw)
        assert rsp is not None, "no answer to poll"
        return rsp

    # ---- helpers for broken frames ----------------------------------------------

    def raw_frame(self, byte0: int, seq: int, session: int, payload: bytes) -> bytes:
        body = bytes([byte0, seq, session]) + payload
        return body + struct.pack("<H", p.crc16(body))


def errors(rsp: p.Frame) -> list[tuple[int, int]]:
    """(ref_type, code) of every ERROR TLV, in order."""
    return [(v[0], v[1]) for v in rsp.find_all(p.ERROR)]


def results(rsp: p.Frame) -> list[tuple[int, int, int, int]]:
    """(req_id, ref_type, result, detail) of every RESULT TLV."""
    return [tuple(v[:4]) for v in rsp.find_all(p.RESULT)]


@pytest.fixture(scope="session")
def link(request):
    port = request.config.getoption("--port")
    try:
        lk = Link(port)
    except Exception as e:  # noqa: BLE001
        pytest.skip(f"SIU port {port} not available: {e}")
    lk.trace = request.config.getoption("--trace-frames")
    yield lk
    lk.close()


@pytest.fixture
def siu(link):
    """An SIU in a fresh ACTIVE session, with its debug log on. Settings are restored afterwards."""
    s = Siu(link)
    s.open_session()
    s.enable_log()
    yield s
    s.open_session()
    s.poll([p.config_set(s.next_req_id(), p.CFG_LED_BRIGHTNESS, bytes([100])), p.led_set(0)])


@pytest.fixture
def fresh(link):
    """An SIU client that has not opened a session yet."""
    s = Siu(link)
    link.flush()
    return s
