#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dd.h"
#include "hook.h"
#include "git.h"
#ifndef LOMHD_VERSION /* git.h from the Makefile has it; the MSVC project's does not */
#define LOMHD_VERSION "dev"
#endif
#include "version.h"
#include "config.h"
#include "versionhelpers.h"
#include "dllmain.h"
#include "lomhd.h"
#include "lomhd_crash.h"

/* Crash and hang reports for playtesters: a readable lomhd_crash_*.txt and a minidump beside the
 * game when it dies of an exception nothing handled, and a lomhd_hang_*.txt when its window stops
 * responding while in front. The text is the part meant to be read from a bug report; see
 * src/lomhd_crash_core.c for its layout.
 *
 * WHICH EXCEPTIONS. Only unhandled ones: this is the top-level (SetUnhandledExceptionFilter)
 * filter, which Windows and Wine call after every frame-based handler has declined -- never for a
 * first-chance exception the game or a system DLL catches (IsBadReadPtr raises and catches access
 * violations all day). A vectored handler also records the last few serious first-chance
 * exceptions, but only as context inside a report and as a capped note in lomhd.log.
 *
 * THE FILTER CHAIN. There is one top-level filter per process, and a game or Storm may set its
 * own. Ours is installed as one of eight entry points, one per chain level (lc_stub_0..7); the
 * bookkeeping is pure and tested natively (lc_chain_* in lomhd_crash_core.c).
 *   1. cnc-ddraw patches SetUnhandledExceptionFilter in the import tables of every module in the
 *      game folder (hook.c; upstream did it for debug builds, this fork for all gcc builds). Such a
 *      call lands in lomhd_crash_set_filter: the process-wide filter stays ours, theirs becomes the
 *      newest entry, and they get back what the real call would have returned, so a filter that
 *      chains to its predecessor still reaches it.
 *   2. A call that bypasses the patched imports (GetProcAddress, a module outside the game folder)
 *      really replaces ours. Once a second the reporter thread puts ours back and records what it
 *      found. The newcomer saved "ours at level j" as its predecessor, so when it chains back we
 *      continue one level down; and when it uninstalls by restoring that, the reporter sees ours at
 *      level j and drops it -- a filter that uninstalled itself is never called. A filter that is
 *      already in the chain is never pushed twice. Between a replacement and the next re-assert (up
 *      to a second) a crash goes to the newcomer alone: the one gap.
 * After the report we call the chain and return its answer; with nothing to chain to,
 * EXCEPTION_CONTINUE_SEARCH -- so Windows Error Reporting or Wine's crash dialog and winedbg still
 * do exactly what they did before.
 *
 * THE CRASHING THREAD does almost nothing: it copies its exception record and context into a
 * request slot, signals the helper thread, waits until one deadline taken on entry (ten seconds),
 * and runs the chain. It never allocates, formats, logs or opens a file; the only lock it takes is
 * a spinlock held for a few instructions (the duplicate check). Its frame is small (measured with
 * -fstack-usage; see the README), so a stack overflow is reported too. The helper, created at
 * startup, writes the report from the request's copies. It uses no heap and no C runtime for the
 * report itself (lomhd_crash_core.c formats into static buffers), but it does call into Windows:
 * CreateFile and WriteFile, VirtualQuery, ReadProcessMemory, and dbghelp's MiniDumpWriteDump, which
 * allocates and may need the loader lock. If the crashing thread holds a lock the helper then
 * needs, the helper blocks; at its deadline the crashing thread abandons the request and carries on
 * to the chain, so the game still ends as it would have. An abandoned request's slot stays the
 * helper's until it finishes, so a later crash never overwrites what it is reading. The text is
 * written and flushed before the minidump is attempted.
 *
 * OUR THREADS never keep the process alive: the helper and the reporter wait on the game's main
 * thread and exit when it ends -- as it does when a game filter ends just that thread.
 *
 * MODULES are listed by walking the address space with VirtualQuery and reading each image's PE
 * headers (the name from its export directory). No Toolhelp, so nothing here ever takes the loader
 * lock -- a lock the crashing thread, or a thread the minidump has suspended, may hold. */

#define LC_MAX_MODULES 160
#define LC_MAX_THREADS 8
#define LC_MAX_REPORTS 4                   /* per session */
#define LC_MAX_HANGS 3                     /* per session */
#define LC_MAX_HANG_REFUNDS 6              /* Wine: recovered hangs given back to the cap, per session */
#define LC_HANG_MS 20000
#define LC_HANDOFF_MS 10000                /* the crashing thread's wait for the helper */
#define LC_QUIET_START_MS 1500             /* the reporter thread's idle start */
#define LC_LOG_LINES 30
#define LC_SEEN_LOG_MAX 10
#define LC_CHAIN_LOG_MAX 6

typedef BOOL(WINAPI* MINIDUMPWRITEDUMPPROC)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
    PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
typedef BOOL(WINAPI* ISHUNGAPPWINDOWPROC)(HWND);
typedef HWND(WINAPI* HUNGWINDOWFROMGHOSTPROC)(HWND);

