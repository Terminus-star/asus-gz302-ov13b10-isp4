#!/usr/bin/env python3
"""Generate amdgpu/gz302-ov13b-sensor-prop.bin (the SET_SENSOR_PROP payload) from the owner's own sensorOV13B.sys.

    /usr/bin/python3 -I tools/gen_sensor_prop.py SENSOROV13B.SYS OUT.bin

Runs the sensor plugin's caps function in an emulator (tools/pe_emu.py) for the GZ302 module (variant "3001",
sensor mode 0 = 4208x3120 at 30 fps, bus type 4) and packs its result the way the ISP driver sends it
(findings 7, 14, 153):
  0x140002d70  caps function: rcx = sensor context (variant string @+0x1a4, mode @+0x410, bus type @+0x464),
               rdx = caps buffer, r8d = mode
  the module variant is also read as an index from a global (0x14000a1bc) that the driver's variant detection sets by
  looking the name up in its table at 0x140008470 (2 entries, stride 0x44, name at the start): done the same way here.
Payload (0x168 bytes): u32 sensor id 0, u32 0, then the first 0x160 bytes of the caps buffer with its first u32
(version) set to 3.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # tools/pe_emu.py
import pe_emu  # noqa: E402
from unicorn.x86_const import UC_X86_REG_R8D, UC_X86_REG_RCX, UC_X86_REG_RDX  # noqa: E402

VARIANT, MODE, BUS_TYPE = b"3001", 0, 4
CAPS_FN, VARIANT_DETECT = 0x140002D70, 0x14000494B
VARIANT_TABLE, VARIANT_COUNT, VARIANT_STRIDE, VARIANT_INDEX = 0x140008470, 2, 0x44, 0x14000A1BC
CTX, CAPS = 0x60000000, 0x60010000
PAYLOAD_SIZE, CAPS_COPIED = 0x168, 0x160
# sensorOV13B.sys versions this was verified with: output byte-identical to the tested file
TESTED = {"53403d7def9f06dac4fbb10c212904b8c5965ded3dbc26879f02456511c8f5ef"}
# sha256 of the first 32 bytes of the code used (the caps function, the variant lookup that sets the index)
FUNCTIONS = {
    CAPS_FN: "6a505235efa996fd73e46d3cb4de689cac1c46c5d13620d5b2e78ebe89bc78fe",
    VARIANT_DETECT: "ac3033de106fe1507bae81c7bfac1c8dd121fabf2b6eef4a5d86bfd42546a944",
}


def generate(path):
    img = pe_emu.Image(path)
    note = img.check(TESTED, FUNCTIONS)
    uc = pe_emu.emulator(img)
    uc.mem_map(CTX, 0x20000)                 # sensor context (0x10000) + caps buffer
    uc.mem_write(CTX + 0x1A4, VARIANT + b"\0")
    uc.mem_write(CTX + 0x410, struct.pack("<i", MODE))
    uc.mem_write(CTX + 0x464, struct.pack("<I", BUS_TYPE))
    for i in range(VARIANT_COUNT):
        if bytes(uc.mem_read(VARIANT_TABLE + i * VARIANT_STRIDE, 16)).split(b"\0")[0] == VARIANT:
            uc.mem_write(VARIANT_INDEX, struct.pack("<I", i))
            break
    else:
        raise ValueError("variant %s is not in the driver's variant table" % VARIANT.decode())
    pe_emu.call(uc, CAPS_FN, {UC_X86_REG_RCX: CTX, UC_X86_REG_RDX: CAPS, UC_X86_REG_R8D: MODE})
    payload = bytearray(PAYLOAD_SIZE)
    payload[8:8 + CAPS_COPIED] = uc.mem_read(CAPS, CAPS_COPIED)
    struct.pack_into("<I", payload, 8, 3)
    return bytes(payload), note


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    try:
        blob, note = generate(sys.argv[1])
    except (ValueError, RuntimeError) as e:
        sys.exit("gen_sensor_prop: %s" % e)
    if note:
        print("gen_sensor_prop: note: " + note)
    with open(sys.argv[2], "wb") as f:
        f.write(blob)
    print("gen_sensor_prop: %s: %d bytes" % (sys.argv[2], len(blob)))


if __name__ == "__main__":
    main()
