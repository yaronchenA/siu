#!/usr/bin/env python3
"""Turns the linked application binary into a firmware image (common/fw_image.h):
pads it to a multiple of 4 bytes, fills in header.image_size, and appends the CRC-32 trailer.

    python3 tools/mkimage.py build/siu_app.bin build/siu.img

The same .img file is what `make flash` writes to the app slot and what a firmware update sends.
"""
import struct
import sys
import zlib

HEADER_OFFSET = 0xC0
MAGIC = 0x31554953                  # "SIU1"
IMAGE_MAX = 26 * 1024 - 4           # FW_IMAGE_MAX (common/fw_layout.h)
HDR = struct.Struct("<IHHIBBBBI12s")   # fw_image_header_t, 32 bytes


def read_header(img: bytes) -> dict:
    magic, ver, hw, size, ma, mi, pa, _, build, _ = HDR.unpack_from(img, HEADER_OFFSET)
    return {"magic": magic, "header_ver": ver, "hw_model": hw, "image_size": size,
            "version": f"{ma}.{mi}.{pa}", "build_id": build}


def make_image(binary: bytes) -> bytes:
    body = bytearray(binary)
    while len(body) % 4:
        body.append(0xFF)
    hdr = read_header(body)
    if hdr["magic"] != MAGIC:
        raise SystemExit(f"no image header at 0x{HEADER_OFFSET:X} (magic 0x{hdr['magic']:08X})")
    struct.pack_into("<I", body, HEADER_OFFSET + 8, len(body))          # image_size
    image = bytes(body) + struct.pack("<I", zlib.crc32(body))
    if len(image) > IMAGE_MAX:
        raise SystemExit(f"image is {len(image)} bytes, the slot allows {IMAGE_MAX}")
    return image


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    image = make_image(open(sys.argv[1], "rb").read())
    open(sys.argv[2], "wb").write(image)
    h = read_header(image)
    print(f"{sys.argv[2]}: firmware {h['version']} build {h['build_id']:08x}, hw 0x{h['hw_model']:04X}, "
          f"{len(image)} bytes ({len(image) * 100 // IMAGE_MAX}% of slot), CRC-32 0x{zlib.crc32(image[:-4]):08X}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