static volatile LONG g_installed;
static volatile LONG g_detached;           /* DLL_PROCESS_DETACH seen: the helper is gone */
static BOOL g_hang_reports;
static DWORD g_start_tick;

static char g_dir[MAX_PATH];               /* game folder, with a trailing backslash */
static char g_exe_path[MAX_PATH];
static char g_exe_name[LC_MODULE_NAME];
static DWORD g_exe_size, g_exe_timestamp, g_exe_checksum;
static DWORD g_exe_base;
static char g_exe_sha[65];
static volatile LONG g_exe_sha_ready;
static char g_host[128] = "unknown";
static const char g_version[] = "cnc-ddraw " VERSION_STRING ", lomhd " LOMHD_VERSION ", git " GIT_COMMIT;
static const char* volatile g_terrain = "not checked yet";

static LC_RING g_log;
static LC_SEEN g_seen[LC_MAX_SEEN];
static volatile LONG g_seen_next;
static LONG g_seen_logged_upto;            /* reporter thread, then DLL_PROCESS_DETACH */
static int g_seen_logged;

static MINIDUMPWRITEDUMPPROC g_minidump;
static ISHUNGAPPWINDOWPROC g_is_hung;
static HUNGWINDOWFROMGHOSTPROC g_ghost_owner;
static DWORD g_ddraw_base, g_storm_base;   /* modules whose data segments go in the minidump */

/* The chain. Changed under g_chain_lock (patched-import calls and the reporter's re-assert); read
 * without it by the crash path. */
static LC_CHAIN g_chain;
static volatile LONG g_chain_lock;
static void* g_stubs[LC_MAX_CHAIN];
static int g_chain_logged;

typedef struct
{
    volatile LONG tid;
    DWORD visited;                         /* chain levels this thread has entered */
} LC_THREAD;

static LC_THREAD g_threads[LC_MAX_THREADS];
static LC_ADMIT g_admit;                   /* the report count and the duplicate reference */
static volatile LONG g_admit_lock;         /* held for a few instructions, never across a wait */
static int g_hang_count;                   /* reporter thread only */
static int g_hang_refunds;                 /* reporter thread only */

/* The handoff to the helper thread: the mailbox (lomhd_crash_core.c) and, per slot, the crashing
 * thread's exception copied out of its stack -- so nothing the helper reads can vanish when a
 * thread that stopped waiting unwinds or exits. */
typedef struct
{
    EXCEPTION_RECORD record;
    CONTEXT context;
    EXCEPTION_POINTERS pointers;           /* pointing at the two above */
    DWORD tid;
} LC_REQUEST;

static LC_MAILBOX g_mail;
static LC_REQUEST g_request_data[LC_SLOTS];
static HANDLE g_helper, g_request, g_done;
static DWORD g_helper_tid;
static volatile LONG g_resume_gen;         /* a report whose chain resumed execution */

/* The game's main thread (the one that loaded this DLL). Our two threads exit when it does, so
 * they can never be what keeps a crashed game's process alive. */
static HANDLE g_main;

/* Everything one report needs, static: a crash and a hang report can be in flight at once, on
 * different threads, so each has its own. */
typedef struct
{
    LC_REPORT r;
    LC_MODULE mods[LC_MAX_MODULES];
    LC_SEEN seen[LC_MAX_SEEN];
    char text[40000];
    char name[64];
    LONG gen;
} LC_WORK;

static LC_WORK g_crash_work, g_hang_work;

static volatile LONG g_beats;
static volatile DWORD g_window_tid;

/* ------------------------------------------------------------------------------------------- */
/* Small helpers                                                                               */
/* ------------------------------------------------------------------------------------------- */

/* Backoff for our spinlocks (policy: lc_backoff): spin, then yield, then sleep, so an owner at
 * lower priority always gets to run. */
static void lc_wait(int kind)
{
    if (kind == LC_SPIN)
        __asm__ volatile("pause");
    else if (kind == LC_YIELD)
        SwitchToThread();
    else
        Sleep(1);
}

static DWORD lc_now(void)
{
    return GetTickCount();
}

/* The chain lock: never on a crash path, so it may wait as long as it takes. */
static void lc_spin_lock(volatile LONG* lock)
{
    for (int attempt = 0; InterlockedCompareExchange(lock, 1, 0) != 0; attempt++)
        lc_wait(lc_backoff(attempt));
}

static void lc_spin_unlock(volatile LONG* lock)
{
    InterlockedExchange(lock, 0);
}

/* Copies what is readable from the start of [src, src + n); returns the byte count. Through
 * ReadProcessMemory on this process, which fails cleanly on an unreadable page -- including one
 * that goes away between a check and the read, as a thread's stack does when it exits -- where a
 * plain read would fault the helper. mingw has no __try. Page by page, so a read that runs off the
 * end of a mapping still returns the part before it. */
static DWORD lc_copy(void* dst, DWORD src, DWORD n)
{
    DWORD done = 0;

    while (done < n)
    {
        DWORD at = src + done;
        DWORD chunk = 0x1000 - (at & 0xFFF);
        SIZE_T got = 0;

        if (at < src)
            break;

        if (chunk > n - done)
            chunk = n - done;

        if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)at, (BYTE*)dst + done, chunk, &got) || got != chunk)
            break;

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

