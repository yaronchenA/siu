"""Hardware-in-the-loop test fixtures: a real SIU on a USB-serial adapter.

    make hil                                   # or:
    .venv/bin/python -m pytest tests/hil -v --port /dev/cu.usbserial-0001

All tests are skipped if the port can't be opened.
"""
from __future__ import annotations

import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools"))
import siu_proto as p  # noqa: E402
from siu_link import DEFAULT_PORT, Link  # noqa: E402

from siu_client import SiuClient, errors, results  # noqa: E402,F401


def pytest_addoption(parser):
    parser.addoption("--port", default=os.environ.get("SIU_PORT", DEFAULT_PORT), help="SIU link serial port")
    parser.addoption("--trace-frames", action="store_true", help="print every frame, TLV by TLV")


Siu = SiuClient


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
