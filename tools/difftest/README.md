# difftest: original-vs-reimplementation emulator harness

Proves, offline, that a C rewrite in `src/reimpl/*.c` behaves like the original function in
`lomse.exe` (MSVC5, x86). No game, Wine or GUI. The exe is read in place, never copied.

## Run

    tools/difftest/run.sh 0x404ab0            # one function (also 0x4026a0, 0x4ac0b0)
    tools/difftest/run.sh all --trials 5000   # everything; options: --trials N --seed S

First run creates `.venv` (gitignored) and installs `unicorn`. `LOMSE_EXE` overrides the exe path
(default: `lom-artifacts-keep/decomp/bin/lomse.exe`). Exit 0 only if all three pass:
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

| Function | Size | Wall time | Trials | reimpl | self control | mutant control |
|---|---|---|---|---|---|---|
| 0x404ab0 script_raise_error | 36 B | ~1 min | 2000 | 0 divergent | 0 | caught (2000/2000) |
| 0x4026a0 script_value_decode | 149 B | ~3 min | 2000 | 0 divergent | 0 | caught (324/2000) |
| 0x4ac0b0 op_abracadabra | 363 B | ~3 min | 2000 | 0 divergent | 0 | caught (540/2000) |

Real divergences the harness found on the first drafts: `(int)float` saturates to INT_MIN while
the original `__ftol` stores 64 bits and keeps the low word (fixed with `lom_ftol`, 156/2000 trials
in 0x4026a0, 45/2000 in the operator); and an aliasing-order difference in 0x4026a0's outputs.