/* g_dir + name into out; FALSE if it does not fit. */
static BOOL lc_join(char* out, unsigned cap, const char* name)
{
    LC_TEXT p;
    lc_text_init(&p, out, cap);
    lc_puts(&p, g_dir);
    lc_puts(&p, name);
    return p.dropped == 0;
}

static void lc_wipe(void* p, unsigned n)
{
    volatile BYTE* b = p;

    for (unsigned i = 0; i < n; i++)
        b[i] = 0;
}

/* ------------------------------------------------------------------------------------------- */
/* Modules, from memory                                                                        */
/* ------------------------------------------------------------------------------------------- */

/* The module an address is in: its allocation base is the module, and the PE export directory
 * carries its name (the exe has none; its name comes from startup). Lock-free and heap-free. */
static BOOL lc_image_at(DWORD addr, LC_MODULE* mod)
{
    MEMORY_BASIC_INFORMATION m;

    if (!VirtualQuery((LPCVOID)addr, &m, sizeof(m)) || m.Type != MEM_IMAGE || !m.AllocationBase)
        return FALSE;

    /* The headers: the first page, as far as it can be read. Every offset in them is checked
     * against what was read or against the image's size before it is followed. */
    DWORD base = (DWORD)m.AllocationBase;
    static BYTE page_helper[0x1000], page_reporter[0x1000];
    BYTE* page = GetCurrentThreadId() == g_helper_tid ? page_helper : page_reporter;
    DWORD readable = lc_copy(page, base, 0x1000);
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)page;

    if (readable < sizeof(*dos) || dos->e_magic != IMAGE_DOS_SIGNATURE ||
        !lc_pe_nt_ok((DWORD)dos->e_lfanew, sizeof(IMAGE_NT_HEADERS32), readable))
        return FALSE;

    const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*)(page + dos->e_lfanew);
    DWORD image_size = nt->OptionalHeader.SizeOfImage;

    if (nt->Signature != IMAGE_NT_SIGNATURE || image_size == 0 || image_size > 0xFFFFFFFFu - base)
        return FALSE;

    mod->base = base;
    mod->size = image_size;
    lc_strcpy(mod->name, LC_MODULE_NAME, base == g_exe_base ? g_exe_name : "(no export name)");

    const IMAGE_DATA_DIRECTORY* dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    IMAGE_EXPORT_DIRECTORY exp;

    if (base != g_exe_base && nt->OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_EXPORT &&
        lc_pe_range_ok(dir->VirtualAddress, sizeof(exp), image_size) &&
        lc_copy(&exp, base + dir->VirtualAddress, sizeof(exp)) == sizeof(exp))
    {
        char name[LC_MODULE_NAME];
        DWORD got = lc_copy(name, base + exp.Name, lc_pe_name_cap(exp.Name, image_size, sizeof(name) - 1));
        name[got] = 0;

        if (got && name[0])
            lc_strcpy(mod->name, LC_MODULE_NAME, name);
    }

    return TRUE;
}

/* Every image mapped in the process, in address order: one VirtualQuery per region. */
static __attribute__((noinline)) void lc_walk_modules(LC_WORK* w)
{
    LC_REPORT* r = &w->r;
    DWORD addr = 0x10000;

    r->mods = w->mods;
    r->mod_count = 0;

    while (r->mod_count < LC_MAX_MODULES)
    {
        MEMORY_BASIC_INFORMATION m;

        if (!VirtualQuery((LPCVOID)addr, &m, sizeof(m)))
            break;

        DWORD next = (DWORD)m.BaseAddress + (DWORD)m.RegionSize;

        if (m.Type == MEM_IMAGE && m.AllocationBase == m.BaseAddress && lc_image_at(addr, &w->mods[r->mod_count]))
        {
            const LC_MODULE* mod = &w->mods[r->mod_count++];

            if (mod->base + mod->size > next)
                next = mod->base + mod->size;
        }

        if (next <= addr)
            break;

        addr = next;
    }

    lc_sort_modules(w->mods, r->mod_count);
}

/* ------------------------------------------------------------------------------------------- */
/* Writing a report (helper and reporter threads)                                              */
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

/* Opens lomhd_<kind>_<time>.txt, never overwriting an older report. The full path exists only for
 * the CreateFile call: it is wiped from the stack at once, because the stacks go in the minidump. */
