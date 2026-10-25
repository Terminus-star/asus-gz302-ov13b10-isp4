"""Run one function of a Windows x64 driver file (.sys) in the Unicorn CPU emulator.

Used by tools/gen_script.py, tools/gen_phy_seq.py and tools/gen_sensor_prop.py to produce the three camera data files
on the owner's machine from the owner's own Windows driver files, so the repository ships neither the data nor a port
of the driver's logic. Nothing here touches hardware: the image is mapped into the emulator's memory, its kernel imports are
replaced by stubs that return 0, and the caller intercepts the helper functions that would access hardware.

Needs python-unicorn (Arch: pacman -S python-unicorn), for the system Python (/usr/bin/python3).
"""
import hashlib
import struct

from unicorn import UC_ARCH_X86, UC_HOOK_CODE, UC_HOOK_MEM_UNMAPPED, UC_MODE_64, Uc
from unicorn.x86_const import UC_X86_REG_RAX, UC_X86_REG_RIP, UC_X86_REG_RSP

STUBS, STACK, SENTINEL = 0x70000000, 0x7F000000, 0x7FFF0000


class Image:
    """A PE32+ driver file: its bytes, preferred base and sections (RVA, size, file offset, raw size)."""

    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        if d[pe:pe + 4] != b"PE\0\0":
            raise ValueError("%s is not a PE file" % path)
        self.opt = pe + 24
        if struct.unpack_from("<H", d, self.opt)[0] != 0x20B:
            raise ValueError("%s is not a 64-bit (PE32+) file" % path)
        nsec, optsize = struct.unpack_from("<H", d, pe + 6)[0], struct.unpack_from("<H", d, pe + 20)[0]
        self.base = struct.unpack_from("<Q", d, self.opt + 24)[0]
        self.size_image, self.size_headers = struct.unpack_from("<II", d, self.opt + 56)
        sec = self.opt + optsize
        self.sections = [struct.unpack_from("<8sIIII", d, sec + 40 * i)[1:] for i in range(nsec)]
        self.sha256 = hashlib.sha256(d).hexdigest()

    def file_bytes(self, va, n):
        rva = va - self.base
        for vsize, sva, rawsize, rawptr in self.sections:
            if sva <= rva < sva + rawsize:
                return self.data[rawptr + rva - sva:rawptr + rva - sva + n]
        raise ValueError("address 0x%x is not in the file" % va)

    def check(self, tested_sha256, functions):
        """Accept a tested file (by sha256); for any other version require each function's first 32 bytes to hash as
        expected ({va: sha256}). Returns a note for an untested version; raises if a function does not match."""
        if self.sha256 in tested_sha256:
            return None
        bad = [va for va, h in functions.items() if hashlib.sha256(self.file_bytes(va, 32)).hexdigest() != h]
        if bad:
            raise ValueError("unsupported driver version (sha256 %s): functions at %s differ from the tested "
                             "version" % (self.sha256, ", ".join("0x%x" % va for va in bad)))
        return "untested driver version (sha256 %s); the functions used match the tested one" % self.sha256


def emulator(img):
    """A Unicorn instance with the image mapped at its preferred base, imports stubbed, a stack and a return sentinel.
    uc.import_stubs maps each imported function name to its stub address (see hook_import)."""
    uc = Uc(UC_ARCH_X86, UC_MODE_64)
    uc.mem_map(img.base, (img.size_image + 0xFFF) & ~0xFFF)
    uc.mem_write(img.base, img.data[:img.size_headers])
    for vsize, va, rawsize, rawptr in img.sections:
        uc.mem_write(img.base + va, img.data[rawptr:rawptr + min(rawsize, vsize or rawsize)])
    # imports (data directory 1): every slot of every imported module points at "xor eax,eax; ret"
    imp_rva = struct.unpack_from("<I", img.data, img.opt + 112 + 8)[0]
    uc.mem_map(STUBS, 0x100000)
    uc.import_stubs = {}
    n, desc = 0, img.base + imp_rva
    while True:
        orig_thunk, _, _, name, first_thunk = struct.unpack("<5I", uc.mem_read(desc, 20))
        if name == 0:
            break
        slot, hint = img.base + first_thunk, img.base + (orig_thunk or first_thunk)
        while struct.unpack("<Q", uc.mem_read(slot, 8))[0]:
            entry = struct.unpack("<Q", uc.mem_read(hint, 8))[0]
            if not entry >> 63:          # imported by name: hint/name entry = u16 hint + name
                uc.import_stubs[read_cstr(uc, img.base + (entry & 0x7FFFFFFF) + 2).decode()] = STUBS + 16 * n
            uc.mem_write(STUBS + 16 * n, b"\x31\xc0\xc3")
            uc.mem_write(slot, struct.pack("<Q", STUBS + 16 * n))
            slot, hint, n = slot + 8, hint + 8, n + 1
        desc += 20

    def unmapped(uc, access, addr, size, value, _):
        raise RuntimeError("emulated driver code accessed unmapped memory 0x%x at 0x%x"
                           % (addr, uc.reg_read(UC_X86_REG_RIP)))

    uc.hook_add(UC_HOOK_MEM_UNMAPPED, unmapped)
    uc.mem_map(STACK, 0x100000)
    uc.mem_map(SENTINEL, 0x1000)
    return uc


def read_cstr(uc, va, limit=4096):
    """The NUL-terminated byte string at va (without the NUL)."""
    out = bytearray()
    while len(out) < limit:
        chunk = bytes(uc.mem_read(va + len(out), 64))
        end = chunk.find(b"\0")
        if end >= 0:
            return bytes(out + chunk[:end])
        out += chunk
    raise RuntimeError("unterminated string at 0x%x" % va)


