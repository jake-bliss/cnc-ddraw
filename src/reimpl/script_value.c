#include "reimpl.h"

/* 0x4026a0: expand one script value into integer, 24.8 fixed and float views.
   Returns 0 (touching nothing) for an unknown tag. */
int lom_script_value_decode(const ScriptValue *v, int32_t *out_int, int32_t *out_fixed,
                            float *out_float)
{
    switch (v->tag & 0xff) {
    case SCRIPT_INT: {
        int32_t n = v->i;
        *out_int = n;
#ifdef MUTANT
        *out_fixed = (int32_t)((uint32_t)n << 9);
#else
        *out_fixed = (int32_t)((uint32_t)n << 8);
#endif
        *out_float = (float)n;
        return 1;
    }
    case SCRIPT_FIXED: {
        int32_t f = v->i;
        *out_fixed = f;
        *out_int = f >> 8;
        *out_float = (float)((double)f * (1.0 / 256.0));
        return 1;
    }
    case SCRIPT_FLOAT: {
        union {
            uint32_t bits;
            float f;
        } u;
        u.bits = v->bits;
        *out_float = u.f;
        *out_fixed = lom_ftol((double)u.f * 256.0);
        *out_int = lom_ftol(u.f);
        return 1;
    }
    default:
        return 0;
    }
}