static HANDLE lc_open(LC_WORK* w, int* attempt_out)
{
    for (int attempt = 1; attempt <= 9; attempt++)
    {
        char path[MAX_PATH];
        lc_report_name(w->name, sizeof(w->name), w->r.kind, w->r.when, attempt, ".txt");

        if (!lc_join(path, sizeof(path), w->name))
            return INVALID_HANDLE_VALUE;

        HANDLE f = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD error = GetLastError();
        lc_wipe(path, sizeof(path));

        if (f != INVALID_HANDLE_VALUE || error != ERROR_FILE_EXISTS)
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

static __attribute__((noinline)) void lc_write_dump(LC_WORK* w, EXCEPTION_POINTERS* ep, DWORD tid, int attempt,
    HANDLE txt)
{
    LC_TEXT t;
    char line[160];
    lc_text_init(&t, line, sizeof(line));
    lc_puts(&t, "Minidump:   ");

    /* Loaded here, on the helper, after the text is written -- never at startup, where a thread
     * inside LoadLibrary holds the loader lock and ExitProcess could kill it holding it, hanging
     * every DLL's detach. If the crashing thread holds the loader lock, this load waits, and the
     * crashing thread's deadline lets the game go on regardless. */
    static BOOL tried;

    if (!g_minidump && !tried)
    {
        tried = TRUE;
        HMODULE dbghelp = real_LoadLibraryA("dbghelp.dll");

        if (dbghelp)
            g_minidump = (MINIDUMPWRITEDUMPPROC)real_GetProcAddress(dbghelp, "MiniDumpWriteDump");
    }

    if (!g_minidump)
    {
        lc_puts(&t, "not written (dbghelp.dll unavailable)");
        lc_nl(&t);
        lc_write_all(txt, line, t.len);
        return;
    }

    char name[64], path[MAX_PATH];
    lc_report_name(name, sizeof(name), w->r.kind, w->r.when, attempt, ".dmp");
    HANDLE f = lc_join(path, sizeof(path), name) ?
        CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL) :
        INVALID_HANDLE_VALUE;
    lc_wipe(path, sizeof(path));

    if (f == INVALID_HANDLE_VALUE)
    {
        lc_puts(&t, "not written (could not create ");
        lc_puts(&t, name);
        lc_puts(&t, ")");
        lc_nl(&t);
        lc_write_all(txt, line, t.len);
        return;
    }

    /* The crashing thread's id: the dump's exception stream and "current thread" are that thread,
     * not this helper. ClientPointers FALSE: the pointers are in this process. */
    MINIDUMP_EXCEPTION_INFORMATION info;
    info.ThreadId = tid;
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

/* On the helper thread, from a request's own copy of the exception. The crashing thread may have
 * stopped waiting by now; its stack is read through lc_copy, which fails cleanly if it is gone. */
static __attribute__((noinline)) void lc_report_crash(EXCEPTION_POINTERS* ep, DWORD tid, LONG gen)
{
    LC_WORK* w = &g_crash_work;
    LC_REPORT* r = &w->r;
    EXCEPTION_RECORD* e = ep ? ep->ExceptionRecord : NULL;

    memset(r, 0, sizeof(*r));
    w->name[0] = 0;
    w->gen = gen;
    lc_fill_common(w, "crash");
    r->thread_id = tid;
    r->window_thread = tid == g_window_tid;

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

    lc_walk_modules(w);
    g_storm_base = 0;

    for (int i = 0; i < r->mod_count; i++)
        if (strcmp(lc_module_kind(w->mods[i].name), "storm") == 0)
            g_storm_base = w->mods[i].base;

    LC_TEXT note;
    char filters[96];
    lc_text_init(&note, filters, sizeof(filters));
    lc_dec(&note, (DWORD)g_chain.count);
    lc_puts(&note, " level(s) chained after this report");
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
    lc_write_dump(w, ep, tid, attempt, f);
    FlushFileBuffers(f);
    CloseHandle(f);
    lc_note("crash", w, TRUE);
}

/* The game's own filter resumed execution after a report: not a crash after all. */
static void lc_note_resumed(LONG gen)
{
    LC_WORK* w = &g_crash_work;

    /* Only the report of that request: a later one may have been written since. */
    if (w->gen != gen || !w->name[0])
        return;

    char path[MAX_PATH];

    if (lc_join(path, sizeof(path), w->name))
    {
        HANDLE f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);

        if (f != INVALID_HANDLE_VALUE)
        {
            static const char outcome[] = "Outcome:    the game's own exception filter resumed execution "
                "(EXCEPTION_CONTINUE_EXECUTION) -- this was not a crash.\r\n";
            lc_write_all(f, outcome, sizeof(outcome) - 1);
            CloseHandle(f);
        }
    }

    char line[160];
    _snprintf(line, sizeof(line), "crash: the game's filter resumed after %s; not a crash", w->name);
    line[sizeof(line) - 1] = 0;
    lomhd_log(line);
    InterlockedDecrement(&g_admit.reports);
}

static DWORD WINAPI lc_helper(LPVOID unused)
{
    (void)unused;
    LONG resumed = 0;
    HANDLE wait[2] = { g_request, g_main };

    for (;;)
    {
        /* The main thread gone means the game is ending (or a filter ended just that thread):
         * this thread must not be what keeps the process alive. */
        if (WaitForMultipleObjects(2, wait, FALSE, INFINITE) != WAIT_OBJECT_0)
            return 0;

        int slot;

        while ((slot = lc_mail_take(&g_mail)) >= 0)
        {
            LC_REQUEST* q = &g_request_data[slot];
            lc_report_crash(&q->pointers, q->tid, g_mail.gen[slot]);
            lc_mail_finish(&g_mail, slot);
            SetEvent(g_done);
        }

        LONG r = g_resume_gen;

        if (r && r != resumed)
        {
            resumed = r;
            lc_note_resumed(r);
        }
    }
}

/* ------------------------------------------------------------------------------------------- */
/* The filter: the crashing thread's side                                                      */
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
            g_threads[i].visited = 0;
            return &g_threads[i];
        }
    }

    return NULL;
}

