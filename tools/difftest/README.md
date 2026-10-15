# difftest: original-vs-reimplementation emulator harness

Proves, offline, that a C rewrite in `src/reimpl/*.c` behaves like the original function in
`lomse.exe` (MSVC5, x86). No game, Wine or GUI. The exe is read in place, never copied.

## Run

    tools/difftest/run.sh 0x404ab0            # one function (also 0x4026a0, 0x4ac0b0)
    tools/difftest/run.sh all --trials 5000   # everything; options: --trials N --seed S

First run creates `.venv` (gitignored) and installs `unicorn`. `LOMSE_EXE` overrides the exe path
(default: `lom-artifacts-keep/decomp/bin/lomse.exe`). Exit 0 only if all three pass and the function is PROVEN (see Coverage):
1. reimpl vs original: 0 divergent trials
2. control 1, original vs itself (second run on a shifted stack): 0 divergent
3. control 2, reimpl built with `-DMUTANT` (a flipped constant): must diverge

## How it works

- `emu.py` maps lomse's PE sections at their VAs and the reimpl blob (the `src/reimpl/*.c` objects,
  built with the DLL's compiler and flags plus `-ffreestanding`, linked by `ld` at base
  0x10000000) into one Unicorn instance.
- Per trial one seeded random state is written (this-object, VM stack, args, registers, stack
  garbage, a combat-state global); the original runs, every write is undone, the reimpl runs from
  the identical state.
- Any fetch from lomse code outside the function under test is a callee: recorded as
  (target, ECX if thiscall, stack args, model-specific pointee, digest of net memory writes so
  far), skipped, and answered with a seeded value; callee-popped args are popped. Pointers into
  the stack compare as `STK`. Models live in `targets.py`.
- `__ftol` (0x5394a0) is an intrinsic: it runs for real in the original and is inlined by the
  compiler in C, so it is not a trace entry.
- Compared: EAX (EDX for 64-bit returns), ESP after return, EBX/ESI/EDI/EBP, FPU control word and
  stack depth, the net memory effect (bytes changed outside the function's own stack frame), and
  the ordered callee trace including the memory state visible at each call.

## Coverage

Every trial records which original instructions executed. The function's bytes are swept with
capstone, split into basic blocks at branch targets and after branches/returns, and reported as
instruction % and block %, with each unreached block, its address range and the branch edge into it.
A function is PROVEN only if reimpl shows 0 divergences and every block is reached or waived in
`waivers.json` (`{"0x<func>": {"0x<block start>": "reason"}}`); otherwise it prints UNPROVEN and
exits non-zero. Coverage is of the original only, per run of the `reimpl` mode, and counts "reached
at the instruction level" (it does not measure data-dependent behaviour inside a block, e.g. all
float classes). No waivers are currently needed.

## What it does not prove

- Only paths the random inputs reach. Sanity lines show faults, callee sequences and bytes written.
- Intermediate/dead stores are not compared, only net effect plus state seen at each call (the
  compiler may legally reorder or drop them).
- Callees are replaced by models, so a callee's own behaviour and the real interplay (e.g. the
  real 0x4026a0 outputs feeding `op_abracadabra`) are untested until those are checked on their own.
- Output pointers of 0x4026a0 are assumed not to alias (the original reads outputs back through
  the pointers; no caller aliases them).
- Writes to the function's own stack arg slots are ignored (MSVC reuses them as locals).
- No threads, interrupts, SEH, x87 precision-control changes, or reads of unmapped memory.
- Not run in the real process: calling-convention fit with the DLL redirect is checked here by ESP
  and register comparison only.

## Results

Wall time is agent working time on this function (harness build excluded, about 6 min of the
total). Seed 1, 2000 trials each; run time is for reimpl + self + mutant together.

| Function | Size | Wall time | Trials | reimpl (default / -march=i486) | self control | mutant control | cache mutant | Coverage (insn / blocks) | Verdict |
|---|---|---|---|---|---|---|---|---|---|
| 0x404ab0 script_raise_error | 36 B | ~1 min | 2000 | 0 / 0 divergent | 0 | caught (2000/2000) | n/a | 10/10, 1/1 | PROVEN |
| 0x4026a0 script_value_decode | 149 B | ~3 min | 2000 | 0 / 0 divergent | 0 | caught (324/2000) | n/a | 54/54, 7/7 | PROVEN |
| 0x4ac0b0 op_abracadabra | 363 B | ~3 min | 2000 | 0 / 0 divergent | 0 | caught (401/2000) | caught (266/2000) | 112/112, 19/19 | PROVEN |

The cache mutant (`-DMUTANT_CACHE`) keeps the script stack pointer across the combat-state call;
callee models for 0x4d4550 and 0x4abec0 scramble the script object (sp, value slots, error flag,
counter) so such caching shows up. Faulted or non-returning runs make a function UNPROVEN; all runs
above had none.

Coverage before targeted inputs: 0x404ab0 and 0x4026a0 were already complete; 0x4ac0b0 was 111/112
instructions and 18/19 blocks (the sp==0 push-failure block at 0x4ac1d6 was unreachable with
non-negative `sp`; fixed by generating negative `sp`, which makes two successful pops land on 0).

Real divergences the harness found on the first drafts: `(int)float` saturates to INT_MIN while
the original `__ftol` stores 64 bits and keeps the low word (fixed with `lom_ftol`, 156/2000 trials
in 0x4026a0, 45/2000 in the operator); and an aliasing-order difference in 0x4026a0's outputs.

## Runtime redirect (not covered by this harness)

`src/lomhd_reimpl.c` patches the originals with `jmp rel32` to the reimpls at DLL attach; the
harness tests the C bodies (including their call counters, whose writes it ignores) but does not
emulate the patched image. The jmp encoding, hash and header checks are unit-tested natively:
`cc -Iinc -Itests/native -std=c99 -Wall -o /tmp/lr tests/lomhd_reimpl_test.c src/lomhd_reimpl_core.c && LOMSE_EXE=<lomse.exe> /tmp/lr`.
Gating: folder `lomhd_reimpl\` beside the exe; inside it a file `all`, or one file per address
(`0x4026a0`), enables entries. Rows live in `LOMHD_REIMPL_LIST` (`inc/lomhd_reimpl.h`).
