#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dd.h"
#include "hook.h"
#include "git.h"
#include "version.h"
#include "dllmain.h"
#include "lomhd.h"
#include "lomhd_crash.h"

/* Crash and hang reports for playtesters: a readable lomhd_crash_*.txt and a minidump beside the
 * game when it dies of an exception nothing handled, and a lomhd_hang_*.txt when its window stops
 * responding while in front. The text is the part meant to be read from a bug report; see
 * src/lomhd_crash_core.c for its layout.
 *
 * WHICH EXCEPTIONS. Only unhandled ones: this is a top-level (SetUnhandledExceptionFilter) filter,
 * which Windows and Wine call after every frame-based handler has declined -- never for a
 * first-chance exception the game or a system DLL catches (IsBadReadPtr raises and catches access
 * violations all day). A vectored handler also records the last few serious first-chance
 * exceptions, but only as context inside a report and as a capped note in lomhd.log.
 *
 * THE FILTER CHAIN. There is one top-level filter per process, and a game or Storm may set its own.
 * Two mechanisms keep ours first without losing theirs:
 *   1. cnc-ddraw already patches SetUnhandledExceptionFilter in the import tables of every module in
 *      the game folder (hook.c; upstream did it for debug builds, this fork for all gcc builds).
 *      Such a call lands in lomhd_crash_set_filter, which keeps the process-wide filter ours and
 *      records theirs as the "game top" -- returning what the real call would have returned, so a
 *      filter that chains to its predecessor still reaches it. Native semantics, one level down.
 *   2. A call that bypasses the patched tables (GetProcAddress, a module outside the game folder)
 *      really replaces ours. The reporter thread re-asserts ours once a second: it swaps ours back
 *      in and pushes the newcomer on the chain above the previous game top. The newcomer's own
 *      "previous filter" is ours, so when it chains back into us we continue one level down
 *      instead of recursing (see lc_filter). Between the replacement and the re-assert (up to a
 *      second) a crash goes to the newcomer alone: the one gap.
 * After the report is written and closed we call the chain and return its answer; with nothing to
 * chain to, EXCEPTION_CONTINUE_SEARCH -- so Windows Error Reporting or Wine's crash dialog and
 * winedbg still do exactly what they did before.
 *
 * THE CRASH PATH runs on the crashing thread: static buffers, no heap, no locks we do not own, no
 * C runtime formatting, function pointers resolved at startup, paths built at startup. The module
 * list comes from a snapshot the reporter thread refreshes every two seconds (Toolhelp takes the
 * loader lock, which a crashing thread may hold); an address in a module loaded since is found
 * with VirtualQuery and named from its export directory. The text is written and flushed before
 * the minidump is attempted, and both are closed before the chain runs: whatever the game's own
 * filter or its shutdown does next -- including hang in a DllMain -- the report is already on disk. */

#define LC_MAX_MODULES 160
#define LC_MAX_CHAIN 8
#define LC_MAX_THREADS 8
#define LC_MAX_REPORTS 4                   /* per session */
#define LC_HANG_MS 20000
#define LC_LOG_LINES 30
#define LC_SEEN_LOG_MAX 10

typedef BOOL(WINAPI* MINIDUMPWRITEDUMPPROC)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
    PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
typedef BOOL(WINAPI* ISHUNGAPPWINDOWPROC)(HWND);

static volatile LONG g_installed;
static volatile LONG g_detached;           /* DLL_PROCESS_DETACH seen: dbghelp may be gone */
static volatile LONG g_stop;
static BOOL g_hang_reports;
static DWORD g_start_tick;

static char g_dir[MAX_PATH];               /* game folder, with a trailing backslash */
static char g_exe_path[MAX_PATH];
static DWORD g_exe_size, g_exe_timestamp, g_exe_checksum;
static DWORD g_exe_base;
static char g_exe_sha[65];
static volatile LONG g_exe_sha_ready;
static char g_host[128] = "unknown";
static const char g_version[] = "cnc-ddraw " VERSION_STRING ", " GIT_BRANCH " @ " GIT_COMMIT;
static const char* volatile g_terrain = "not checked yet";

static LC_RING g_log;
static LC_SEEN g_seen[LC_MAX_SEEN];
static volatile LONG g_seen_next;

static LC_MODULE g_snap[2][LC_MAX_MODULES];
static int g_snap_count[2];
static DWORD g_snap_tick[2];
static volatile LONG g_snap_active = -1;

static MINIDUMPWRITEDUMPPROC g_minidump;
static ISHUNGAPPWINDOWPROC g_is_hung;
static DWORD g_ddraw_base, g_storm_base;   /* modules whose data segments go in the minidump */

static LPTOP_LEVEL_EXCEPTION_FILTER volatile g_chain[LC_MAX_CHAIN];
static volatile LONG g_chain_count;
static volatile LONG g_chain_lock;
static volatile LONG g_replaced;           /* times something bypassed the hook and replaced ours */

typedef struct
{
    volatile LONG tid;
    BOOL active, reporting;
    int depth;
} LC_THREAD;