static LONG lc_call(EXCEPTION_POINTERS* ep, int level)
{
    LPTOP_LEVEL_EXCEPTION_FILTER f = (LPTOP_LEVEL_EXCEPTION_FILTER)lc_chain_next(&g_chain, g_stubs, level);
    return f ? f(ep) : EXCEPTION_CONTINUE_SEARCH;
}

/* Asks the helper for a report and waits for it until `deadline`, the one deadline this thread
 * has (taken on entry, so a thread queued behind another crash waits no longer in total). Returns
 * the request's generation, or 0 if no report was asked for. */
static LONG lc_handoff(EXCEPTION_POINTERS* ep, DWORD me, DWORD deadline)
{
    const EXCEPTION_RECORD* e = ep ? ep->ExceptionRecord : NULL;

    /* No helper -- the process is exiting, or the main thread has ended it -- no report. */
    if (!e || !ep->ContextRecord || g_detached || !g_helper || WaitForSingleObject(g_helper, 0) != WAIT_TIMEOUT)
        return 0;

    /* Bounded by this thread's deadline: a lock whose owner never runs again cannot keep a
     * crashing thread from the game's filter. */
    if (!lc_lock_until(&g_admit_lock, deadline, lc_now, lc_wait))
        return 0;

    /* A report is counted, and becomes the duplicate reference, only once a slot is claimed. Both
     * slots busy (the helper stuck on one, another queued): no report, and nothing counted. */
    int slot = -1;

    if (lc_admit_check(&g_admit, e->ExceptionCode, (DWORD)e->ExceptionAddress, me, LC_MAX_REPORTS) &&
        (slot = lc_mail_claim(&g_mail)) >= 0)
        lc_admit_commit(&g_admit, e->ExceptionCode, (DWORD)e->ExceptionAddress, me);

    lc_spin_unlock(&g_admit_lock);

    if (slot < 0)
        return 0;

    LC_REQUEST* q = &g_request_data[slot];
    q->record = *e;
    q->record.ExceptionRecord = NULL;
    q->context = *ep->ContextRecord;
    q->pointers.ExceptionRecord = &q->record;
    q->pointers.ContextRecord = &q->context;
    q->tid = me;
    LONG gen = lc_mail_post(&g_mail, slot);
    SetEvent(g_request);

    for (;;)
    {
        if (lc_mail_collect(&g_mail, slot))
            return gen;

        DWORD left = lc_left(deadline, GetTickCount());

        if (!left)
            break;

        WaitForSingleObject(g_done, left < 100 ? left : 100);
    }

    /* Out of time: cancelled if the helper never started it; if it did, the slot stays the
     * helper's until it finishes, and the request's copies stay valid for it. */
    lc_mail_abandon(&g_mail, slot);
    return gen;
}

/* Every entry point lands here with its level. Kept small: it runs on the crashing thread, which
 * after a stack overflow has little stack left. */
static __attribute__((noinline)) LONG lc_enter(EXCEPTION_POINTERS* ep, int level)
{
    DWORD me = GetCurrentThreadId();

    /* The helper faulting inside its own report: nothing to hand off to. */
    if (me == g_helper_tid)
        return EXCEPTION_CONTINUE_SEARCH;

    LC_THREAD* th = lc_thread_find(me);

    /* Re-entry on this thread: a filter we called chained into one of our levels. Continue below
     * it, once per level. */
    if (th)
        return lc_chain_enter(&th->visited, level) ? lc_call(ep, level) : EXCEPTION_CONTINUE_SEARCH;

    th = lc_thread_claim(me);

    if (!th)
        return lc_call(ep, level);

    lc_chain_enter(&th->visited, level);
    LONG gen = g_installed ? lc_handoff(ep, me, GetTickCount() + LC_HANDOFF_MS) : 0;
    LONG ret = lc_call(ep, level);

    if (gen && ret == EXCEPTION_CONTINUE_EXECUTION)
    {
        InterlockedExchange(&g_resume_gen, gen);
        SetEvent(g_request);
    }

    th->visited = 0;
    InterlockedExchange(&th->tid, 0);
    return ret;
}

#define LC_STUB(n) \
    static LONG WINAPI lc_stub_##n(EXCEPTION_POINTERS* ep) { return lc_enter(ep, n); }
LC_STUB(0)
LC_STUB(1)
LC_STUB(2)
LC_STUB(3)
LC_STUB(4)
LC_STUB(5)
LC_STUB(6)
LC_STUB(7)

