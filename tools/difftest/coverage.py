"""Instruction/basic-block coverage of the original function (capstone linear sweep)."""
import json
import os

from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import X86_OP_IMM

WAIVERS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "waivers.json")


def blocks_of(code, lo, hi):
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    insns = list(md.disasm(code, lo))
    addrs = [i.address for i in insns]
    leaders = {lo}
    info = {}
    for n, i in enumerate(insns):
        nxt = i.address + i.size
        is_jmp = i.mnemonic.startswith("j")
        is_ret = i.mnemonic.startswith("ret")
        if is_jmp or is_ret:
            if nxt < hi:
                leaders.add(nxt)
        if is_jmp and i.operands and i.operands[0].type == X86_OP_IMM:
            t = i.operands[0].imm
            if lo <= t < hi:
                leaders.add(t)
            info[nxt] = info.get(nxt, "") + ""
            info.setdefault(("guard", t), "%s %s -> taken at %#x" % (i.mnemonic, i.op_str, t))
        if is_jmp and i.mnemonic != "jmp":
            info.setdefault(("fall", nxt), "fallthrough of %s %s at %#x" % (i.mnemonic, i.op_str, i.address))
    leaders = sorted(a for a in leaders if a in set(addrs))
    blocks = []
    for k, a in enumerate(leaders):
        end = leaders[k + 1] if k + 1 < len(leaders) else hi
        members = [x for x in addrs if a <= x < end]
        blocks.append((a, end, members))
    return blocks, info, {i.address: i for i in insns}


def report(addr, hi, code, executed, name):
    """Print coverage; return True when every block is reached or waived."""
    blocks, info, insns = blocks_of(code, addr, hi)
    waivers = {}
    if os.path.exists(WAIVERS):
        waivers = json.load(open(WAIVERS)).get(hex(addr), {})
    n_ins = len(insns)
    hit_ins = sum(1 for a in insns if a in executed)
    hit_blocks = [b for b in blocks if b[0] in executed]
    miss = [b for b in blocks if b[0] not in executed]
    unwaived = [b for b in miss if hex(b[0]) not in waivers]
    waived_ins = sum(len(b[2]) for b in miss if hex(b[0]) in waivers)
    print("  coverage: instructions %d/%d (%.1f%%), blocks %d/%d (%.1f%%)%s" % (
        hit_ins, n_ins, 100.0 * hit_ins / n_ins, len(hit_blocks), len(blocks),
        100.0 * len(hit_blocks) / len(blocks),
        "" if not waivers else ", waived blocks %d" % sum(1 for b in miss if hex(b[0]) in waivers)))
    for a, end, members in miss:
        why = waivers.get(hex(a))
        guard = info.get(("guard", a)) or info.get(("fall", a)) or "no branch edge found"
        print("    unreached block %#x-%#x (%d insn): %s%s" % (
            a, end, len(members), guard, "  [WAIVED: %s]" % why if why else ""))
    # An instruction inside a reached block that never ran means a block split by a fault or a stop.
    partial = [a for b in hit_blocks for a in b[2] if a not in executed]
    for a in partial:
        print("    reached block but instruction %#x not executed" % a)
    return not unwaived and not partial, hit_ins, n_ins, len(hit_blocks), len(blocks), len(miss) - len(unwaived)
