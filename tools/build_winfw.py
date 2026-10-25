#!/usr/bin/env python3
"""Build test ISP firmware images for Linux (isp_4_1_1.bin format) from the Windows .ram files.

The Linux image is

    0x100 bytes  AMD common firmware header (struct common_firmware_header, rest zero padded)
    ucode        16-byte prefix + "$PS1" PSP container      (ucode_size_bytes long)

and the Windows "srx_*_isp_firmware_signed.ram" files are exactly such a ucode part.  So a test image is
the stock Linux header with size_bytes, ucode_size_bytes and crc32 recomputed, followed by the Windows
file.  Nothing is signed or modified: the ucode bytes are copied unchanged.

Header (little endian), offsets: 0x00 size_bytes, 0x04 header_size_bytes (0x20), 0x08 header_version
major/minor (u16 each), 0x0c ip_version major/minor (u16 each), 0x10 ucode_version, 0x14 ucode_size_bytes,
0x18 ucode_array_offset_bytes (0x100), 0x1c crc32 of the payload.

The tool works read-only on its inputs and writes only into --out-dir (must not exist yet or be empty).
Do not redistribute the output: it contains the Windows firmware.

Usage (pass paths as arguments; run isolated):
    python3 -I tools/build_winfw.py --linux-fw FILE --windows-dir DIR --out-dir NEWDIR [--tag srx_h]
    python3 -I tools/build_winfw.py --linux-fw FILE --check        # only verify the crc32 rule on the stock image
"""

import argparse
import hashlib
import os
import struct
import sys
import zlib

HDR_FMT = "<IIHHHHIIII"        # 8 fields up to and including crc32 = 0x20 bytes
HDR_SIZE = struct.calcsize(HDR_FMT)
FIELDS = ("size_bytes", "header_size_bytes", "hv_major", "hv_minor", "ip_major", "ip_minor",
          "ucode_version", "ucode_size_bytes", "ucode_array_offset_bytes", "crc32")


def read_linux_fw(path):
    """Return the raw stock image, decompressing .zst through the zstandard module or the zstd tool."""
    if not path.endswith(".zst"):
        with open(path, "rb") as f:
            return f.read()
    import subprocess
    return subprocess.run(["zstd", "-dc", path], check=True, capture_output=True).stdout


def parse_header(img):
    vals = struct.unpack_from(HDR_FMT, img, 0)
    return dict(zip(FIELDS, vals))


def crc_candidates(payload):
    """Possible definitions of the 'crc32 checksum of the payload'."""
    return {
        "zlib.crc32(payload)": zlib.crc32(payload) & 0xFFFFFFFF,
        "~zlib.crc32(payload)": (~zlib.crc32(payload)) & 0xFFFFFFFF,
    }


def find_rule(img, hdr):
    payload = img[hdr["ucode_array_offset_bytes"]:hdr["ucode_array_offset_bytes"] + hdr["ucode_size_bytes"]]
    for name, val in crc_candidates(payload).items():
        if val == hdr["crc32"]:
            return name
    return None


def build(stock, rule, ram):
    hdr = parse_header(stock)
    off = hdr["ucode_array_offset_bytes"]
    new = dict(hdr)
    new["ucode_size_bytes"] = len(ram)
    new["size_bytes"] = off + len(ram)
    crcs = crc_candidates(ram)
    new["crc32"] = crcs[rule] if rule else hdr["crc32"]
    head = bytearray(stock[:off])                 # keep the zero padding of the stock header
    struct.pack_into(HDR_FMT, head, 0, *[new[k] for k in FIELDS])
    return bytes(head) + ram, new


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--linux-fw", required=True, help="stock isp_4_1_1.bin (or .zst)")
    ap.add_argument("--windows-dir", help="directory with the srx_*_isp_firmware_signed.ram files")
    ap.add_argument("--out-dir", help="new directory for the test images")
    ap.add_argument("--check", action="store_true", help="only verify the header rules on the stock image")
    ap.add_argument("--tag", action="append", choices=("srx", "srx_h"),
                    help="build only this image (repeatable; default: both). The GZ302 uses srx_h.")
    a = ap.parse_args()

    stock = read_linux_fw(a.linux_fw)
    hdr = parse_header(stock)
    print("stock image: %d bytes" % len(stock))
    for k in FIELDS:
        print("  %-26s 0x%08x (%d)" % (k, hdr[k], hdr[k]))
    ok = (hdr["size_bytes"] == len(stock) and
          hdr["ucode_array_offset_bytes"] + hdr["ucode_size_bytes"] == len(stock))
    print("size fields consistent with the file: %s" % ok)
    if not ok:
        return 1
    rule = find_rule(stock, hdr)
    print("crc32 rule reproduced on the stock image: %s" % (rule or "NO (crc left as in the stock header)"))
    if a.check or not (a.windows_dir and a.out_dir):
        return 0

    os.makedirs(a.out_dir, exist_ok=False)        # refuse to reuse an existing directory
    lines = []
    for tag in (a.tag or ("srx", "srx_h")):
        src = os.path.join(a.windows_dir, "%s_isp_firmware_signed.ram" % tag)
        with open(src, "rb") as f:
            ram = f.read()
        img, new = build(stock, rule, ram)
        # self-check: the produced header must parse back and the sizes must add up
        chk = parse_header(img)
        assert chk["size_bytes"] == len(img) and chk["ucode_array_offset_bytes"] + chk["ucode_size_bytes"] == len(img)
        assert img[chk["ucode_array_offset_bytes"]:] == ram
        out = os.path.join(a.out_dir, "isp_4_1_1_%s.bin" % tag)
        with open(out, "wb") as f:
            f.write(img)
        sha = hashlib.sha256(img).hexdigest()
        print("wrote %s: %d bytes, size_bytes 0x%x, ucode_size 0x%x, crc32 0x%08x" % (
            out, len(img), new["size_bytes"], new["ucode_size_bytes"], new["crc32"]))
        lines.append("%s  %s" % (sha, os.path.basename(out)))
    with open(os.path.join(a.out_dir, "SHA256SUMS"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("wrote SHA256SUMS; ucode_version in both headers stays 0x%08x (copied from the stock header)" % hdr["ucode_version"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