static LPTOP_LEVEL_EXCEPTION_FILTER const g_stub_fn[LC_MAX_CHAIN] = {
    lc_stub_0, lc_stub_1, lc_stub_2, lc_stub_3, lc_stub_4, lc_stub_5, lc_stub_6, lc_stub_7,
};

/* ------------------------------------------------------------------------------------------- */
/* Keeping the chain                                                                           */
/* ------------------------------------------------------------------------------------------- */

LPTOP_LEVEL_EXCEPTION_FILTER lomhd_crash_set_filter(LPTOP_LEVEL_EXCEPTION_FILTER filter)
{
    if (!g_installed)
        return real_SetUnhandledExceptionFilter(filter);

    int install;
    lc_spin_lock(&g_chain_lock);
    void* old = lc_chain_set(&g_chain, g_stubs, (void*)filter, &install);

    if (install >= 0)
        real_SetUnhandledExceptionFilter(g_stub_fn[install]);

    lc_spin_unlock(&g_chain_lock);
    return (LPTOP_LEVEL_EXCEPTION_FILTER)old;
}

static void lc_reassert(void)
{
    lc_spin_lock(&g_chain_lock);
    int before = g_chain.count;
    LPTOP_LEVEL_EXCEPTION_FILTER ours = g_stub_fn[before - 1];
    LPTOP_LEVEL_EXCEPTION_FILTER current = real_SetUnhandledExceptionFilter(ours);
    int dropped;
    int level = lc_chain_observe(&g_chain, g_stubs, (void*)current, &dropped);

    if (g_stub_fn[level] != ours)
        real_SetUnhandledExceptionFilter(g_stub_fn[level]);

    int after = g_chain.count;
    lc_spin_unlock(&g_chain_lock);

    if (after == before && !dropped)
        return;

    if (g_chain_logged++ >= LC_CHAIN_LOG_MAX)
        return;

    char line[200];

    if (dropped)
        _snprintf(line, sizeof(line), "crash reports: another filter (%08lx) replaced ours; the chain is full, "
            "so ours is back on top without it", (unsigned long)(DWORD)current);
    else if (after > before)
        _snprintf(line, sizeof(line), "crash reports: another filter (%08lx) replaced ours without the hook; "
            "ours runs first again, then that one", (unsigned long)(DWORD)current);
    else
        _snprintf(line, sizeof(line), "crash reports: a filter uninstalled itself; %d level(s) chained now", after);

    line[sizeof(line) - 1] = 0;
    lomhd_log(line);
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
        LC_MODULE m;

        /* The game's exe whatever it is called, Storm, or this DLL: not the system DLLs, whose
         * IsBadReadPtr and friends fault and recover as a matter of course. */
        if (*logged >= LC_SEEN_LOG_MAX || !lc_image_at(s.address, &m) ||
            (m.base != g_exe_base && strcmp(lc_module_kind(m.name), "other") == 0))
            continue;

        char line[200];
        _snprintf(line, sizeof(line), "first-chance %08lx at %s+0x%08lx (target %08lx, thread %lu) -- "
            "may have been handled; if the game closes or hangs next, it started here",
            (unsigned long)s.code, m.name, (unsigned long)(s.address - m.base), (unsigned long)s.target,
            (unsigned long)s.thread);
        line[sizeof(line) - 1] = 0;
        lomhd_log(line);

        if (++*logged == LC_SEEN_LOG_MAX)
            lomhd_log("first-chance: no more of these this session");
    }
}

/* ------------------------------------------------------------------------------------------- */
/* Hang watchdog (reporter thread)                                                             */
/* ------------------------------------------------------------------------------------------- */

void lomhd_crash_beat(void)
{
    if (GetCurrentThreadId() == g_window_tid)
        InterlockedIncrement(&g_beats);
}

static BOOL lc_report_hang(DWORD tid, DWORD silent_ms)
{
    LC_WORK* w = &g_hang_work;
    LC_REPORT* r = &w->r;
    memset(r, 0, sizeof(*r));
    w->name[0] = 0;
    lc_fill_common(w, "hang");
    r->thread_id = tid;
    r->window_thread = TRUE;
    r->hang_ms = silent_ms;
    r->wine = IsWine();

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

    lc_walk_modules(w);

    int attempt = 1;
    HANDLE f = lc_open(w, &attempt);

    if (f == INVALID_HANDLE_VALUE)
    {
        w->name[0] = 0;
        lc_note("hang", w, FALSE);
        return TRUE;
    }

    LC_TEXT t;
    lc_text_init(&t, w->text, sizeof(w->text));
    lc_format_report(r, &t);

    if (!r->has_regs)
        lc_puts(&t, "\r\nThe window thread's registers could not be read (SuspendThread/GetThreadContext failed).\r\n");

    lc_write_all(f, w->text, t.len);
    CloseHandle(f);
    lc_note("hang", w, TRUE);
    return TRUE;
}

/* The reported hang ended. Say so in its report, and on Wine give back its place under the cap:
 * there every switch to another app can look like a hang (see lc_hang_outcome). */
