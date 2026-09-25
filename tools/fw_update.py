#!/usr/bin/env python3
"""Firmware update of an SIU over the CPM<->SIU protocol (cpm_siu_protocol.md §8.6), from the PC.

    .venv/bin/python tools/fw_update.py build/siu.img [--port /dev/cu.usbserial-0001]

Steps: handshake -> FW_BEGIN (the SIU erases its staging slot, ~1 s of silence) -> FW_CHUNKs ->
FW_END (the SIU checks CRC + header) -> FW_ACTIVATE (the SIU resets; its bootloader installs the
image) -> handshake again and confirm the SIU is running.

The same code does the job the CPM will do on the product (it relays images from the CCU).
"""
from __future__ import annotations

import argparse
import struct
import sys
import time
import zlib

import siu_proto as p
from siu_client import SiuClient, results
from siu_link import DEFAULT_PORT, Link


class UpdateError(Exception):
    pass


def _result_for(rsp: p.Frame | None, req_id: int, tlv: int) -> tuple[int, int] | None:
    if rsp is None:
        return None
    for r_req, r_type, result, detail in results(rsp):
        if r_req == req_id and r_type == tlv:
            return result, detail
    return None


def _status(rsp: p.Frame | None) -> p.FwStatus | None:
    v = rsp.find(p.FW_STATUS) if rsp is not None else None
    return p.FwStatus.parse(v) if v is not None else None


def _wait_state(siu: SiuClient, wanted: int, timeout_s: float, say) -> p.FwStatus:
    """Polls until FW_STATUS reaches `wanted` (tolerating silence, e.g. during the flash erase)."""
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        st = _status(siu.request([], timeout=0.04))    # keep polling often: the SIU's link timeout is 200 ms
        if st is not None:
            if st.state == wanted:
                return st
            if st.state == p.FWU_ERROR:
                raise UpdateError(f"SIU reports: {st}")
        time.sleep(0.01)
    raise UpdateError(f"timed out waiting for state '{p.FWU_STATES[wanted]}'")


def _action(siu: SiuClient, tlv_fn, *args, tlv: int, attempts: int = 5) -> tuple[int, int, p.Frame]:
    """Sends an action with one REQ_ID, retrying lost responses and BUSY. Returns (result, detail, rsp)."""
    req = siu.next_req_id()
    for _ in range(attempts):
        rsp = siu.request([tlv_fn(req, *args)], timeout=0.1)
        res = _result_for(rsp, req, tlv)
        if res is None:
            continue                                   # lost: same REQ_ID again, it won't run twice
        if res[0] == p.RESULT_BUSY:
            time.sleep(0.01)
            continue
        return res[0], res[1], rsp
    raise UpdateError(f"no usable answer to {p.tlv_name(tlv)}")


def update(siu: SiuClient, image: bytes, say=print, activate: bool = True, reconnect: bool = True) -> dict:
    """Runs a complete update. Returns timings and the SIU's identity afterwards."""
    hdr = p.image_header(image)
    size, crc = len(image), zlib.crc32(image[:-4])     # the image's own CRC (its trailer)
    t0 = time.monotonic()
    say(f"image: v{'.'.join(map(str, hdr['version']))} build {hdr['build_id']:08x}, {size} bytes, CRC-32 0x{crc:08X}")

    result, detail, _ = _action(siu, p.fw_begin, size, crc, hdr["version"], tlv=p.FW_BEGIN)
    if result != p.RESULT_IN_PROGRESS:
        raise UpdateError(f"FW_BEGIN rejected: {p.FWU_REJECT.get(detail, detail)}")
    say("erasing the staging slot ...")
    _wait_state(siu, p.FWU_RECEIVING, 5.0, say)
    t_erase = time.monotonic() - t0

    offset, last_pct = 0, -1
    while offset < size:
        data = image[offset:offset + p.FW_CHUNK_MAX]
        result, detail, rsp = _action(siu, p.fw_chunk, offset, data, tlv=p.FW_CHUNK)
        if result == p.RESULT_OK:
            offset += len(data)
        elif result == p.RESULT_REJECTED and detail == 1 and (st := _status(rsp)) is not None:
            offset = st.next_offset                    # resync to where the SIU is
        else:
            raise UpdateError(f"FW_CHUNK at {offset} rejected: {p.FWU_REJECT.get(detail, detail)}")
        pct = offset * 100 // size
        if pct // 10 != last_pct // 10:
            say(f"  {pct:3d} %  ({offset}/{size} bytes)")
            last_pct = pct
    t_transfer = time.monotonic() - t0 - t_erase

    result, detail, _ = _action(siu, lambda r: p.fw_end(r), tlv=p.FW_END)
    if result not in (p.RESULT_IN_PROGRESS, p.RESULT_OK):
        raise UpdateError(f"FW_END rejected: {p.FWU_REJECT.get(detail, detail)}")
    _wait_state(siu, p.FWU_VERIFIED, 3.0, say)
    say("verified by the SIU (CRC-32 + header)")
    out = {"erase_s": t_erase, "transfer_s": t_transfer}
    if not activate:
        return out

    result, detail, _ = _action(siu, lambda r: p.fw_activate(r), tlv=p.FW_ACTIVATE)
    if result != p.RESULT_OK:
        raise UpdateError(f"FW_ACTIVATE rejected: {p.FWU_REJECT.get(detail, detail)}")
    say("activated — the SIU resets and its bootloader installs the image ...")
    t_act = time.monotonic()
    if not reconnect:
        return out

    time.sleep(0.5)
    siu.open_session(hello_attempts=60)                # up to ~10 s for erase + copy + boot
    out["install_s"] = time.monotonic() - t_act
    out["total_s"] = time.monotonic() - t0
    out["identity"] = siu.hello_rsp
    say(f"SIU is back after {out['install_s']:.1f} s:")
    say("  " + p.describe_identity(siu.hello_rsp).replace("\n", "\n  "))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", help="firmware image, e.g. build/siu.img")
    ap.add_argument("--port", default=DEFAULT_PORT)
    ap.add_argument("--no-activate", action="store_true", help="transfer and verify only")
    ap.add_argument("--log", action="store_true", help="show the SIU's debug log")
    args = ap.parse_args()

    image = open(args.image, "rb").read()
    link = Link(args.port)
    siu = SiuClient(link)
    if not args.log:
        link.on_log = None
    try:
        siu.open_session()
        if args.log:
            siu.enable_log()
        t = update(siu, image, activate=not args.no_activate)
        print(f"done: erase {t['erase_s']:.1f} s, transfer {t['transfer_s']:.1f} s"
              + (f", install + reboot {t['install_s']:.1f} s, total {t['total_s']:.1f} s" if "total_s" in t else ""))
        return 0
    except (UpdateError, AssertionError) as e:
        print(f"UPDATE FAILED: {e}", file=sys.stderr)
        return 1
    finally:
        link.close()


if __name__ == "__main__":
    sys.exit(main())