static LC_THREAD g_threads[LC_MAX_THREADS];
static volatile LONG g_report_lock;        /* thread id writing a report, or 0 */
static volatile LONG g_reports;
static DWORD g_last_code, g_last_address, g_last_thread;   /* the last exception reported */
static int g_hang_count;                   /* reporter thread only */
#define LC_MAX_HANGS 3                     /* per session */

/* Everything one report needs, static: a crash and a hang report can be in flight at once, on
 * different threads, so each has its own. */
typedef struct
{
    LC_REPORT r;
    LC_MODULE mods[LC_MAX_MODULES];
    LC_SEEN seen[LC_MAX_SEEN];
    char text[40000];
    char path[MAX_PATH];
    char name[64];
} LC_WORK;

static LC_WORK g_crash_work, g_hang_work;

static volatile LONG g_beats;
static volatile DWORD g_window_tid;

static LONG WINAPI lc_filter(EXCEPTION_POINTERS* ep);

/* ------------------------------------------------------------------------------------------- */
/* Small helpers (no heap, no C runtime)                                                       */
/* ------------------------------------------------------------------------------------------- */

static void lc_spin_lock(volatile LONG* lock)
{
    while (InterlockedCompareExchange(lock, 1, 0) != 0)
        Sleep(0);
}

static void lc_spin_unlock(volatile LONG* lock)
{
    InterlockedExchange(lock, 0);
}