static void lc_note_hang_recovered(DWORD stalled_ms)
{
    LC_WORK* w = &g_hang_work;
    BOOL wine = IsWine();
    char path[MAX_PATH], outcome[160];
    LC_TEXT t;

    lc_text_init(&t, outcome, sizeof(outcome));
    lc_hang_outcome(&t, stalled_ms, wine);

    BOOL appended = FALSE;

    if (lc_join(path, sizeof(path), w->name))
    {
        HANDLE f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        lc_wipe(path, sizeof(path));

        if (f != INVALID_HANDLE_VALUE)
        {
            DWORD a = 0, b = 0;
            appended = WriteFile(f, "\r\n", 2, &a, NULL) && WriteFile(f, outcome, t.len, &b, NULL) &&
                a == 2 && b == t.len;
            CloseHandle(f);
        }
    }

    /* Bounded: a game that really does stall 20 s and recover, again and again, still stops
     * writing reports after LC_MAX_HANGS + LC_MAX_HANG_REFUNDS. */
    BOOL refund = wine && g_hang_count > 0 && g_hang_refunds < LC_MAX_HANG_REFUNDS;

    if (refund)
    {
        g_hang_count--;
        g_hang_refunds++;
    }

    char line[200];
    _snprintf(line, sizeof(line), "hang: %s resumed after %lu s%s%s", w->name, (unsigned long)(stalled_ms / 1000),
        wine ? (refund ? " (Wine: likely a background window, not counted)" : " (Wine: likely a background window)") : "",
        appended ? "" : "; could not add this to the report");
    line[sizeof(line) - 1] = 0;
    lomhd_log(line);
}

typedef struct
{
    LONG beats;
    DWORD quiet_since;          /* the last beat, or the last moment the window was not in front */
    BOOL reported;              /* this hang is reported; re-armed by the next beat */
    BOOL written;               /* ...and its report is g_hang_work.name */
    DWORD hang_since;           /* when the reported hang's silence began */
    HWND hwnd;
} LC_WATCH;

