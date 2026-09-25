"""Hardware-in-the-loop: firmware update (cpm_siu_protocol.md §8.6) through the real bootloader.

Needs build/siu.img to be the firmware currently flashed (run `make flash` first). The install tests
send a variant of it (one reserved header byte changed, CRC recomputed): same code, but a different
image, so the bootloader really erases and reprograms the app slot.
"""
from __future__ import annotations

import os
import struct
import sys
import time
import zlib

import pytest

from conftest import p, results

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools"))
import fw_update as fu  # noqa: E402

IMAGE_PATH = os.path.join(os.path.dirname(__file__), "..", "..", "build", "siu.img")


@pytest.fixture(scope="module")
def image() -> bytes:
    if not os.path.exists(IMAGE_PATH):
        pytest.skip("build/siu.img missing — run make")
    return open(IMAGE_PATH, "rb").read()


def variant(image: bytes, marker: int, hw_model: int | None = None) -> bytes:
    """Same code, different image: patches a reserved header word (and optionally hw_model)."""
    img = bytearray(image)
    size = struct.unpack_from("<I", img, 0xC0 + 8)[0]
    struct.pack_into("<I", img, 0xC0 + 20, marker)
    if hw_model is not None:
        struct.pack_into("<H", img, 0xC0 + 6, hw_model)
    struct.pack_into("<I", img, size, zlib.crc32(img[:size]))
    return bytes(img)


def identity(rsp: p.Frame) -> dict:
    _, _, boot_id, reset, caps, _ = struct.unpack("<BBIBIB", rsp.find(p.HELLO_INFO))
    ma, mi, pa, build, boot = struct.unpack("<BBBIB", rsp.find(p.FW_INFO))
    return {"boot_id": boot_id, "reset": reset, "caps": caps, "version": (ma, mi, pa), "bootloader": boot}


def begin(siu, img: bytes, crc: int | None = None):
    hdr = p.image_header(img)
    crc = zlib.crc32(img[:-4]) if crc is None else crc
    return fu._action(siu, p.fw_begin, len(img), crc, hdr["version"], tlv=p.FW_BEGIN)


def send_chunks(siu, img: bytes, upto: int | None = None) -> None:
    upto = len(img) if upto is None else upto
    off = 0
    while off < upto:
        data = img[off:off + p.FW_CHUNK_MAX]
        result, detail, _ = fu._action(siu, p.fw_chunk, off, data, tlv=p.FW_CHUNK)
        assert result == p.RESULT_OK, f"chunk at {off}: {p.RESULT_NAMES[result]} {detail}"
        off += len(data)


def fw_status(siu) -> p.FwStatus | None:
    v = siu.poll().find(p.FW_STATUS)
    return p.FwStatus.parse(v) if v is not None else None


# ---- successful updates ------------------------------------------------------------------------

def test_capability_advertised(fresh):
    caps = identity(fresh.hello())["caps"]
    assert caps & (1 << 9), "HELLO_INFO capability bit9 (firmware update) must be set"
    assert identity(fresh.hello_rsp)["bootloader"] >= 1


def test_update_installs_new_image(siu, image):
    before = identity(siu.hello_rsp)
    t = fu.update(siu, variant(image, 0xA1), say=lambda *_: None)
    after = identity(t["identity"])
    print(f"\n  erase {t['erase_s']:.2f} s, transfer {t['transfer_s']:.2f} s, install+boot {t['install_s']:.2f} s")
    assert after["reset"] == 4, "reset reason must be fw-update after an install"
    assert after["boot_id"] != before["boot_id"]
    assert after["version"] == before["version"] and after["bootloader"] == before["bootloader"]
    assert t["total_s"] < 15


def test_same_image_again_does_not_reinstall(siu, image):
    img = variant(image, 0xA1)                           # what the previous test installed
    t = fu.update(siu, img, say=lambda *_: None)
    assert identity(t["identity"])["reset"] == 3         # plain software reset: nothing to install


def test_back_to_the_original_image(siu, image):
    t = fu.update(siu, image, say=lambda *_: None)
    assert identity(t["identity"])["reset"] == 4


# ---- rejections and failures --------------------------------------------------------------------

