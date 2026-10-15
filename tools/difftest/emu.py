"""Unicorn-based emulation of lomse.exe plus a reimplementation blob, with write/callee capture."""
import hashlib
import struct
import subprocess
import os
import random

from unicorn import (Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE, UC_HOOK_MEM_WRITE, UcError)
from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX,
                               UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EBP, UC_X86_REG_ESP,
                               UC_X86_REG_EIP, UC_X86_REG_EFLAGS, UC_X86_REG_FPCW,
                               UC_X86_REG_FPSW, UC_X86_REG_FPTAG, UC_X86_REG_ST0, UC_X86_REG_ST1,
                               UC_X86_REG_ST2, UC_X86_REG_ST3, UC_X86_REG_ST4, UC_X86_REG_ST5,
                               UC_X86_REG_ST6, UC_X86_REG_ST7)

PAGE = 0x1000
TEXT_LO, TEXT_HI = 0x401000, 0x54D000
SENTINEL = 0x00300000
STACK_LO, STACK_HI = 0x00A00000, 0x00A40000
HEAP_LO, HEAP_SIZE = 0x01000000, 0x100000
BLOB_BASE = 0x10000000
CALLEE_SAVED = [("ebx", UC_X86_REG_EBX), ("esi", UC_X86_REG_ESI), ("edi", UC_X86_REG_EDI),
                ("ebp", UC_X86_REG_EBP)]
ST_REGS = [UC_X86_REG_ST0, UC_X86_REG_ST1, UC_X86_REG_ST2, UC_X86_REG_ST3, UC_X86_REG_ST4,
           UC_X86_REG_ST5, UC_X86_REG_ST6, UC_X86_REG_ST7]


def pe_sections(data):
    """Yield (va, virtual_size, raw_bytes) for each section, and return image base."""
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[pe:pe + 4] == b"PE\0\0"
    nsec, = struct.unpack_from("<H", data, pe + 6)
    optsz, = struct.unpack_from("<H", data, pe + 20)
    base, = struct.unpack_from("<I", data, pe + 24 + 28)
    sec = pe + 24 + optsz
    out = []
    for i in range(nsec):
        vsz, va, rsz, roff = struct.unpack_from("<IIII", data, sec + 40 * i + 8)
        out.append((base + va, vsz, data[roff:roff + min(rsz, vsz or rsz)]))
    return base, out


def page_up(n):
    return (n + PAGE - 1) & ~(PAGE - 1)


def build_blob(repo, outdir, mutant):
    """Compile src/reimpl/*.c with the DLL's compiler/flags and link to a fixed-base PE."""
    os.makedirs(outdir, exist_ok=True)
    srcs = sorted(os.path.join(repo, "src/reimpl", f)
                  for f in os.listdir(os.path.join(repo, "src/reimpl")) if f.endswith(".c"))
    cflags = ["-I" + os.path.join(repo, "inc"), "-O2", "-Wall", "-std=c99", "-ffreestanding",
              "-fno-stack-protector", "-fno-asynchronous-unwind-tables"]
    if mutant:
        cflags.append("-DMUTANT")
    objs = []
    for s in srcs:
        o = os.path.join(outdir, os.path.basename(s)[:-2] + ".o")
        subprocess.run(["i686-w64-mingw32-gcc", *cflags, "-c", s, "-o", o], check=True)
        objs.append(o)
    exe = os.path.join(outdir, "reimpl.exe")
    subprocess.run(["i686-w64-mingw32-ld", "-m", "i386pe", "--image-base", hex(BLOB_BASE),
                    "--section-alignment", "4096", "--file-alignment", "4096",
                    "--disable-reloc-section", "-e", "0", "-o", exe, *objs], check=True)
    nm = subprocess.run(["i686-w64-mingw32-nm", exe], check=True, capture_output=True,
                        text=True).stdout
    syms = {}
    for line in nm.splitlines():
        p = line.split()
        if len(p) == 3:
            syms[p[2].lstrip("_")] = int(p[0], 16)
    return exe, syms