static void lc_watch(LC_WATCH* s, DWORD now)
{
    HWND hwnd = g_ddraw.hwnd;
    DWORD pid = 0;
    DWORD tid = hwnd && IsWindow(hwnd) ? GetWindowThreadProcessId(hwnd, &pid) : 0;

    /* A report belongs to its window: beats from a new window do not mean the old one recovered. */
    if (!tid || pid != GetCurrentProcessId())
    {
        s->quiet_since = now;
        s->hwnd = NULL;
        s->written = FALSE;
        return;
    }

    if (hwnd != s->hwnd)
    {
        s->hwnd = hwnd;
        s->quiet_since = now;
        s->written = FALSE;
    }

    InterlockedExchange((volatile LONG*)&g_window_tid, (LONG)tid);
    LONG beats = g_beats;

    if (beats != s->beats)
    {
        if (s->written)
            lc_note_hang_recovered(now - s->hang_since);

        s->beats = beats;
        s->quiet_since = now;
        s->reported = FALSE;
        s->written = FALSE;
        return;
    }

    /* Only time in front counts: a minimized or background game is allowed to sit still. Five
     * seconds into a hang Windows replaces the window with a ghost, which then is the foreground
     * window; HungWindowFromGhostWindow says whose ghost it is. (Wine does not ghost, and has no
     * such function; there the window itself stays in front.) */
    HWND fg = GetForegroundWindow();
    HWND ghost_of = fg && fg != hwnd && g_ghost_owner ? g_ghost_owner(fg) : NULL;

    if (!lc_in_front(hwnd, fg, ghost_of, IsIconic(hwnd)))
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

    if (g_hang_count >= LC_MAX_HANGS)
    {
        s->reported = TRUE;
        return;
    }

    if (lc_report_hang(tid, now - s->quiet_since))
    {
        s->reported = TRUE;
        s->written = g_hang_work.name[0] != 0;
        s->hang_since = s->quiet_since;
        g_hang_count++;
    }
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

    /* A static buffer, not malloc: ExitProcess can end this thread anywhere, and one ended inside
     * the C runtime's heap leaves that heap locked for everything that runs at exit -- DllMain
     * detach included. (The victim's `caught` mode hung about one run in seventy that way.) */
    static BYTE buf[1 << 16];
    LC_SHA256 sha;
    lc_sha256_init(&sha);
    DWORD got = 0;
    BOOL ok;

    while ((ok = ReadFile(f, buf, sizeof(buf), &got, NULL)) && got)
        lc_sha256_update(&sha, buf, got);

    CloseHandle(f);

    if (ok)
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

    /* Nothing here takes the loader lock -- no LoadLibrary, GetModuleHandle or GetProcAddress
     * (Wine's take it too): ExitProcess can end this thread at any moment, and a thread ended
     * holding the loader lock hangs every DLL's detach. Found 2026-09-27 as a rare hang of the
     * victim's `caught` mode, when this thread loaded dbghelp here. Lookups happen in DllMain,
     * which holds the lock already; dbghelp loads on the helper when a report needs it. */
    /* And nothing at all for the first moments: a game that exits at once (a failed check at
     * launch, or the victim's `caught` mode) must find this thread idle in a wait, not inside a
     * file or heap call whose lock ExitProcess would orphan. Measured on Wine 10: `caught` hung in
     * about one run in seventy with the startup work done at once, one in 240 with its malloc
     * gone. The helper is ready from the start; only the log line, the exe hash, the re-assert and
     * the hang watchdog wait. */
    if (WaitForSingleObject(g_main, LC_QUIET_START_MS) != WAIT_TIMEOUT)
        return 0;

    char line[200];
    _snprintf(line, sizeof(line), "crash reports: on (hang reports %s)", g_hang_reports ? "on" : "off");
    line[sizeof(line) - 1] = 0;
    lomhd_log(line);

    lc_hash_exe();
    g_terrain = lomhd_terrain_active() ? (lomhd_files_on() ? "hybrid exe, art from lomhd_terrain" :
        "hybrid exe, art from pic.mpq") : "off (not the hybrid exe)";

    LC_WATCH watch;
    memset(&watch, 0, sizeof(watch));
    watch.quiet_since = GetTickCount();
    DWORD last_assert = 0;

    /* Every quarter second -- or until the main thread ends, and then this thread ends too. */
    while (WaitForSingleObject(g_main, 250) == WAIT_TIMEOUT)
    {
        DWORD now = GetTickCount();

        if (now - last_assert >= 1000)
        {
            lc_reassert();
            last_assert = now;
        }

        lc_log_seen(&g_seen_logged_upto, &g_seen_logged);

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
    lc_strcpy(g_exe_name, sizeof(g_exe_name), lc_basename(g_exe_path));

    /* Lords of Magic only, like the rest of the fork, unless asked for (the test program is not
     * lomse.exe). And off entirely with lomhd_no_crash_reports beside the game. */
    if ((strcmp(lc_module_kind(g_exe_name), "lomse") != 0 && !lc_flag("lomhd_crash_reports")) ||
        lc_flag("lomhd_no_crash_reports"))
        return;

    /* Pinned: the process-wide filter points into this DLL, so it must never be unmapped, even by
     * an exe that loads and frees ddraw.dll (lomse imports it statically; opted-in exes may not). */
    BOOL(WINAPI * handle_ex)(DWORD, LPCSTR, HMODULE*) =
        (void*)real_GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetModuleHandleExA");
    HMODULE self;

    if (!handle_ex || !handle_ex(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        (LPCSTR)(void*)lc_stub_0, &self))
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

    /* Lookups here, under the loader lock DllMain already holds, so our threads never need it. */
    HMODULE user32 = GetModuleHandleA("user32.dll");
    g_is_hung = (ISHUNGAPPWINDOWPROC)real_GetProcAddress(user32, "IsHungAppWindow");
    g_ghost_owner = (HUNGWINDOWFROMGHOSTPROC)real_GetProcAddress(user32, "HungWindowFromGhostWindow");
    lc_describe_host();

    g_request = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_done = CreateEventA(NULL, FALSE, FALSE, NULL);

    /* DllMain runs on the thread that loaded us: for lomse (a static import) the main thread. A
     * host that loads ddraw.dll on a worker thread gets that thread instead, and loses reporting
     * when it ends (see the README). */
    if (g_request && g_done && DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
        &g_main, SYNCHRONIZE, FALSE, 0))
    {
        /* Both threads are created under the loader lock and start once DllMain returns. */
        g_helper = CreateThread(NULL, 0, lc_helper, NULL, 0, &g_helper_tid);
    }

    if (!g_helper)
    {
        /* Partly set up: give back what was made, and stay off. */
        if (g_main)
            CloseHandle(g_main);

        if (g_request)
            CloseHandle(g_request);

        if (g_done)
            CloseHandle(g_done);

        g_main = g_request = g_done = NULL;
        return;
    }

    for (int i = 0; i < LC_MAX_CHAIN; i++)
        g_stubs[i] = (void*)g_stub_fn[i];

    lc_chain_init(&g_chain, (void*)real_SetUnhandledExceptionFilter(g_stub_fn[0]));
    InterlockedExchange(&g_installed, 1);

    PVOID(WINAPI * add_handler)(ULONG, PVECTORED_EXCEPTION_HANDLER) =
        (void*)real_GetProcAddress(GetModuleHandleA("kernel32.dll"), "AddVectoredExceptionHandler");

    if (add_handler)
        add_handler(0, (PVECTORED_EXCEPTION_HANDLER)lc_vectored);

    HANDLE t = CreateThread(NULL, 0, lc_reporter, NULL, 0, NULL);

    if (t)
        CloseHandle(t);
}

void lomhd_crash_exit(BOOL process_exit)
{
    /* Installed means pinned, so the only detach we see is the process ending. */
    if (!g_installed || !process_exit)
        return;

    InterlockedExchange(&g_detached, 1);

    /* ExitProcess has already ended the reporter thread, so breadcrumbs from its last quarter
     * second may be unlogged -- and a game that caught an access violation and chose to exit lands
     * exactly here. Logged now, before any DLL detached after us (Storm, say) can hang. */
    lc_log_seen(&g_seen_logged_upto, &g_seen_logged);
}
