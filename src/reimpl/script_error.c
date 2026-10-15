#include "reimpl.h"

typedef void(LOM_THISCALL *ReportScriptErrorFn)(GameScript *);
#define orig_report_script_error LOM_ORIG(0x4d4550, ReportScriptErrorFn)

/* 0x404ab0: publish `code` for the reporter to read, run it, then clear the error state. */
void LOM_THISCALL lom_script_raise_error(GameScript *self, int32_t code)
{
    self->error_code = code;
    orig_report_script_error(self);
    self->error_code = 0;
#ifdef MUTANT
    self->error_aux = 1;
#else
    self->error_aux = 0;
#endif
}
