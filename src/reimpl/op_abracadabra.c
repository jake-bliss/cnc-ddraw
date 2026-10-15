#include "reimpl.h"

typedef void(LOM_THISCALL *ReportScriptErrorFn)(GameScript *);
typedef void(LOM_THISCALL *RaiseErrorFn)(GameScript *, int32_t);
typedef int32_t(LOM_THISCALL *CombatQueryFn)(void *, int32_t, int32_t, int32_t);
typedef int (*DecodeFn)(const ScriptValue *, int32_t *, int32_t *, float *);

#define orig_report_script_error LOM_ORIG(0x4d4550, ReportScriptErrorFn)
#define orig_raise_error LOM_ORIG(0x404ab0, RaiseErrorFn)
#define orig_decode_value LOM_ORIG(0x4026a0, DecodeFn)
#define orig_combat_query LOM_ORIG(0x4abec0, CombatQueryFn)
#define COMBAT_STATE ((void *)0x5ae958)

enum { ERR_STACK_EMPTY = 3, ERR_STACK_UNDERFLOW = 6, ERR_BAD_ARG = 0xe };

/* The original inlines raise_error here; this is that sequence. */
static void raise_inline(GameScript *s, int32_t code)
{
    s->error_code = code;
    orig_report_script_error(s);
    s->error_code = 0;
    s->error_aux = 0;
}

static ScriptValue pop_value(GameScript *s)
{
    ScriptValue v;
    if (s->sp < s->limit) {
        s->sp++;
        v = s->values[s->sp - 1];
    } else {
        raise_inline(s, ERR_STACK_UNDERFLOW);
        v.tag = 0;
        v.i = 0;
    }
    return v;
}

/* 0x4ac0b0: pops an argument and a second value, asks the combat state about them
   (flags 0x40000) and pushes the integer result. */
void lom_op_abracadabra(GameScript *self)
{
    LOM_COUNT(2);
    ScriptValue first = pop_value(self);
    int32_t arg;

    switch (first.tag & 0xff) {
    case SCRIPT_INT:
        arg = first.i;
        break;
    case SCRIPT_FIXED:
        arg = first.i >> 8;
        break;
    case SCRIPT_FLOAT: {
        union {
            uint32_t bits;
            float f;
        } u;
        u.bits = first.bits;
        arg = lom_ftol(u.f);
        break;
    }
    default:
        orig_raise_error(self, ERR_BAD_ARG);
        return;
    }

    ScriptValue second = pop_value(self);
    int32_t as_int, as_fixed;
    float as_float;

    if (!orig_decode_value(&second, &as_int, &as_fixed, &as_float)) {
        raise_inline(self, ERR_BAD_ARG);
        return;
    }

#ifdef MUTANT
    int32_t result = orig_combat_query(COMBAT_STATE, as_int, arg, 0x40001);
#else
    int32_t result = orig_combat_query(COMBAT_STATE, as_int, arg, 0x40000);
#endif

    if (self->sp == 0) {
        raise_inline(self, ERR_STACK_EMPTY);
        return;
    }
    self->sp--;
    self->values[self->sp].tag = SCRIPT_INT;
    self->values[self->sp].i = result;
}
