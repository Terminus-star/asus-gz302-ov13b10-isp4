#!/usr/bin/env python3
"""Generate amdgpu/gz302-ov13b-phy-seq.bin (the MIPI D-PHY receiver start sequence) from the owner's own amdcamera.sys.

    /usr/bin/python3 -I tools/gen_phy_seq.py AMDCAMERA.SYS OUT.bin

Runs the driver's PHY analog-configuration function in an emulator (tools/pe_emu.py) for PHY 0, 1120 Mbps, 4 lanes,
the configuration of the GZ302 rear camera, and records the register operations it would perform, instead of
performing them (findings 41, 153). Intercepted:
  0x14005131c  read-modify-write of one register field (ecx = phy, rdx -> descriptor: register u16 @+8, mask @+0x18,
               value @+0x1a)                  -> recorded as kind 0, with the descriptor's mask and value
  0x1400014b8  16-bit register write (rcx = address, edx = value); address +0x24 selects the register through the
               PHY's indirect port, +0x28 writes it  -> recorded as kind 1, mask 0xFFFF
  0x140001320  register read                  -> returns 0
Output (read by the capture driver, isp4_interface.c): u32 'GZPH' (0x48505a47), u32 version 1, u32 phy, u32 rate,
u32 lanes, u32 count, then count x {u16 reg, u16 mask, u16 value, u16 kind}, little-endian.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # tools/pe_emu.py
import pe_emu  # noqa: E402
from unicorn.x86_const import UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_R8D, UC_X86_REG_RCX, UC_X86_REG_RDX, \
    UC_X86_REG_XMM1  # noqa: E402

PHY, RATE_MBPS, LANES = 0, 1120, 4
PHY_FN, RMW_FN, WRITE16_FN, READ_FN = 0x14005356C, 0x14005131C, 0x1400014B8, 0x140001320
# amdcamera.sys versions this was verified with: output byte-identical to the tested file
TESTED = {"2376fd793af49fdd1e7d51607cbcaefc1d9d823fcb645c5be7f6d2d25b054263"}
# sha256 of the first 32 bytes of each function used, to accept other versions with the same functions
FUNCTIONS = {
    PHY_FN: "adb3feaf56b8615891c80bf360f0cf205f02f320ad4d8551edc86eb51b3624e4",
    RMW_FN: "286296705d234ff9a254bdbf49abc3e25d72af2f7cf0e492f731e81eb910c706",
    WRITE16_FN: "fe067a413966617eb0f385e66a9b492a538d8b138b7a06052e077e6122315429",
    READ_FN: "2bb62b06ba96ab24fb499af9f694e1454c08984fa70a7b2cc89fc8764b71a7f8",
}


def generate(path):
    img = pe_emu.Image(path)
    note = img.check(TESTED, FUNCTIONS)
    uc = pe_emu.emulator(img)
    ops, port = [], {"reg": None}

    def rmw(uc):
        desc = uc.reg_read(UC_X86_REG_RDX)
        ops.append(tuple(struct.unpack("<H", uc.mem_read(desc + o, 2))[0] for o in (8, 0x18, 0x1A)) + (0,))

    def write16(uc):
        addr, value = uc.reg_read(UC_X86_REG_RCX), uc.reg_read(UC_X86_REG_EDX) & 0xFFFF
        if addr & 0xFF == 0x24:
            port["reg"] = value
        elif addr & 0xFF == 0x28 and port["reg"] is not None:
            ops.append((port["reg"], 0xFFFF, value, 1))
        else:
            raise RuntimeError("unexpected PHY register write to 0x%x" % addr)

    pe_emu.hook(uc, RMW_FN, rmw)
    pe_emu.hook(uc, WRITE16_FN, write16)
    pe_emu.hook(uc, READ_FN, lambda uc: 0)
    rate_f32 = struct.unpack("<I", struct.pack("<f", float(RATE_MBPS)))[0]
    pe_emu.call(uc, PHY_FN, {UC_X86_REG_ECX: PHY, UC_X86_REG_XMM1: rate_f32, UC_X86_REG_R8D: LANES})
    blob = struct.pack("<6I", 0x48505A47, 1, PHY, RATE_MBPS, LANES, len(ops))
    return blob + b"".join(struct.pack("<4H", *op) for op in ops), len(ops), note


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    try:
        blob, n, note = generate(sys.argv[1])
    except (ValueError, RuntimeError) as e:
        sys.exit("gen_phy_seq: %s" % e)
    if note:
        print("gen_phy_seq: note: " + note)
    if n != 296:
        print("gen_phy_seq: note: %d operations (the tested driver gives 296)" % n)
    with open(sys.argv[2], "wb") as f:
        f.write(blob)
    print("gen_phy_seq: %s: %d operations, %d bytes" % (sys.argv[2], n, len(blob)))


if __name__ == "__main__":
    main()
