"""Per-function test specs: calling convention, randomised input state, callee models."""
import random
import struct

from emu import HEAP_LO, SENTINEL

MASK = 0xFFFFFFFF
COMBAT_STATE = 0x5AE958


def rand_float_bits(rng):
    k = rng.randrange(10)
    if k == 0:
        return rng.getrandbits(32)
    specials = [0x7FC00000, 0x7F800000, 0xFF800000, 0x80000000, 0, 0x4F000000, 0xCF000000,
                0x4EFFFFFF, 0x4F000001, 0xCF000001, 0x3F800000, 0xBF800000, 0x7F7FFFFF]
    if k == 1:
        return rng.choice(specials)
    if k in (2, 3):
        return struct.unpack("<I", struct.pack("<f", rng.uniform(-1e6, 1e6)))[0]
    if k in (4, 5):
        return struct.unpack("<I", struct.pack("<f", rng.uniform(-1e10, 1e10)))[0]
    if k == 6:
        return struct.unpack("<I", struct.pack("<f", rng.uniform(-3, 3)))[0]
    if k == 7:
        return struct.unpack("<I", struct.pack("<f", rng.uniform(-9e6, 9e6)))[0]
    return struct.unpack("<I", struct.pack("<f", float(rng.randrange(-70000, 70000)) / 256))[0]


def rand_int(rng):
    k = rng.randrange(6)
    if k == 0:
        return rng.getrandbits(32)
    if k == 1:
        return rng.choice([0, 1, 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000, 0x80, 0xFF, 0x100, 0xFFFFFF00])
    if k == 2:
        return rng.randrange(-300, 300) & MASK
    return rng.randrange(-100000, 100000) & MASK


def rand_tag(rng):
    low = rng.choice([0, 1, 1, 1, 2, 2, 2, 4, 4, 4, 3, 5, 0xFF, 0x81, 0x82, 0x84])
    return (rng.getrandbits(24) << 8) | low


def rand_value_slot(rng):
    tag = rand_tag(rng)
    low = tag & 0xFF
    word = rand_float_bits(rng) if low == 4 else rand_int(rng)
    return struct.pack("<II", tag, word)


def rand_regs(rng):
    return {n: rng.getrandbits(32) for n in ("eax", "ebx", "ecx", "edx", "esi", "edi", "ebp")}


class Describe4026a0:
    """Callee model for 0x4026a0 (cdecl, 4 args): fixed pseudo-random outputs."""
    nargs = 4

    @staticmethod
    def describe(emu, args, ecx):
        tag = emu.u32(args[0])
        low = tag & 0xFF
        if low in (1, 2, 4):
            return (low, emu.u32(args[0] + 4))
        return (low,)

    @staticmethod
    def apply(emu, rng, args, ecx):
        ok = rng.random() < 0.85
        if ok:
            for a in args[1:3]:
                emu.poke(a, struct.pack("<I", rand_int(rng)), "model")
            emu.poke(args[3], struct.pack("<I", rand_float_bits(rng)), "model")
        return int(ok)


class Generic:
    @staticmethod
    def describe(emu, args, ecx):
        return ()

    @staticmethod
    def apply(emu, rng, args, ecx):
        return rng.getrandbits(32)


SIGS = {
    0x4D4550: {"conv": "thiscall", "nargs": 0, "model": Generic},
    0x404AB0: {"conv": "thiscall", "nargs": 1, "model": Generic},
    0x4ABEC0: {"conv": "thiscall", "nargs": 3, "model": Generic},
    0x4026A0: {"conv": "cdecl", "nargs": 4, "model": Describe4026a0},
}
INTRINSICS = {0x5394A0}  # __ftol: runs for real in the original, inlined by the compiler in C


def setup_heap(emu, rng, size=0x4000):
    emu.poke(HEAP_LO, rng.randbytes(size))


def setup_404ab0(emu, rng):
    setup_heap(emu, rng)
    this = HEAP_LO + 0x100 * rng.randrange(1, 8)
    arg = rand_int(rng)
    regs = rand_regs(rng)
    regs["ecx"] = this
    return {"args": [arg], "regs": regs}


def setup_4026a0(emu, rng):
    setup_heap(emu, rng)
    val = HEAP_LO + 0x1000
    emu.poke(val, rand_value_slot(rng))
    outs = [HEAP_LO + 0x2000 + 0x10 * i for i in range(3)]
    return {"args": [val] + outs, "regs": rand_regs(rng)}


def setup_op(emu, rng):
    setup_heap(emu, rng)
    emu.poke(COMBAT_STATE, rng.randbytes(0x40))
    this = HEAP_LO + 0x400
    arr = HEAP_LO + 0x1000 + 8 * rng.randrange(0, 4)
    limit = rng.randrange(0, 9)
    sp = rng.choice([0, 0, 1, 2, -2, -2, -1, rng.randrange(0, 11), limit, limit - 1, limit + 1])
    emu.poke(this + 0x50, struct.pack("<IiI", arr, sp, limit))
    for i in range(16):
        emu.poke(arr + 8 * i, rand_value_slot(rng))
    regs = rand_regs(rng)
    return {"args": [this], "regs": regs}


# name -> spec. ret: "void" | "eax" | "eax_edx"
TARGETS = {
    0x404AB0: dict(name="script_raise_error", sym="lom_script_raise_error", end=0x404AD1 + 3,
                   ret="void", setup=setup_404ab0, thiscall=True),
    0x4026A0: dict(name="script_value_decode", sym="lom_script_value_decode", end=0x402735,
                   ret="eax", setup=setup_4026a0, thiscall=False),
    0x4AC0B0: dict(name="op_abracadabra", sym="lom_op_abracadabra", end=0x4AC21B,
                   ret="void", setup=setup_op, thiscall=False),
}
