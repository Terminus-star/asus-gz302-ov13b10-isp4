#!/usr/bin/env python3
"""Generate amdgpu/gz302-ov13b-script.bin (the SET_DEVICE_SCRIPT payload) from the owner's own Windows files: the
sensor script text asm_OV13B.asm, assembled by the script assembler inside amdcamera.sys.

    /usr/bin/python3 -I tools/gen_script.py AMDCAMERA.SYS ASM_OV13B.ASM OUT.bin

Runs the driver's assembler in an emulator (tools/pe_emu.py), the way the Windows driver does it before uploading the
script:
  0x14005e6c0  assembler: rcx = script text, edx = text length, r8 = output buffer (the compiled script, 0x15e8 bytes)
  imports it uses: ExAllocatePool2 (served from an emulator heap), strcmp / strcpy_s / sprintf_s / vsprintf_s
  (pe_emu.add_crt); its trace logging is skipped by marking the log list at 0x1400bc008 empty.
Payload (0x16ec bytes): u32 sensor id 0, then the compiled script; the four sensor-index fields at 0x169c, 0x16b0,
0x16c4 and 0x16d8 are 0 for sensor 0.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # tools/pe_emu.py
import pe_emu  # noqa: E402
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EDX, UC_X86_REG_R8, UC_X86_REG_RCX  # noqa: E402

ASM_FN, LOG_HEAD = 0x14005E6C0, 0x1400BC008
HEAP, TEXT, OUT = 0x50000000, 0x5F000000, 0x5F800000
COMPILED_SIZE, PAYLOAD_SIZE = 0x15E8, 0x16EC
# amdcamera.sys versions this was verified with: output byte-identical to the tested file
TESTED = {"2376fd793af49fdd1e7d51607cbcaefc1d9d823fcb645c5be7f6d2d25b054263"}
# sha256 of the first 32 bytes of the assembler, to accept other versions with the same function
FUNCTIONS = {ASM_FN: "3eb9cfcc57680df9b1ba5bb41aea8799a93ed91bb9ea1a329410fd91c60ac9c5"}


def generate(sys_path, asm_path):
    img = pe_emu.Image(sys_path)
    note = img.check(TESTED, FUNCTIONS)
    uc = pe_emu.emulator(img)
    heap = {"next": HEAP}
    uc.mem_map(HEAP, 0x1000000)

    def alloc(uc):                       # ExAllocatePool2(flags, size, tag): zeroed memory
        size, p = uc.reg_read(UC_X86_REG_EDX), heap["next"]
        heap["next"] = (p + size + 0xFFF) & ~0xFFF
        if heap["next"] > HEAP + 0x1000000:
            raise RuntimeError("emulator heap exhausted")
        return p

    pe_emu.hook_import(uc, "ExAllocatePool2", alloc)
    pe_emu.add_crt(uc)
    uc.mem_write(LOG_HEAD, struct.pack("<Q", LOG_HEAD))
    with open(asm_path, "rb") as f:
        text = f.read()
    if len(text) > 0x800000:
        raise ValueError("%s is too large for a sensor script" % asm_path)
    uc.mem_map(TEXT, 0x800000)
    uc.mem_write(TEXT, text)
    uc.mem_map(OUT, 0x10000)
    pe_emu.call(uc, ASM_FN, {UC_X86_REG_RCX: TEXT, UC_X86_REG_EDX: len(text), UC_X86_REG_R8: OUT})
    if uc.reg_read(UC_X86_REG_EAX) != 0:
        raise RuntimeError("the driver's assembler rejected %s (status 0x%x)" % (asm_path, uc.reg_read(UC_X86_REG_EAX)))
    payload = bytearray(PAYLOAD_SIZE)
    payload[4:4 + COMPILED_SIZE] = uc.mem_read(OUT, COMPILED_SIZE)
    return bytes(payload), note


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    try:
        blob, note = generate(sys.argv[1], sys.argv[2])
    except (ValueError, RuntimeError) as e:
        sys.exit("gen_script: %s" % e)
    if note:
        print("gen_script: note: " + note)
    with open(sys.argv[3], "wb") as f:
        f.write(blob)
    print("gen_script: %s: %d bytes" % (sys.argv[3], len(blob)))


if __name__ == "__main__":
    main()
