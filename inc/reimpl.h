#ifndef REIMPL_H
#define REIMPL_H

/* Clean-room C replacements for functions inside lomse.exe (MSVC5, x86, base 0x400000).
   Each one keeps the original calling convention and stack discipline so the DLL can later
   redirect the original address to it. Functions not yet rewritten are reached through their
   original addresses with LOM_ORIG. */

#include <stddef.h>
#include <stdint.h>

/* Per-function call counter, read by the DLL's log (see lomhd_reimpl.h). Atomic, no OS calls, so
   the offline harness runs the very same code. */
extern volatile int32_t lom_reimpl_calls[];
#define LOM_COUNT(i) ((void)__sync_fetch_and_add(&lom_reimpl_calls[i], 1))

#define LOM_THISCALL __attribute__((thiscall))
#define LOM_ORIG(addr, type) ((type)(uintptr_t)(addr))

/* Type tag held in the low byte of ScriptValue.tag. */
enum {
    SCRIPT_INT = 1,   /* plain 32-bit integer */
    SCRIPT_FIXED = 2, /* signed 24.8 fixed point */
    SCRIPT_FLOAT = 4  /* IEEE single, bits stored in .bits */
};

typedef struct ScriptValue {
    uint32_t tag;
    union {
        int32_t i;
        uint32_t bits;
    };
} ScriptValue;

/* The script VM object (the `this` of the GameScript operators). Only the fields used so far. */
typedef struct GameScript {
    uint8_t pad0[0x50];
    ScriptValue *values;  /* 0x50: value array; a pop reads values[sp] after incrementing sp */
    int32_t sp;           /* 0x54: number of slots consumed from the top; pushes decrement it */
    int32_t limit;        /* 0x58: pops fail once sp reaches this */
    uint8_t pad5c[0x90 - 0x5c];
    int32_t error_code;   /* 0x90: set around the error reporter call */
    uint8_t pad94[0xa4 - 0x94];
    int32_t error_aux;    /* 0xa4: cleared after an error is reported */
} GameScript;

_Static_assert(offsetof(GameScript, values) == 0x50, "values");
_Static_assert(offsetof(GameScript, limit) == 0x58, "limit");
_Static_assert(offsetof(GameScript, error_code) == 0x90, "error_code");
_Static_assert(offsetof(GameScript, error_aux) == 0xa4, "error_aux");
_Static_assert(sizeof(ScriptValue) == 8, "ScriptValue");

/* The original's __ftol truncates through a 64-bit store and keeps the low word, so out-of-range
   values wrap instead of saturating to INT_MIN as a plain (int32_t) cast would. */
static inline int32_t lom_ftol(double x)
{
    return (int32_t)(int64_t)x;
}

/* 0x404ab0 */
void LOM_THISCALL lom_script_raise_error(GameScript *self, int32_t code);
/* 0x4026a0 */
int lom_script_value_decode(const ScriptValue *v, int32_t *out_int, int32_t *out_fixed,
                            float *out_float);
/* 0x4ac0b0 */
void lom_op_abracadabra(GameScript *self);

#endif
