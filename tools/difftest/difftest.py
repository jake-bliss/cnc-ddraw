#!/usr/bin/env python3
"""Differential test: original lomse.exe function vs src/reimpl C, under emulation."""
import argparse
import os
import random
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import emu as E
import targets as T
import coverage as C

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DEFAULT_EXE = "/Users/jakebliss/personal-projects/lom-artifacts-keep/decomp/bin/lomse.exe"


def diff_obs(a, b, ret):
    """Return a list of human-readable differences between two observations."""
    d = []
    if a["fault"] != b["fault"]:
        d.append("fault: %s vs %s" % (a["fault"], b["fault"]))
    if a["eip"] != b["eip"]:
        d.append("did not return to sentinel: %#x vs %#x" % (a["eip"], b["eip"]))
    if a["esp_delta"] != b["esp_delta"]:
        d.append("ESP after return (relative to entry): %d vs %d" % (a["esp_delta"], b["esp_delta"]))
    if ret in ("eax", "eax_edx") and a["eax"] != b["eax"]:
        d.append("EAX: %#x vs %#x" % (a["eax"], b["eax"]))
    if ret == "eax_edx" and a["edx"] != b["edx"]:
        d.append("EDX: %#x vs %#x" % (a["edx"], b["edx"]))
    for n in ("ebx", "esi", "edi", "ebp"):
        if a[n] != b[n]:
            d.append("%s: %#x vs %#x" % (n, a[n], b[n]))
    for n in ("fpcw", "fptag", "fptop"):
        if a[n] != b[n]:
            d.append("FPU %s: %#x vs %#x" % (n, a[n], b[n]))
    wa, wb = a["writes"], b["writes"]
    if wa != wb:
        for addr in sorted(set(wa) | set(wb)):
            if wa.get(addr) != wb.get(addr):
                d.append("memory byte %#x: %s vs %s" % (addr, wa.get(addr), wb.get(addr)))
                if len(d) > 12:
                    break
    ca, cb = a["callees"], b["callees"]
    if ca != cb:
        for i in range(max(len(ca), len(cb))):
            x = ca[i] if i < len(ca) else None
            y = cb[i] if i < len(cb) else None
            if x != y:
                d.append("callee #%d: %s vs %s" % (i, x, y))
                break
    return d


def run_pair(emu, tgt, entry_a, range_a, entry_b, range_b, seed, shift_b):
    rng = random.Random("%s/setup" % seed)
    spec = tgt["setup"](emu, rng)
    emu.log_setup = list(emu.log)
    setup_log = emu.log
    emu.log = []
    obs_a = emu.run(entry_a, range_a, spec, seed)
    emu.undo()
    obs_b = emu.run(entry_b, range_b, spec, seed, esp_shift=shift_b)
    emu.undo()
    emu.log = setup_log
    emu.undo()
    return obs_a, obs_b


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("addr")
    ap.add_argument("--mode", choices=["reimpl", "self", "mutant", "mutant_cache"], default="reimpl")
    ap.add_argument("--i486", action="store_true", help="build the reimpls with -march=i486 as build.cmd does")
    ap.add_argument("--trials", type=int, default=2000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--exe", default=os.environ.get("LOMSE_EXE", DEFAULT_EXE))
    args = ap.parse_args()
    addr = int(args.addr, 0)
    tgt = T.TARGETS[addr]
    if args.mode == "mutant_cache" and not tgt.get("cache_mutant"):
        print("%s %s [mutant_cache]: n/a (no callee-cached state to mutate)" % (hex(addr), tgt["name"]))
        return 0
    defines = {"mutant": ["-DMUTANT"], "mutant_cache": ["-DMUTANT_CACHE"]}.get(args.mode, [])
    flags = ["-march=i486"] if args.i486 else []
    tag = args.mode + ("-i486" if args.i486 else "")
    blob, syms = E.build_blob(REPO, os.path.join(REPO, "tools/difftest/build", tag), defines + flags)
    emu = E.Emu(args.exe, blob)
    emu.sigs = T.SIGS
    emu.intrinsics = T.INTRINSICS
    orig_range = (addr, tgt["end"])
    if args.mode == "self":
        entry_b, range_b, shift = addr, orig_range, 0x40
    else:
        entry_b, range_b, shift = E.BLOB_BASE + 0 + (syms[tgt["sym"]] - E.BLOB_BASE), (0, 0), 0
    t0 = time.time()
    bad = 0
    first = None
    paths = {}
    stats = {"faults": 0, "no_return": 0, "callees": 0, "writes": 0}
    for t in range(args.trials):
        seed = "%s:%d" % (args.seed, t)
        a, b = run_pair(emu, tgt, addr, orig_range, entry_b, range_b, seed, shift)
        for o in (a, b):
            stats["faults"] += bool(o["fault"])
            stats["no_return"] += o["eip"] != E.SENTINEL
            stats["callees"] += len(o["callees"])
            stats["writes"] += len(o["writes"])
        key = tuple(c[0] for c in a["callees"]) + (("ret", a["eax"]) if False else ())
        paths[key] = paths.get(key, 0) + 1
        d = diff_obs(a, b, tgt["ret"])
        if d:
            bad += 1
            if first is None:
                first = (t, seed, d)
    dt = time.time() - t0
    print("%s %s [%s]: trials=%d divergent=%d time=%.1fs" % (hex(addr), tgt["name"], tag,
                                                           args.trials, bad, dt))
    print("  sanity: faults=%(faults)d no_return=%(no_return)d callees_traced=%(callees)d "
          "net_bytes_written=%(writes)d (summed over both runs)" % stats)
    if len(paths) > 1 or any(paths):
        print("  callee sequences in the original: " + "; ".join(
            "[%s]x%d" % (" ".join("%x" % x for x in k), n) for k, n in sorted(paths.items())))
    if first:
        print("  first divergence: trial %d (seed %s)" % first[:2])
        for line in first[2]:
            print("    " + line)
    proven = True
    if args.mode == "reimpl":
        code = bytes(emu.uc.mem_read(addr, tgt["end"] - addr))
        ok, *_ = C.report(addr, tgt["end"], code, emu.executed, tgt["name"])
        proven = ok and bad == 0 and stats["faults"] == 0 and stats["no_return"] == 0
        if stats["faults"] or stats["no_return"]:
            print("  faulted or non-returning runs present: not proof")
        print("  %s" % ("PROVEN" if proven else "UNPROVEN"))
    return 1 if (bad or not proven) else 0


if __name__ == "__main__":
    sys.exit(1 if main() else 0)