def mask_signed(v, size):
    return v & ((1 << (8 * size)) - 1)


class Emu:
    def __init__(self, exe_path, blob_exe):
        self.uc = Uc(UC_ARCH_X86, UC_MODE_32)
        uc = self.uc
        data = open(exe_path, "rb").read()
        base, secs = pe_sections(data)
        top = max(va + page_up(vsz) for va, vsz, _ in secs)
        uc.mem_map(base, page_up(top - base))
        for va, vsz, raw in secs:
            uc.mem_write(va, raw)
        bdata = open(blob_exe, "rb").read()
        bbase, bsecs = pe_sections(bdata)
        btop = max(va + page_up(vsz) for va, vsz, _ in bsecs)
        uc.mem_map(bbase, page_up(btop - bbase))
        for va, vsz, raw in bsecs:
            uc.mem_write(va, raw)
        uc.mem_map(SENTINEL, PAGE)
        uc.mem_write(SENTINEL, b"\xcc")
        uc.mem_map(STACK_LO, STACK_HI - STACK_LO)
        uc.mem_map(HEAP_LO, HEAP_SIZE)
        self.log = []          # (addr, size, old_bytes, new_bytes, tag)
        self.recording = False
        self.callees = []
        self.model = None
        self.fn_lo = self.fn_hi = 0
        self.sigs = {}
        self.intrinsics = set()
        self.in_intrinsic_until = None
        self.entry_esp = 0
        self.nargs = 0
        self.call_idx = 0
        self.seed = 0
        uc.hook_add(UC_HOOK_MEM_WRITE, self._on_write)
        uc.hook_add(UC_HOOK_CODE, self._on_code, begin=TEXT_LO, end=TEXT_HI)

    def _on_write(self, uc, access, addr, size, value, _):
        if not self.recording:
            return
        old = bytes(uc.mem_read(addr, size))
        new = (value & ((1 << (8 * size)) - 1)).to_bytes(size, "little") if size <= 8 else None
        if new is None:
            raise RuntimeError("write wider than 8 bytes at %#x" % addr)
        self.log.append((addr, size, old, new, "emu"))

    def poke(self, addr, data, tag="harness"):
        """Memory write by the harness itself; logged so it can be undone."""
        old = bytes(self.uc.mem_read(addr, len(data)))
        self.log.append((addr, len(data), old, bytes(data), tag))
        self.uc.mem_write(addr, bytes(data))

    def u32(self, addr):
        return struct.unpack("<I", self.uc.mem_read(addr, 4))[0]

    def undo(self):
        for addr, size, old, new, tag in reversed(self.log):
            self.uc.mem_write(addr, old)
        self.log = []

    def net_writes(self):
        """Bytes whose value differs from before the run, outside the callee's own stack frame."""
        first_old, last = {}, {}
        for addr, size, old, new, tag in self.log:
            if tag not in ("emu", "model"):
                continue
            for i in range(size):
                first_old.setdefault(addr + i, old[i])
                last[addr + i] = new[i]
        lo, hi = STACK_LO, self.entry_esp + 4 * self.nargs  # arg slots are callee scratch
        return {a: v for a, v in last.items() if v != first_old[a] and not (lo <= a < hi)}

    def digest(self):
        net = self.net_writes()
        return hashlib.sha1(repr(sorted(net.items())).encode()).hexdigest()[:12]

    def norm(self, v):
        return "STK" if STACK_LO <= v < STACK_HI else v

    def _on_code(self, uc, addr, size, _):
        if self.in_intrinsic_until is not None:
            if addr == self.in_intrinsic_until:
                self.in_intrinsic_until = None
            return
        if self.fn_lo <= addr < self.fn_hi:
            return
        esp = uc.reg_read(UC_X86_REG_ESP)
        if addr in self.intrinsics:
            self.in_intrinsic_until = self.u32(esp)
            return
        sig = self.sigs.get(addr)
        if sig is None:
            raise RuntimeError("unmodelled call into original code at %#x" % addr)
        ret = self.u32(esp)
        args = [self.u32(esp + 4 + 4 * i) for i in range(sig["nargs"])]
        ecx = uc.reg_read(UC_X86_REG_ECX) if sig["conv"] == "thiscall" else None
        extra = sig["model"].describe(self, args, ecx) if sig.get("model") else ()
        self.callees.append((addr, self.norm(ecx) if ecx is not None else None,
                             tuple(self.norm(a) for a in args), extra, self.digest()))
        rng = random.Random("%s/%d/%x" % (self.seed, self.call_idx, addr))
        self.call_idx += 1
        eax = sig["model"].apply(self, rng, args, ecx) if sig.get("model") else rng.getrandbits(32)
        uc.reg_write(UC_X86_REG_EAX, eax & 0xFFFFFFFF)
        pop = 4 + (4 * sig["nargs"] if sig["conv"] in ("thiscall", "stdcall") else 0)
        uc.reg_write(UC_X86_REG_ESP, esp + pop)
        uc.reg_write(UC_X86_REG_EIP, ret)

    def run(self, entry, fn_range, spec, seed, esp_shift=0):
        """Run one call from the already-prepared memory; returns an observation dict."""
        uc = self.uc
        self.callees, self.call_idx, self.seed = [], 0, seed
        self.fn_lo, self.fn_hi = fn_range
        self.in_intrinsic_until = None
        esp = STACK_HI - 0x1000 - esp_shift
        stack_rng = random.Random("stack/%s" % seed)
        self.recording = True
        self.poke(esp - 0x800, stack_rng.randbytes(0x800), "stackfill")
        esp -= 4 * len(spec["args"])
        for i, a in enumerate(spec["args"]):
            self.poke(esp + 4 * i, struct.pack("<I", a & 0xFFFFFFFF), "args")
        esp -= 4
        self.poke(esp, struct.pack("<I", SENTINEL), "ret")
        self.entry_esp = esp + 4
        self.nargs = len(spec["args"])
        regs = spec["regs"]
        for name, r in [("eax", UC_X86_REG_EAX), ("ebx", UC_X86_REG_EBX), ("ecx", UC_X86_REG_ECX),
                        ("edx", UC_X86_REG_EDX), ("esi", UC_X86_REG_ESI),
                        ("edi", UC_X86_REG_EDI), ("ebp", UC_X86_REG_EBP)]:
            uc.reg_write(r, regs[name])
        uc.reg_write(UC_X86_REG_ESP, esp)
        uc.reg_write(UC_X86_REG_EFLAGS, 0x202)
        uc.reg_write(UC_X86_REG_FPCW, 0x37F)
        uc.reg_write(UC_X86_REG_FPSW, 0)
        uc.reg_write(UC_X86_REG_FPTAG, 0xFFFF)
        fault = None
        try:
            uc.emu_start(entry, SENTINEL, count=200000)
        except (UcError, RuntimeError) as e:
            fault = str(e)
        self.recording = False
        obs = {"fault": fault, "eip": uc.reg_read(UC_X86_REG_EIP)}
        obs["esp_delta"] = uc.reg_read(UC_X86_REG_ESP) - self.entry_esp
        obs["eax"] = uc.reg_read(UC_X86_REG_EAX)
        obs["edx"] = uc.reg_read(UC_X86_REG_EDX)
        for name, r in CALLEE_SAVED:
            obs[name] = uc.reg_read(r)
        obs["fpcw"] = uc.reg_read(UC_X86_REG_FPCW)
        obs["fptag"] = uc.reg_read(UC_X86_REG_FPTAG)
        obs["fptop"] = (uc.reg_read(UC_X86_REG_FPSW) >> 11) & 7
        obs["writes"] = self.net_writes()
        obs["callees"] = list(self.callees)
        return obs