def hook_import(uc, name, fn):
    """Replace the stub of the imported function `name` by fn (see hook); no-op if the image does not import it."""
    if name in uc.import_stubs:
        hook(uc, uc.import_stubs[name], fn)


def _format(uc, fmt, args):
    """Minimal printf: %s %d %i %u %x %X %c %% with optional flags/width/length; args is a callable giving the next
    64-bit argument."""
    out, i = bytearray(), 0
    while i < len(fmt):
        c = fmt[i:i + 1]
        if c != b"%":
            out += c
            i += 1
            continue
        j = i + 1
        while j < len(fmt) and fmt[j:j + 1] in b"-+ #0123456789.lhzI":
            j += 1
        spec, conv = fmt[i + 1:j], fmt[j:j + 1]
        width = int(spec.lstrip(b"-+ #0").split(b".")[0] or 0) if spec.lstrip(b"-+ #0")[:1].isdigit() else 0
        if conv == b"%":
            s = b"%"
        elif conv == b"s":
            s = read_cstr(uc, args())
        elif conv == b"c":
            s = bytes([args() & 0xFF])
        elif conv in (b"d", b"i"):
            v = args() & 0xFFFFFFFF
            s = str(v - (1 << 32) if v >> 31 else v).encode()
        elif conv == b"u":
            s = str(args() & 0xFFFFFFFF).encode()
        elif conv in (b"x", b"X"):
            s = ("%x" % (args() & 0xFFFFFFFF)).encode()
            s = s.upper() if conv == b"X" else s
        else:
            raise RuntimeError("unsupported printf conversion %r" % conv)
        pad = b"0" if spec.lstrip(b"-+ #")[:1] == b"0" and conv not in (b"s", b"c") else b" "
        out += s.rjust(width, pad) if not spec.startswith(b"-") else s.ljust(width)
        i = j + 1
    return bytes(out)


def add_crt(uc):
    """Real implementations of the kernel C-library imports a driver's text parsers use (strcmp, strcpy_s, sprintf_s,
    vsprintf_s); the default stubs would return 0 (e.g. every strcmp "equal")."""
    from unicorn.x86_const import UC_X86_REG_R8, UC_X86_REG_R9, UC_X86_REG_RCX, UC_X86_REG_RDX

    def strcmp(uc):
        a, b = read_cstr(uc, uc.reg_read(UC_X86_REG_RCX)), read_cstr(uc, uc.reg_read(UC_X86_REG_RDX))
        return 0 if a == b else (1 if a > b else 0xFFFFFFFF)

    def strcpy_s(uc):
        dst, size, src = uc.reg_read(UC_X86_REG_RCX), uc.reg_read(UC_X86_REG_RDX), uc.reg_read(UC_X86_REG_R8)
        s = read_cstr(uc, src)
        if len(s) + 1 > size:
            if size:
                uc.mem_write(dst, b"\0")
            return 34                    # ERANGE
        uc.mem_write(dst, s + b"\0")
        return 0

    def sprintf_like(uc, args):
        dst, size = uc.reg_read(UC_X86_REG_RCX), uc.reg_read(UC_X86_REG_RDX)
        s = _format(uc, read_cstr(uc, uc.reg_read(UC_X86_REG_R8)), args)
        if len(s) + 1 > size:
            if size:
                uc.mem_write(dst, b"\0")
            return 0xFFFFFFFF
        uc.mem_write(dst, s + b"\0")
        return len(s)

    def sprintf_s(uc):
        # Windows x64 varargs: 4th argument in r9, the rest on the stack from rsp+0x28 (rsp points at the return)
        rsp, state = uc.reg_read(UC_X86_REG_RSP), {"n": 0}

        def nxt():
            n = state["n"]
            state["n"] += 1
            return uc.reg_read(UC_X86_REG_R9) if n == 0 else struct.unpack("<Q", uc.mem_read(rsp + 0x20 + 8 * n, 8))[0]
        return sprintf_like(uc, nxt)

    def vsprintf_s(uc):
        state = {"p": uc.reg_read(UC_X86_REG_R9)}    # va_list: pointer to consecutive 64-bit arguments

        def nxt():
            v = struct.unpack("<Q", uc.mem_read(state["p"], 8))[0]
            state["p"] += 8
            return v
        return sprintf_like(uc, nxt)

    hook_import(uc, "strcmp", strcmp)
    hook_import(uc, "strcpy_s", strcpy_s)
    hook_import(uc, "sprintf_s", sprintf_s)
    hook_import(uc, "vsprintf_s", vsprintf_s)


def hook(uc, va, fn):
    """Intercept the function at va: fn(uc) runs instead and returns a value; the function's own code never runs."""
    def on_code(uc, addr, size, _):
        value = fn(uc) or 0
        rsp = uc.reg_read(UC_X86_REG_RSP)
        uc.reg_write(UC_X86_REG_RIP, struct.unpack("<Q", uc.mem_read(rsp, 8))[0])
        uc.reg_write(UC_X86_REG_RSP, rsp + 8)
        uc.reg_write(UC_X86_REG_RAX, value)
    uc.hook_add(UC_HOOK_CODE, on_code, begin=va, end=va)


def call(uc, va, regs):
    """Call the function at va with the given registers (Windows x64 convention) until it returns."""
    rsp = STACK + 0x100000 - 0x1000
    uc.mem_write(rsp, struct.pack("<Q", SENTINEL))
    uc.reg_write(UC_X86_REG_RSP, rsp)
    for reg, value in regs.items():
        uc.reg_write(reg, value)
    uc.emu_start(va, SENTINEL)