static BOOL lc_readable_region(const MEMORY_BASIC_INFORMATION* m)
{
    return m->State == MEM_COMMIT && !(m->Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
        (m->Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}

/* Copies what is readable from the start of [src, src + n); returns the byte count. VirtualQuery
 * rather than a fault handler: mingw has no __try, and a second fault inside a crash handler is
 * the one thing it must not do. */
static DWORD lc_copy(void* dst, DWORD src, DWORD n)
{
    DWORD done = 0;

    while (done < n)
    {
        DWORD at = src + done;
        MEMORY_BASIC_INFORMATION m;

        if (at < src || !VirtualQuery((LPCVOID)at, &m, sizeof(m)) || !lc_readable_region(&m))
            break;

        DWORD end = (DWORD)m.BaseAddress + (DWORD)m.RegionSize;
        DWORD chunk = end > at ? end - at : 0;

        if (chunk == 0)
            break;

        if (chunk > n - done)
            chunk = n - done;

        for (DWORD i = 0; i < chunk; i++)
            ((BYTE*)dst)[done + i] = ((const BYTE*)at)[i];

        done += chunk;
    }

    return done;
}

static void lc_strcpy(char* dst, unsigned cap, const char* src)
{
    unsigned i = 0;

    for (; src && src[i] && i + 1 < cap; i++)
        dst[i] = src[i];

    if (cap)
        dst[i] = 0;
}

static const char* lc_basename(const char* path)
{
    const char* base = path;

    for (const char* p = path; *p; p++)
        if (*p == '\\' || *p == '/')
            base = p + 1;

    return base;
}

static BOOL lc_flag(const char* name)
{
    char path[MAX_PATH];
    _snprintf(path, sizeof(path), "%s%s", g_dir, name);
    path[sizeof(path) - 1] = 0;
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

/* ------------------------------------------------------------------------------------------- */
/* Modules                                                                                     */
/* ------------------------------------------------------------------------------------------- */

/* The reporter thread's snapshot, never taken on a crashing thread: Toolhelp takes the loader lock
 * and allocates. Written to the buffer not being read, then published. */
static void lc_snapshot_modules(void)
{
    LONG active = g_snap_active;
    int slot = active == 0 ? 1 : 0;
    int n = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);

    if (snap == INVALID_HANDLE_VALUE)
        return;

    MODULEENTRY32 me;
    me.dwSize = sizeof(me);

    for (BOOL ok = Module32First(snap, &me); ok && n < LC_MAX_MODULES; ok = Module32Next(snap, &me))
    {
        g_snap[slot][n].base = (DWORD)me.modBaseAddr;
        g_snap[slot][n].size = me.modBaseSize;
        lc_strcpy(g_snap[slot][n].name, LC_MODULE_NAME, me.szModule);

        if (_stricmp(me.szModule, "storm.dll") == 0)
            g_storm_base = (DWORD)me.modBaseAddr;

        n++;
    }

    CloseHandle(snap);
    lc_sort_modules(g_snap[slot], n);
    g_snap_count[slot] = n;
    g_snap_tick[slot] = GetTickCount();
    InterlockedExchange(&g_snap_active, slot);
}

/* An address in an image the snapshot has not seen yet: its allocation base is the module, and
 * the PE export directory usually carries its name. Lock-free, heap-free. */
static void lc_add_image(LC_WORK* w, DWORD addr)
{
    LC_REPORT* r = &w->r;
    MEMORY_BASIC_INFORMATION m;

    if (r->mod_count >= LC_MAX_MODULES || lc_find_module(w->mods, r->mod_count, addr) ||
        !VirtualQuery((LPCVOID)addr, &m, sizeof(m)) || m.Type != MEM_IMAGE || !m.AllocationBase)
        return;

    DWORD base = (DWORD)m.AllocationBase;
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS32 nt;

    if (lc_copy(&dos, base, sizeof(dos)) != sizeof(dos) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        lc_copy(&nt, base + (DWORD)dos.e_lfanew, sizeof(nt)) != sizeof(nt) || nt.Signature != IMAGE_NT_SIGNATURE)
        return;

    LC_MODULE* mod = &w->mods[r->mod_count];
    mod->base = base;
    mod->size = nt.OptionalHeader.SizeOfImage;
    lc_strcpy(mod->name, LC_MODULE_NAME, "(loaded since the snapshot)");

    IMAGE_DATA_DIRECTORY* dir = &nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    IMAGE_EXPORT_DIRECTORY exp;

    if (nt.OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_EXPORT && dir->VirtualAddress &&
        lc_copy(&exp, base + dir->VirtualAddress, sizeof(exp)) == sizeof(exp) && exp.Name)
    {
        char name[LC_MODULE_NAME];
        DWORD got = lc_copy(name, base + exp.Name, sizeof(name) - 1);
        name[got] = 0;

        if (got && name[0])
            lc_strcpy(mod->name, LC_MODULE_NAME, name);
    }

    r->mod_count++;
    lc_sort_modules(w->mods, r->mod_count);
}

static void lc_fill_modules(LC_WORK* w)
{
    LC_REPORT* r = &w->r;
    LONG slot = g_snap_active;

    r->mods = w->mods;
    r->mod_count = 0;

    if (slot >= 0)
    {
        int n = g_snap_count[slot];

        for (int i = 0; i < n && i < LC_MAX_MODULES; i++)
            w->mods[i] = g_snap[slot][i];

        r->mod_count = n;
        r->mods_age_ms = GetTickCount() - g_snap_tick[slot];
    }

    if (r->has_exception)
        lc_add_image(w, r->address);

    if (r->has_regs)
    {
        lc_add_image(w, r->regs.eip);

        for (int i = 0; i < r->stack_count; i++)
            lc_add_image(w, r->stack[i]);
    }
}

/* ------------------------------------------------------------------------------------------- */
/* Writing a report                                                                            */
/* ------------------------------------------------------------------------------------------- */

static void lc_fill_common(LC_WORK* w, const char* kind)
{
    LC_REPORT* r = &w->r;
    SYSTEMTIME t;
    GetLocalTime(&t);

    r->kind = kind;
    r->when[0] = t.wYear;
    r->when[1] = t.wMonth;
    r->when[2] = t.wDay;
    r->when[3] = t.wHour;
    r->when[4] = t.wMinute;
    r->when[5] = t.wSecond;
    r->uptime_ms = GetTickCount() - g_start_tick;
    r->exe_path = g_exe_path;
    r->exe_size = g_exe_size;
    r->exe_timestamp = g_exe_timestamp;
    r->exe_checksum = g_exe_checksum;
    r->exe_sha256 = g_exe_sha_ready ? g_exe_sha : NULL;
    r->ddraw_version = g_version;
    r->host = g_host;
    r->pack = lomhd_pack_state();
    r->terrain = g_terrain;
    r->log = &g_log;
    r->log_lines = LC_LOG_LINES;

    /* The newest few, oldest first. */
    LONG next = g_seen_next;
    LONG have = next < LC_MAX_SEEN ? next : LC_MAX_SEEN;

    for (LONG i = 0; i < have; i++)
        w->seen[i] = g_seen[(DWORD)(next - have + i) % LC_MAX_SEEN];

    r->seen = w->seen;
    r->seen_count = (int)have;
    r->seen_dropped = (DWORD)(next - have);
}

static void lc_fill_regs(LC_WORK* w, const CONTEXT* c)
{
    LC_REPORT* r = &w->r;
    r->has_regs = TRUE;
    r->regs.eax = c->Eax;
    r->regs.ebx = c->Ebx;
    r->regs.ecx = c->Ecx;
    r->regs.edx = c->Edx;
    r->regs.esi = c->Esi;
    r->regs.edi = c->Edi;
    r->regs.ebp = c->Ebp;
    r->regs.esp = c->Esp;
    r->regs.eip = c->Eip;
    r->regs.eflags = c->EFlags;
    r->regs.cs = c->SegCs;
    r->regs.ds = c->SegDs;
    r->regs.es = c->SegEs;
    r->regs.fs = c->SegFs;
    r->regs.gs = c->SegGs;
    r->regs.ss = c->SegSs;
    r->stack_addr = c->Esp;
    r->stack_count = (int)(lc_copy(r->stack, c->Esp, sizeof(r->stack)) / 4);
    r->code_count = (int)lc_copy(r->code_bytes, c->Eip, sizeof(r->code_bytes));
}

/* Opens lomhd_<kind>_<time>.txt, never overwriting an older report. */
static HANDLE lc_open(LC_WORK* w, int* attempt_out)
{
    for (int attempt = 1; attempt <= 9; attempt++)
    {
        lc_report_name(w->name, sizeof(w->name), w->r.kind, w->r.when, attempt, ".txt");
        LC_TEXT p;
        lc_text_init(&p, w->path, sizeof(w->path));
        lc_puts(&p, g_dir);
        lc_puts(&p, w->name);

        if (p.dropped)
            return INVALID_HANDLE_VALUE;

        HANDLE f = CreateFileA(w->path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL, NULL);

        if (f != INVALID_HANDLE_VALUE || GetLastError() != ERROR_FILE_EXISTS)
        {
            *attempt_out = attempt;
            return f;
        }
    }

    return INVALID_HANDLE_VALUE;
}

static void lc_write_all(HANDLE f, const char* s, DWORD n)
{
    DWORD written = 0;
    WriteFile(f, s, n, &written, NULL);
}

/* Data segments only for the game, Storm and this DLL: every system DLL's as well would take the
 * dump from a few MB to tens. */
static BOOL CALLBACK lc_dump_callback(PVOID param, const PMINIDUMP_CALLBACK_INPUT in, PMINIDUMP_CALLBACK_OUTPUT out)
{
    (void)param;

    if (in && out && in->CallbackType == ModuleCallback)
    {
        DWORD base = (DWORD)in->Module.BaseOfImage;

        if (base != g_exe_base && base != g_ddraw_base && base != g_storm_base)
            out->ModuleWriteFlags &= ~ModuleWriteDataSeg;
    }

    return TRUE;
}

static void lc_write_dump(LC_WORK* w, EXCEPTION_POINTERS* ep, int attempt, HANDLE txt)
{
    LC_TEXT t;
    char line[MAX_PATH + 96];
    lc_text_init(&t, line, sizeof(line));
    lc_puts(&t, "Minidump:   ");

    if (!g_minidump || g_detached || (ep && ep->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW))
    {
        lc_puts(&t, !g_minidump ? "not written (dbghelp.dll unavailable)" : g_detached ?
            "not written (the process was already shutting down)" :
            "not written (stack overflow: too little stack left to write one safely)");
        lc_nl(&t);
        lc_write_all(txt, line, t.len);
        return;
    }

    char name[64], path[MAX_PATH];
    lc_report_name(name, sizeof(name), w->r.kind, w->r.when, attempt, ".dmp");
    LC_TEXT p;
    lc_text_init(&p, path, sizeof(path));
    lc_puts(&p, g_dir);
    lc_puts(&p, name);

    HANDLE f = p.dropped ? INVALID_HANDLE_VALUE :
        CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);

    if (f == INVALID_HANDLE_VALUE)
    {
        lc_puts(&t, "not written (could not create ");
        lc_puts(&t, name);
        lc_puts(&t, ")");
        lc_nl(&t);
        lc_write_all(txt, line, t.len);
        return;
    }

    MINIDUMP_EXCEPTION_INFORMATION info;
    info.ThreadId = GetCurrentThreadId();
    info.ExceptionPointers = ep;
    info.ClientPointers = FALSE;

    MINIDUMP_CALLBACK_INFORMATION cb;
    cb.CallbackRoutine = (MINIDUMP_CALLBACK_ROUTINE)lc_dump_callback;
    cb.CallbackParam = NULL;

    BOOL ok = g_minidump(GetCurrentProcess(), GetCurrentProcessId(), f,
        (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithDataSegs), ep ? &info : NULL, NULL, &cb);
    DWORD error = ok ? 0 : GetLastError();
    DWORD size = GetFileSize(f, NULL);
    CloseHandle(f);

    lc_puts(&t, name);

    if (ok)
    {
        lc_puts(&t, " (");
        lc_dec(&t, size >> 10);
        lc_puts(&t, " KB)");
    }
    else
    {
        lc_puts(&t, " FAILED, error ");
        lc_dec(&t, error);
    }

    lc_nl(&t);
    lc_write_all(txt, line, t.len);
}

static void lc_note(const char* what, const LC_WORK* w, BOOL written)
{
    char line[160];
    _snprintf(line, sizeof(line), "%s: %s %s", what, written ? "report written to" : "could not write",
        w->name[0] ? w->name : "a report (game folder not writable?)");
    line[sizeof(line) - 1] = 0;
    lomhd_log(line);
}

static void lc_report_crash(EXCEPTION_POINTERS* ep)
{
    LC_WORK* w = &g_crash_work;
    LC_REPORT* r = &w->r;
    EXCEPTION_RECORD* e = ep ? ep->ExceptionRecord : NULL;

    for (unsigned i = 0; i < sizeof(*r); i++)
        ((BYTE*)r)[i] = 0;

    w->name[0] = 0;
    lc_fill_common(w, "crash");
    r->thread_id = GetCurrentThreadId();
    r->window_thread = r->thread_id == g_window_tid;

    if (e)
    {
        r->has_exception = TRUE;
        r->code = e->ExceptionCode;
        r->address = (DWORD)e->ExceptionAddress;
        r->nparams = e->NumberParameters < 4 ? e->NumberParameters : 4;

        for (DWORD i = 0; i < r->nparams; i++)
            r->params[i] = (DWORD)e->ExceptionInformation[i];
    }

    if (ep && ep->ContextRecord)
        lc_fill_regs(w, ep->ContextRecord);

    lc_fill_modules(w);

    LC_TEXT note;
    char filters[96];
    lc_text_init(&note, filters, sizeof(filters));
    lc_dec(&note, (DWORD)g_chain_count);
    lc_puts(&note, " level(s) chained after this report; replaced and re-asserted ");
    lc_dec(&note, (DWORD)g_replaced);
    lc_puts(&note, " time(s)");
    r->filter_note = filters;

    int attempt = 1;
    HANDLE f = lc_open(w, &attempt);

    if (f == INVALID_HANDLE_VALUE)
    {
        w->name[0] = 0;
        lc_note("crash", w, FALSE);
        return;
    }

    LC_TEXT t;
    lc_text_init(&t, w->text, sizeof(w->text));
    lc_format_report(r, &t);
    lc_nl(&t);
    lc_write_all(f, w->text, t.len);
    FlushFileBuffers(f);

    /* After the text is safely down: a minidump is the step most likely to fail or stall. */
    lc_write_dump(w, ep, attempt, f);
    FlushFileBuffers(f);
    CloseHandle(f);
    lc_note("crash", w, TRUE);
}

/* ------------------------------------------------------------------------------------------- */
/* The filter                                                                                  */
/* ------------------------------------------------------------------------------------------- */

static LC_THREAD* lc_thread_find(DWORD me)
{
    for (int i = 0; i < LC_MAX_THREADS; i++)
        if ((DWORD)g_threads[i].tid == me)
            return &g_threads[i];

    return NULL;
}

static LC_THREAD* lc_thread_claim(DWORD me)
{
    for (int i = 0; i < LC_MAX_THREADS; i++)
    {
        if (InterlockedCompareExchange(&g_threads[i].tid, (LONG)me, 0) == 0)
        {
            g_threads[i].active = TRUE;
            g_threads[i].reporting = FALSE;
            g_threads[i].depth = 0;
            return &g_threads[i];
        }
    }

    return NULL;
}

/* Depth 0 is the newest game filter; each time a filter chains back into ours we go one down. */
static LONG lc_call_chain(EXCEPTION_POINTERS* ep, int depth)
{
    LONG idx = g_chain_count - 1 - depth;

    if (idx < 0)
        return EXCEPTION_CONTINUE_SEARCH;

    LPTOP_LEVEL_EXCEPTION_FILTER f = g_chain[idx];

    if (!f || f == lc_filter)
        return EXCEPTION_CONTINUE_SEARCH;

    return f(ep);
}

static LONG WINAPI lc_filter(EXCEPTION_POINTERS* ep)
{
    DWORD me = GetCurrentThreadId();
    LC_THREAD* th = lc_thread_find(me);

    if (th)
    {
        /* Re-entry on this thread. A fault inside our own report: give up on it quietly. A filter
         * we called chaining to its predecessor, which it believes is us: go one level down. */
        if (th->reporting)
            return EXCEPTION_CONTINUE_SEARCH;

        return lc_call_chain(ep, ++th->depth);
    }

    if (!g_installed)
        return lc_call_chain(ep, 0);

    th = lc_thread_claim(me);

    if (!th)
        return lc_call_chain(ep, 0);

    /* One report at a time. Another thread's report in progress is waited for (up to 30 s), so a
     * second crashing thread cannot end the process while the first is still writing. */
    BOOL mine = FALSE;

    for (int i = 0; i < 300; i++)
    {
        if (InterlockedCompareExchange(&g_report_lock, (LONG)me, 0) == 0)
        {
            mine = TRUE;
            break;
        }

        Sleep(100);
    }

    /* One fault reaches the filter twice under Wine when winedbg attaches: it re-delivers the
     * exception (same thread, same address; observed 2026-09-27 on Wine 10). One report each. */
    const EXCEPTION_RECORD* e = ep ? ep->ExceptionRecord : NULL;
    BOOL repeat = e && e->ExceptionCode == g_last_code && (DWORD)e->ExceptionAddress == g_last_address &&
        me == g_last_thread;

    if (mine)
    {
        if (!repeat && InterlockedIncrement(&g_reports) <= LC_MAX_REPORTS)
        {
            if (e)
            {
                g_last_code = e->ExceptionCode;
                g_last_address = (DWORD)e->ExceptionAddress;
                g_last_thread = me;
            }

            th->reporting = TRUE;
            lc_report_crash(ep);
            th->reporting = FALSE;
        }

        InterlockedExchange(&g_report_lock, 0);
    }

    LONG ret = lc_call_chain(ep, 0);

    th->active = FALSE;
    InterlockedExchange(&th->tid, 0);
    return ret;
}

LPTOP_LEVEL_EXCEPTION_FILTER lomhd_crash_set_filter(LPTOP_LEVEL_EXCEPTION_FILTER filter)
{
    if (!g_installed)
        return real_SetUnhandledExceptionFilter(filter);

    lc_spin_lock(&g_chain_lock);
    LONG top = g_chain_count - 1;
    LPTOP_LEVEL_EXCEPTION_FILTER old = g_chain[top];

    /* A filter handing ours back (it got ours from a real call that bypassed the hook) means
     * "remove me": the level below is the one to restore, which is what top-level NULL would miss. */
    if (filter == lc_filter && top > 0)
        g_chain_count--;
    else if (filter != lc_filter)
        g_chain[top] = filter;

    lc_spin_unlock(&g_chain_lock);
    return old == lc_filter ? NULL : old;
}

static void lc_reassert(void)
{
    LPTOP_LEVEL_EXCEPTION_FILTER current = real_SetUnhandledExceptionFilter(lc_filter);

    if (current == lc_filter)
        return;

    lc_spin_lock(&g_chain_lock);

    if (g_chain_count < LC_MAX_CHAIN)
        g_chain[g_chain_count++] = current;
    else
        g_chain[LC_MAX_CHAIN - 1] = current;

    lc_spin_unlock(&g_chain_lock);

    if (InterlockedIncrement(&g_replaced) <= 3)
    {
        char line[160];
        _snprintf(line, sizeof(line), "crash reports: another filter (%08lx) replaced ours without the hook; "
            "ours runs first again, then that one", (unsigned long)(DWORD)current);
        line[sizeof(line) - 1] = 0;
        lomhd_log(line);
    }
}

/* ------------------------------------------------------------------------------------------- */
/* First-chance breadcrumbs                                                                    */
/* ------------------------------------------------------------------------------------------- */

static LONG WINAPI lc_vectored(EXCEPTION_POINTERS* ep)
{
    const EXCEPTION_RECORD* e = ep ? ep->ExceptionRecord : NULL;

    /* Errors only (0xC...): not breakpoints, debug prints, thread names or C++ throws. */
    if (e && (e->ExceptionCode & 0xF0000000) == 0xC0000000)
    {
        LONG i = InterlockedIncrement(&g_seen_next) - 1;
        LC_SEEN* s = &g_seen[(DWORD)i % LC_MAX_SEEN];
        s->code = e->ExceptionCode;
        s->address = (DWORD)e->ExceptionAddress;
        s->access = e->NumberParameters >= 2 ? (DWORD)e->ExceptionInformation[0] : 0;
        s->target = e->NumberParameters >= 2 ? (DWORD)e->ExceptionInformation[1] : 0;
        s->thread = GetCurrentThreadId();
        s->tick = GetTickCount() - g_start_tick;
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

/* In lomhd.log, the first few first-chance exceptions inside the game, Storm or this DLL: if the
 * game then closes or hangs without a crash report (something caught the exception and exited),
 * this is where it started. Capped, and never a report. */
static void lc_log_seen(LONG* logged_upto, int* logged)
{
    LONG next = g_seen_next;

    if (next - *logged_upto > LC_MAX_SEEN)
        *logged_upto = next - LC_MAX_SEEN;

    for (; *logged_upto < next; (*logged_upto)++)
    {
        LC_SEEN s = g_seen[(DWORD)*logged_upto % LC_MAX_SEEN];
        LONG slot = g_snap_active;
        const LC_MODULE* m = slot >= 0 ? lc_find_module(g_snap[slot], g_snap_count[slot], s.address) : NULL;
        const char* kind = lc_module_kind(m ? m->name : NULL);

        if (*logged >= LC_SEEN_LOG_MAX || !m || strcmp(kind, "other") == 0)
            continue;

        char line[200];
        _snprintf(line, sizeof(line), "first-chance %08lx at %s+0x%08lx (target %08lx, thread %lu) -- "
            "may have been handled; if the game closes or hangs next, it started here",
            (unsigned long)s.code, m->name, (unsigned long)(s.address - m->base), (unsigned long)s.target,
            (unsigned long)s.thread);
        line[sizeof(line) - 1] = 0;
        lomhd_log(line);

        if (++*logged == LC_SEEN_LOG_MAX)
            lomhd_log("first-chance: no more of these this session");
    }
}

/* ------------------------------------------------------------------------------------------- */
/* Hang watchdog                                                                               */
/* ------------------------------------------------------------------------------------------- */

void lomhd_crash_beat(void)
{
    if (GetCurrentThreadId() == g_window_tid)
        InterlockedIncrement(&g_beats);
}

static void lc_report_hang(HWND hwnd, DWORD tid, DWORD silent_ms)
{
    LC_WORK* w = &g_hang_work;
    LC_REPORT* r = &w->r;
    memset(r, 0, sizeof(*r));
    w->name[0] = 0;
    lc_fill_common(w, "hang");
    r->thread_id = tid;
    r->window_thread = TRUE;
    r->hang_ms = silent_ms;

    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);

    if (h)
    {
        /* Suspended only for GetThreadContext and a copy of its stack: no allocation, no lock, no
         * file while it is stopped -- it may hold the heap or loader lock. */
        if (SuspendThread(h) != (DWORD)-1)
        {
            CONTEXT c;
            memset(&c, 0, sizeof(c));
            c.ContextFlags = CONTEXT_FULL;

            if (GetThreadContext(h, &c))
                lc_fill_regs(w, &c);

            ResumeThread(h);
        }

        CloseHandle(h);
    }

    (void)hwnd;
    lc_fill_modules(w);

    int attempt = 1;
    HANDLE f = lc_open(w, &attempt);

    if (f == INVALID_HANDLE_VALUE)
    {
        w->name[0] = 0;
        lc_note("hang", w, FALSE);
        return;
    }

    LC_TEXT t;
    lc_text_init(&t, w->text, sizeof(w->text));
    lc_format_report(r, &t);

    if (!r->has_regs)
        lc_puts(&t, "\r\nThe window thread's registers could not be read (SuspendThread/GetThreadContext failed).\r\n");

    lc_write_all(f, w->text, t.len);
    CloseHandle(f);
    lc_note("hang", w, TRUE);
}

typedef struct
{
    LONG beats;
    DWORD quiet_since;          /* the last beat, or the last moment the window was not in front */
    BOOL reported;              /* this hang is reported; re-armed by the next beat */
    HWND hwnd;
} LC_WATCH;

static void lc_watch(LC_WATCH* s, DWORD now)
{
    HWND hwnd = g_ddraw.hwnd;
    DWORD pid = 0;
    DWORD tid = hwnd && IsWindow(hwnd) ? GetWindowThreadProcessId(hwnd, &pid) : 0;

    if (!tid || pid != GetCurrentProcessId())
    {
        s->quiet_since = now;
        s->hwnd = NULL;
        return;
    }

    if (hwnd != s->hwnd)
    {
        s->hwnd = hwnd;
        s->quiet_since = now;
    }

    InterlockedExchange((volatile LONG*)&g_window_tid, (LONG)tid);
    LONG beats = g_beats;

    if (beats != s->beats)
    {
        s->beats = beats;
        s->quiet_since = now;
        s->reported = FALSE;
        return;
    }

    /* Only time in front counts: a minimized or background game is allowed to sit still. */
    if (GetForegroundWindow() != hwnd || IsIconic(hwnd))
    {
        s->quiet_since = now;
        return;
    }

    if (s->reported || now - s->quiet_since < LC_HANG_MS)
        return;

    /* No DirectDraw call from the window thread for 20 s. A game idle in GetMessage (a menu
     * waiting for input) is silent too, so it is only a hang if Windows/Wine agree the window is
     * not pumping messages. If it is, start counting again. */
    BOOL hung;

    if (g_is_hung)
        hung = g_is_hung(hwnd);
    else
    {
        DWORD_PTR result;
        hung = !SendMessageTimeoutA(hwnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000, &result);
    }

    if (!hung)
    {
        s->quiet_since = now;
        return;
    }

    s->reported = TRUE;

    if (++g_hang_count <= LC_MAX_HANGS)
        lc_report_hang(hwnd, tid, now - s->quiet_since);
}

/* ------------------------------------------------------------------------------------------- */
/* Reporter thread                                                                             */
/* ------------------------------------------------------------------------------------------- */

static void lc_hash_exe(void)
{
    HANDLE f = CreateFileA(g_exe_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);

    if (f == INVALID_HANDLE_VALUE)
        return;

    BYTE* buf = malloc(1 << 16);
    LC_SHA256 sha;
    lc_sha256_init(&sha);
    DWORD got = 0;
    BOOL ok = buf != NULL;

    while (ok && !g_stop && (ok = ReadFile(f, buf, 1 << 16, &got, NULL)) && got)
        lc_sha256_update(&sha, buf, got);

    free(buf);
    CloseHandle(f);

    if (ok && !g_stop)
    {
        lc_sha256_hex(&sha, g_exe_sha);
        InterlockedExchange(&g_exe_sha_ready, 1);
    }
}

static void lc_describe_host(void)
{
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    const char* (__cdecl * wine_version)(void) = (void*)real_GetProcAddress(ntdll, "wine_get_version");
    void (__cdecl * wine_host)(const char**, const char**) = (void*)real_GetProcAddress(ntdll, "wine_get_host_version");
    LONG(WINAPI * rtl_version)(RTL_OSVERSIONINFOW*) = (void*)real_GetProcAddress(ntdll, "RtlGetVersion");

    RTL_OSVERSIONINFOW v;
    memset(&v, 0, sizeof(v));
    v.dwOSVersionInfoSize = sizeof(v);

    if (rtl_version)
        rtl_version(&v);

    if (wine_version)
    {
        const char* sys = NULL;
        const char* rel = NULL;

        if (wine_host)
            wine_host(&sys, &rel);

        _snprintf(g_host, sizeof(g_host), "Wine %s on %s %s, as Windows %lu.%lu.%lu", wine_version(),
            sys ? sys : "?", rel ? rel : "?", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
    }
    else
    {
        _snprintf(g_host, sizeof(g_host), "Windows %lu.%lu.%lu", v.dwMajorVersion, v.dwMinorVersion,
            v.dwBuildNumber);
    }

    g_host[sizeof(g_host) - 1] = 0;
}

static DWORD WINAPI lc_reporter(LPVOID unused)
{
    (void)unused;

    /* Everything the crash path calls, resolved here and not there: a crashing thread may hold
     * the loader lock that LoadLibrary needs. */
    HMODULE dbghelp = real_LoadLibraryA("dbghelp.dll");

    if (dbghelp)
        g_minidump = (MINIDUMPWRITEDUMPPROC)real_GetProcAddress(dbghelp, "MiniDumpWriteDump");

    g_is_hung = (ISHUNGAPPWINDOWPROC)real_GetProcAddress(GetModuleHandleA("user32.dll"), "IsHungAppWindow");
    lc_describe_host();
    lc_snapshot_modules();

    char line[200];
    _snprintf(line, sizeof(line), "crash reports: on (hang reports %s); %s", g_hang_reports ? "on" : "off",
        g_minidump ? "minidumps on" : "no dbghelp.dll, text reports only");
    line[sizeof(line) - 1] = 0;
    lomhd_log(line);

    lc_hash_exe();
    g_terrain = lomhd_terrain_active() ? (lomhd_files_on() ? "hybrid exe, art from lomhd_terrain" :
        "hybrid exe, art from pic.mpq") : "off (not the hybrid exe)";

    LC_WATCH watch;
    memset(&watch, 0, sizeof(watch));
    watch.quiet_since = GetTickCount();
    DWORD last_snap = GetTickCount(), last_assert = 0;
    LONG seen_upto = 0;
    int seen_logged = 0;

    while (!g_stop)
    {
        Sleep(250);
        DWORD now = GetTickCount();

        if (now - last_snap >= 2000)
        {
            lc_snapshot_modules();
            last_snap = now;
        }

        if (now - last_assert >= 1000)
        {
            lc_reassert();
            last_assert = now;
        }

        lc_log_seen(&seen_upto, &seen_logged);

        if (g_hang_reports)
            lc_watch(&watch, now);
    }

    return 0;
}

/* ------------------------------------------------------------------------------------------- */
/* Install and exit                                                                            */
/* ------------------------------------------------------------------------------------------- */

void lomhd_crash_log_line(const char* line)
{
    lc_ring_push(&g_log, line);
}

void lomhd_crash_install(void)
{
    g_start_tick = GetTickCount();
    g_ddraw_base = (DWORD)g_ddraw_module;
    g_exe_base = (DWORD)GetModuleHandleA(NULL);

    DWORD n = GetModuleFileNameA(NULL, g_exe_path, sizeof(g_exe_path));

    if (!n || n >= sizeof(g_exe_path))
        return;

    lc_strcpy(g_dir, sizeof(g_dir), g_exe_path);
    g_dir[lc_basename(g_exe_path) - g_exe_path] = 0;

    /* Lords of Magic only, like the rest of the fork, unless asked for (the test program is not
     * lomse.exe). And off entirely with lomhd_no_crash_reports beside the game. */
    BOOL lom = strcmp(lc_module_kind(lc_basename(g_exe_path)), "lomse") == 0;

    if ((!lom && !lc_flag("lomhd_crash_reports")) || lc_flag("lomhd_no_crash_reports"))
        return;

    g_hang_reports = !lc_flag("lomhd_no_hang_reports");

    WIN32_FILE_ATTRIBUTE_DATA a;

    if (GetFileAttributesExA(g_exe_path, GetFileExInfoStandard, &a))
        g_exe_size = a.nFileSizeLow;

    /* The exe's own headers, from memory: already mapped, so no file read here. */
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)g_exe_base;

    if (dos && dos->e_magic == IMAGE_DOS_SIGNATURE)
    {
        const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*)(g_exe_base + (DWORD)dos->e_lfanew);

        if (nt->Signature == IMAGE_NT_SIGNATURE)
        {
            g_exe_timestamp = nt->FileHeader.TimeDateStamp;
            g_exe_checksum = nt->OptionalHeader.CheckSum;
        }
    }

    g_chain[0] = real_SetUnhandledExceptionFilter(lc_filter);
    g_chain_count = 1;
    InterlockedExchange(&g_installed, 1);

    PVOID(WINAPI * add_handler)(ULONG, PVECTORED_EXCEPTION_HANDLER) =
        (void*)real_GetProcAddress(GetModuleHandleA("kernel32.dll"), "AddVectoredExceptionHandler");

    if (add_handler)
        add_handler(0, (PVECTORED_EXCEPTION_HANDLER)lc_vectored);

    /* Created under the loader lock, runs once DllMain returns. */
    HANDLE t = CreateThread(NULL, 0, lc_reporter, NULL, 0, NULL);

    if (t)
        CloseHandle(t);
}

void lomhd_crash_exit(BOOL process_exit)
{
    if (!g_installed)
        return;

    InterlockedExchange(&g_detached, 1);

    if (process_exit)
        return;             /* the code stays mapped: a crash during shutdown is still reported */

    /* FreeLibrary: this code is about to go. Hand the process its filter back and stop. */
    InterlockedExchange(&g_stop, 1);
    InterlockedExchange(&g_installed, 0);
    real_SetUnhandledExceptionFilter(g_chain[g_chain_count - 1] == lc_filter ? NULL : g_chain[g_chain_count - 1]);
}