def test_wrong_crc_is_caught_and_not_activated(siu, image):
    img = variant(image, 0xB2)
    result, _, _ = begin(siu, img, crc=zlib.crc32(img[:-4]) ^ 0xDEAD)
    assert result == p.RESULT_IN_PROGRESS
    fu._wait_state(siu, p.FWU_RECEIVING, 5, print)
    send_chunks(siu, img)
    fu._action(siu, lambda r: p.fw_end(r), tlv=p.FW_END)
    with pytest.raises(fu.UpdateError, match="CRC"):
        fu._wait_state(siu, p.FWU_VERIFIED, 3, print)
    result, detail, _ = fu._action(siu, lambda r: p.fw_activate(r), tlv=p.FW_ACTIVATE)
    assert (result, detail) == (p.RESULT_REJECTED, 2)    # wrong state: nothing verified
    siu.poll()                                           # still running, no reset


def test_image_for_other_hardware_rejected(siu, image):
    img = variant(image, 0xB3, hw_model=0xB0FF)
    begin(siu, img)
    fu._wait_state(siu, p.FWU_RECEIVING, 5, print)
    send_chunks(siu, img)
    fu._action(siu, lambda r: p.fw_end(r), tlv=p.FW_END)
    with pytest.raises(fu.UpdateError, match="wrong hardware model"):
        fu._wait_state(siu, p.FWU_VERIFIED, 3, print)


def test_chunk_out_of_order_and_resync(siu, image):
    begin(siu, image)
    fu._wait_state(siu, p.FWU_RECEIVING, 5, print)
    send_chunks(siu, image, upto=400)
    result, detail, rsp = fu._action(siu, p.fw_chunk, 1000, image[1000:1200], tlv=p.FW_CHUNK)
    assert (result, detail) == (p.RESULT_REJECTED, 1)
    assert p.FwStatus.parse(rsp.find(p.FW_STATUS)).next_offset == 400   # where to continue


def test_end_before_all_data_rejected(siu, image):
    begin(siu, image)
    fu._wait_state(siu, p.FWU_RECEIVING, 5, print)
    send_chunks(siu, image, upto=200)
    result, detail, _ = fu._action(siu, lambda r: p.fw_end(r), tlv=p.FW_END)
    assert (result, detail) == (p.RESULT_REJECTED, 1)


def test_begin_rejected_while_charging(siu, image):
    siu.poll([p.cp_set(p.CP_MODE_PWM, 267)])
    result, detail, _ = begin(siu, image)
    assert (result, detail) == (p.RESULT_REJECTED, 7)
    siu.poll([p.cp_set(p.CP_MODE_STATE_F)])


@pytest.mark.parametrize("size", [100, 26 * 1024, 11027])
def test_begin_rejects_bad_sizes(siu, image, size):
    req = siu.next_req_id()
    rsp = siu.poll([p.fw_begin(req, size, 0, (0, 5, 0))])
    assert results(rsp) == [(req, p.FW_BEGIN, p.RESULT_REJECTED, 6)]


def test_link_loss_aborts_transfer(siu, image):
    begin(siu, image)
    fu._wait_state(siu, p.FWU_RECEIVING, 5, print)
    send_chunks(siu, image, upto=600)
    time.sleep(0.3)                                      # SIU link timeout
    siu.open_session()
    assert fw_status(siu) is None                        # idle again: FW_STATUS no longer sent
    result, detail, _ = fu._action(siu, p.fw_chunk, 600, image[600:800], tlv=p.FW_CHUNK)
    assert (result, detail) == (p.RESULT_REJECTED, 2)


def test_fw_status_while_updating(siu, image):
    assert fw_status(siu) is None                        # idle: not in responses
    begin(siu, image)
    fu._wait_state(siu, p.FWU_RECEIVING, 5, print)
    send_chunks(siu, image, upto=1000)
    # next_offset counts bytes written to flash; the last accepted chunk is written a few ms later.
    offsets = [fw_status(siu).next_offset for _ in range(5)]
    assert offsets[0] in (800, 1000) and offsets[-1] == 1000
    st = fw_status(siu)
    assert st.state == p.FWU_RECEIVING and st.error == 0
