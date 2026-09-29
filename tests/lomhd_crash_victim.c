/* A program that loads the built ddraw.dll and misbehaves on purpose, to exercise the crash and hang
 * reporter (src/lomhd_crash.c) under Wine or Windows. A test tool only: never shipped. Run it through
 * tests/lomhd_crash_victim.sh, which asserts each mode's exit code and report files.
 *
 * Its filters stand in for a game's: game_filter is installed through the victim's import table
 * (the path cnc-ddraw patches), the "bypass" ones through GetProcAddress, around the patch. Each
 * filter that ends the process does so with its own exit code, so the exit code says which ran:
 *   3  a vectored handler caught the fault and exited (modes caught, caught-heap)
 *   43 game_filter called its predecessor, which returned (mode chain)
 *   44 game_filter ran
 *   45 bypass_filter ran
 *   46 bypass_filter called its predecessor, which returned without ending the process
 *   47 execution resumed after the fault (mode continue)
 *   49 deadline: the second crashing thread reached the game's filter within ~10 s of its fault
 *   50 deadline: it took longer
 *   51 abandon: the minidump's exception is not the one that happened
 *   52 abandon: no minidump to check
 *   0  no fault, or the hang modes ran to the end, or abandon's dump checked out
 * Modes exitthread and overflow-loud end only the faulting (main) thread; the process must then
 * end by itself, which the harness checks with its timeout. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef HRESULT(WINAPI* DIRECTDRAWCREATE)(GUID*, void**, IUnknown*);
typedef LPTOP_LEVEL_EXCEPTION_FILTER(WINAPI* SETFILTER)(LPTOP_LEVEL_EXCEPTION_FILTER);

static LPTOP_LEVEL_EXCEPTION_FILTER g_game_prev, g_bypass_prev;
static BOOL g_bypass_chains;

static LONG WINAPI game_filter(EXCEPTION_POINTERS* ep)
{
    printf("game filter: %08lx\n", ep->ExceptionRecord->ExceptionCode);
    ExitProcess(44);
    return EXCEPTION_EXECUTE_HANDLER;
}

/* For the stack overflow: no printf on a thread whose stack is spent. */
static LONG WINAPI game_filter_quiet(EXCEPTION_POINTERS* ep)
{
    (void)ep;
    ExitProcess(44);
    return EXCEPTION_EXECUTE_HANDLER;
}

static LONG WINAPI game_filter_chaining(EXCEPTION_POINTERS* ep)
{
    printf("game filter: calling its predecessor %p\n", (void*)g_game_prev);

    if (g_game_prev)
        g_game_prev(ep);

    ExitProcess(43);
    return EXCEPTION_EXECUTE_HANDLER;
}

static LONG WINAPI bypass_filter(EXCEPTION_POINTERS* ep)
{
    printf("bypass filter: %08lx\n", ep->ExceptionRecord->ExceptionCode);

    if (g_bypass_chains)
    {
        printf("bypass filter: calling its predecessor %p\n", (void*)g_bypass_prev);

        if (g_bypass_prev)
            g_bypass_prev(ep);

        ExitProcess(46);
    }

    ExitProcess(45);
    return EXCEPTION_EXECUTE_HANDLER;
}

/* Skips the faulting store (always the two-byte `mov %edx,(%eax)` below) and carries on. */
static LONG WINAPI resuming_filter(EXCEPTION_POINTERS* ep)
{
    printf("resuming filter: skipping the faulting store\n");
    ep->ContextRecord->Eip += 2;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void put(const char* s);

static LONG WINAPI catch_and_exit(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
    {
        put("caught: calling ExitProcess\n");
        ExitProcess(3);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

/* A few frames of the victim's own on the stack, so the report has return addresses to annotate.
 * The store is written out so its length is known: 89 10, mov %edx,(%eax). */
static __attribute__((noinline)) int deepest(volatile int* p, int v)
{
    __asm__ volatile("movl %%edx, (%%eax)" : : "a"(p), "d"(v) : "memory");
    return v + 1;
}

static __attribute__((noinline)) int middle(int v)
{
    return deepest((volatile int*)0x10, v) * 3;
}

static __attribute__((noinline)) int outer(int v)
{
    return middle(v + 1) + 7;
}

static __attribute__((noinline)) int recurse(volatile int depth)
{
    volatile char pad[256];
    pad[0] = (char)depth;
    return recurse(depth + 1) + pad[0];
}

/* Output without the heap: some modes hold the heap lock on another thread. */
static void put(const char* s)
{
    DWORD n = 0, w;

    while (s[n])
        n++;

    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, n, &w, NULL);
}

static void put_num(DWORD v)
{
    char b[12];
    int i = 11;
    b[i] = 0;

    do
        b[--i] = (char)('0' + v % 10);
    while ((v /= 10) && i > 0);

    put(b + i);
}

/* exitthread: a game filter that ends only the faulting thread. */
static LONG WINAPI exit_thread_filter(EXCEPTION_POINTERS* ep)
{
    (void)ep;
    put("exitthread: the filter ends the faulting thread only\n");
    ExitThread(48);
    return EXCEPTION_EXECUTE_HANDLER;
}

/* deadline: thread A takes the loader lock and faults. The helper writes A's text report (which
 * needs no loader lock) and then stalls loading dbghelp for the minidump; B faults half a second
 * later. B must reach the game's filter about ten seconds after its fault, not ten for the report
 * lock plus ten for the handoff -- and A's text report must exist. */
typedef LONG(WINAPI* LDRLOCKLOADERLOCK)(ULONG, ULONG*, ULONG_PTR*);
static LDRLOCKLOADERLOCK g_lock_loader;
static volatile DWORD g_a_tid, g_b_tid, g_b_fault_tick;
static HANDLE g_a_locked, g_a_clobbered, g_b_ready;
static volatile DWORD g_a_top, g_a_fault_address;

static LONG WINAPI deadline_filter(EXCEPTION_POINTERS* ep)
{
    (void)ep;

    if (GetCurrentThreadId() == g_b_tid)
    {
        DWORD ms = GetTickCount() - g_b_fault_tick;
        put("deadline: B reached the game filter ");
        put_num(ms);
        put(" ms after its fault\n");
        TerminateProcess(GetCurrentProcess(), ms <= 11500 ? 49 : 50);
    }

    Sleep(INFINITE);            /* A: keeps the loader lock */
    return EXCEPTION_CONTINUE_SEARCH;
}

/* A takes the lock only once B is running: creating a thread needs the loader lock (thread-attach
 * calls) and allocates, so creating B after A held it deadlocked the victim itself. */
static DWORD WINAPI deadline_a(LPVOID unused)
{
    (void)unused;
    WaitForSingleObject(g_b_ready, INFINITE);
    ULONG_PTR magic = 0;
    g_lock_loader(0, NULL, &magic);
    SetEvent(g_a_locked);
    return (DWORD)outer(1);
}

static DWORD WINAPI deadline_b(LPVOID unused)
{
    (void)unused;
    SetEvent(g_b_ready);
    WaitForSingleObject(g_a_locked, INFINITE);
    Sleep(500);
    g_b_fault_tick = GetTickCount();
    return (DWORD)outer(1);
}

/* abandon: A takes the heap lock and faults; the helper stalls in dbghelp; A times out, and its
 * filter unlocks the heap and resumes A at clobber() on a fresh stack pointer near the top of A's
 * stack, which overwrites the stack where A's exception record and context were. The helper then
 * finishes the minidump -- which must still show A's real exception. */
static void __attribute__((noinline, noreturn)) clobber(void)
{
    volatile BYTE area[96 * 1024];

    for (unsigned i = 0; i < sizeof(area); i++)
        area[i] = 0xCC;

    SetEvent(g_a_clobbered);

    for (;;)
        Sleep(INFINITE);
}

static LONG WINAPI abandon_filter(EXCEPTION_POINTERS* ep)
{
    g_a_fault_address = (DWORD)ep->ExceptionRecord->ExceptionAddress;
    HeapUnlock(GetProcessHeap());
    ep->ContextRecord->Esp = (g_a_top - 512) & ~15u;
    ep->ContextRecord->Eip = (DWORD)clobber;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static DWORD WINAPI abandon_a(LPVOID unused)
{
    volatile DWORD top = 0;
    (void)unused;
    g_a_top = (DWORD)&top;
    HeapLock(GetProcessHeap());
    return (DWORD)outer(1);
}

/* The exception stream of the one lomhd_crash_*.dmp in the current folder: code, address, and
 * the EIP of its context. */
static int check_dump(DWORD want_address)
{
    WIN32_FIND_DATAA fd;
    HANDLE find = FindFirstFileA("lomhd_crash_*.dmp", &fd);

    if (find == INVALID_HANDLE_VALUE)
        return 52;

    FindClose(find);
    HANDLE f = CreateFileA(fd.cFileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    static BYTE d[4 << 20];
    DWORD size = 0;

    if (f == INVALID_HANDLE_VALUE || !ReadFile(f, d, sizeof(d), &size, NULL))
        return 52;

    CloseHandle(f);
    DWORD streams = *(DWORD*)(d + 8), dir = *(DWORD*)(d + 12);

    for (DWORD i = 0; i < streams && dir + 12 * i + 12 <= size; i++)
    {
        DWORD type = *(DWORD*)(d + dir + 12 * i), rva = *(DWORD*)(d + dir + 12 * i + 8);

        if (type != 6 || rva + 0xA8 > size)
            continue;

        /* MINIDUMP_EXCEPTION_STREAM: ThreadId, pad, then MINIDUMP_EXCEPTION (code at +8, address
         * at +24 as 64 bits); the context's location at +0xA0. CONTEXT's Eip is at +0xB8. */
        DWORD code = *(DWORD*)(d + rva + 8), address = *(DWORD*)(d + rva + 24);
        DWORD ctx = *(DWORD*)(d + rva + 0xA4);
        DWORD eip = ctx + 0xBC <= size ? *(DWORD*)(d + ctx + 0xB8) : 0;
        put("abandon: dump says code ");
        put_num(code);
        put(", address ");
        put_num(address);
        put(", context eip ");
        put_num(eip);
        put("; the fault was at ");
        put_num(want_address);
        put("\n");

        if (code == 0xC0000005 && address == want_address && eip == want_address)
        {
            put("abandon: dump holds the copied exception\n");
            return 0;
        }

        return 51;
    }

    return 52;
}

/* caught-heap: another thread holds the heap lock when the game exits (ExitProcess kills it with
 * the lock held). Nothing on the way out -- our DLL_PROCESS_DETACH logging included -- may wait
 * for that lock. */
static HANDLE g_heap_held;

static DWORD WINAPI hold_heap(LPVOID unused)
{
    (void)unused;
    HeapLock(GetProcessHeap());
    SetEvent(g_heap_held);
    Sleep(INFINITE);
    return 0;
}

static SETFILTER real_set_filter(void)
{
    return (SETFILTER)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetUnhandledExceptionFilter");
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    return DefWindowProcA(h, m, w, l);
}

static void pump(DWORD ms)
{
    DWORD end = GetTickCount() + ms;

    while ((LONG)(end - GetTickCount()) > 0)
    {
        MSG msg;

        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        Sleep(10);
    }
}

/* One Lock/Unlock on the window thread: the beat the hang watchdog waits for. IDirectDraw::
 * CreateSurface is slot 6; IDirectDrawSurface Release 2, Lock 25, Unlock 32. */
static BOOL beat(void** dd, const char* mode)
{
    typedef HRESULT(WINAPI* CREATESURFACE)(void*, void*, void***, void*);
    typedef HRESULT(WINAPI* LOCK)(void*, RECT*, void*, DWORD, HANDLE);
    typedef HRESULT(WINAPI* UNLOCK)(void*, void*);
    typedef ULONG(WINAPI* RELEASE)(void*);
    DWORD desc[27];                                         /* DDSURFACEDESC, 108 bytes */
    void** surface = NULL;

    memset(desc, 0, sizeof(desc));
    desc[0] = sizeof(desc);
    desc[1] = 1;                                            /* DDSD_CAPS */
    desc[26] = 0x200;                                       /* DDSCAPS_PRIMARYSURFACE */

    if (((CREATESURFACE)((void**)*dd)[6])(dd, desc, &surface, NULL) != 0 || !surface)
    {
        printf("%s: CreateSurface failed\n", mode);
        return FALSE;
    }

    desc[0] = sizeof(desc);
    HRESULT locked = ((LOCK)((void**)*surface)[25])(surface, NULL, desc, 1 /* DDLOCK_WAIT */, NULL);

    if (locked == 0)
        ((UNLOCK)((void**)*surface)[32])(surface, NULL);

    ((RELEASE)((void**)*surface)[2])(surface);
    printf("%s: resumed, Lock %s\n", mode, locked == 0 ? "ok" : "failed");
    return locked == 0;
}

/* "away": while the window thread is stuck, another thread puts a window in front, the way a
 * player alt-tabs away from a hung game. */
static DWORD WINAPI step_away(LPVOID unused)
{
    Sleep(25000);
    HWND other = CreateWindowExA(0, "lomhd_crash_victim", "lomhd crash victim (in front)",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 200, 200, 320, 200, NULL, NULL, GetModuleHandleA(NULL), NULL);
    SetForegroundWindow(other);
    MSG m;

    for (DWORD end = GetTickCount() + 20000; GetTickCount() < end; Sleep(10))
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE))
            DispatchMessageA(&m);

    return 0;
}

static int hang(HMODULE ddraw, const char* mode)
{
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "lomhd_crash_victim";
    RegisterClassA(&wc);

    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "lomhd crash victim (hang test)",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 320, 200, NULL, NULL, wc.hInstance, NULL);

    DIRECTDRAWCREATE create = (DIRECTDRAWCREATE)GetProcAddress(ddraw, "DirectDrawCreate");
    void** dd = NULL;

    if (!hwnd || !create || create(NULL, (void**)&dd, NULL) != 0 || !dd)
    {
        printf("%s: setup failed\n", mode);
        return 2;
    }

    /* IDirectDraw::SetCooperativeLevel is slot 20 of the vtable. */
    typedef HRESULT(WINAPI* SETCOOP)(void*, HWND, DWORD);
    SETCOOP set_coop = (SETCOOP)((void**)*dd)[20];
    set_coop(dd, hwnd, 8 /* DDSCL_NORMAL */);

    SetForegroundWindow(hwnd);
    pump(2000);

    if (strcmp(mode, "minimized") == 0)
    {
        /* The real ShowWindow: cnc-ddraw patches the victim's import. */
        BOOL(WINAPI * show)(HWND, int) = (void*)GetProcAddress(GetModuleHandleA("user32.dll"), "ShowWindow");
        show(hwnd, SW_MINIMIZE);
        pump(1000);
    }

    if (strcmp(mode, "background") == 0)
    {
        HWND other = CreateWindowExA(0, wc.lpszClassName, "lomhd crash victim (in front)",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE, 200, 200, 320, 200, NULL, NULL, wc.hInstance, NULL);
        SetForegroundWindow(other);
        pump(1000);
    }

    if (strcmp(mode, "resume4") == 0)
    {
        /* Four stalls that each recover: on Wine all four are reported, past LC_MAX_HANGS. */
        for (int i = 0; i < 4; i++)
        {
            printf("%s: not pumping for 22 s (%d)\n", mode, i + 1);
            Sleep(22000);

            if (!beat(dd, mode))
                return 2;

            pump(1000);
        }

        return 0;
    }

    if (strcmp(mode, "away") == 0)
        CloseHandle(CreateThread(NULL, 0, step_away, NULL, 0, NULL));

    BOOL idle = strcmp(mode, "idle") == 0;
    printf("%s: %s for 40 s\n", mode, idle ? "pumping, no DirectDraw calls" : "not pumping");

    if (idle)
        pump(40000);
    else
        Sleep(40000);

    if (strcmp(mode, "resume") == 0 || strcmp(mode, "away") == 0)
    {
        if (!beat(dd, mode))
            return 2;

        pump(1500);
    }

    /* The real GetForegroundWindow: cnc-ddraw patches the victim's import to answer with the game
     * window (found 2026-09-27; the watchdog in ddraw.dll calls the real one). */
    HWND(WINAPI * foreground)(void) = (void*)GetProcAddress(GetModuleHandleA("user32.dll"), "GetForegroundWindow");
    printf("%s: after 40 s, in front %s, minimized %s\n", mode, foreground() == hwnd ? "yes" : "no",
        IsIconic(hwnd) ? "yes" : "no");
    pump(500);
    return 0;
}

int main(int argc, char** argv)
{
    const char* mode = argc > 1 ? argv[1] : "av";
    setvbuf(stdout, NULL, _IONBF, 0);

    HMODULE ddraw = LoadLibraryA("ddraw.dll");

    if (!ddraw)
    {
        printf("could not load ddraw.dll (%lu)\n", GetLastError());
        return 2;
    }

    printf("mode %s\n", mode);

    if (strcmp(mode, "hang") == 0 || strcmp(mode, "resume") == 0 || strcmp(mode, "resume4") == 0 ||
        strcmp(mode, "away") == 0 || strcmp(mode, "idle") == 0 || strcmp(mode, "minimized") == 0 ||
        strcmp(mode, "background") == 0)
        return hang(ddraw, mode);

    if (strcmp(mode, "handled") == 0)
    {
        BOOL r = IsBadReadPtr((void*)0x10, 4);
        BOOL w = IsBadWritePtr((void*)0x10, 4);
        printf("handled: IsBadReadPtr %d, IsBadWritePtr %d\n", r, w);
        Sleep(3000);            /* past the reporter's quiet start, so its log line is there */
        return 0;
    }

    if (strcmp(mode, "deadline") == 0)
    {
        SetUnhandledExceptionFilter(deadline_filter);
        g_lock_loader = (LDRLOCKLOADERLOCK)GetProcAddress(GetModuleHandleA("ntdll.dll"), "LdrLockLoaderLock");

        if (!g_lock_loader)
        {
            put("deadline: no LdrLockLoaderLock\n");
            return 2;
        }

        g_a_locked = CreateEventA(NULL, TRUE, FALSE, NULL);
        g_b_ready = CreateEventA(NULL, TRUE, FALSE, NULL);
        HANDLE b = CreateThread(NULL, 0, deadline_b, NULL, 0, (DWORD*)&g_b_tid);
        HANDLE a = CreateThread(NULL, 0, deadline_a, NULL, 0, (DWORD*)&g_a_tid);
        (void)a;
        WaitForSingleObject(b, INFINITE);
        return 0;
    }

    if (strcmp(mode, "abandon") == 0)
    {
        SetUnhandledExceptionFilter(abandon_filter);
        g_a_clobbered = CreateEventA(NULL, TRUE, FALSE, NULL);
        CreateThread(NULL, 1 << 20, abandon_a, NULL, 0, (DWORD*)&g_a_tid);

        if (WaitForSingleObject(g_a_clobbered, 40000) != WAIT_OBJECT_0)
        {
            put("abandon: A never resumed\n");
            return 52;
        }

        Sleep(5000);            /* the helper, unstuck, finishes the minidump */
        return check_dump(g_a_fault_address);
    }

    if (strcmp(mode, "exitthread") == 0)
        SetUnhandledExceptionFilter(exit_thread_filter);
    else if (strcmp(mode, "overflow-loud") == 0)
        SetUnhandledExceptionFilter(game_filter);

    if (strcmp(mode, "caught-heap") == 0)
    {
        AddVectoredExceptionHandler(0, catch_and_exit);
        Sleep(3000);            /* the reporter is past its quiet start */
        g_heap_held = CreateEventA(NULL, TRUE, FALSE, NULL);
        CreateThread(NULL, 0, hold_heap, NULL, 0, NULL);
        WaitForSingleObject(g_heap_held, INFINITE);
        put("caught-heap: heap held by another thread; crashing\n");
        return outer(argc);
    }

    if (strcmp(mode, "caught") == 0)
    {
        /* Last in line, as a game's own SEH __except is (vectored handlers all run before any frame
         * handler). Crash at once: the reporter thread has had no chance to log the breadcrumb, so
         * what reaches lomhd.log came from DLL_PROCESS_DETACH. */
        AddVectoredExceptionHandler(0, catch_and_exit);
        return outer(argc);
    }

    if (strcmp(mode, "freelib") == 0)
    {
        /* The reporter pins ddraw.dll: after this FreeLibrary its filter must still be mapped. */
        FreeLibrary(ddraw);
        printf("freelib: ddraw.dll still loaded after FreeLibrary: %s\n", GetModuleHandleA("ddraw.dll") ? "yes" : "no");
        SetUnhandledExceptionFilter(game_filter);
    }
    else if (strcmp(mode, "av") == 0 || strcmp(mode, "noflag") == 0)
        SetUnhandledExceptionFilter(game_filter);
    else if (strcmp(mode, "overflow") == 0 || strcmp(mode, "overflow-noflag") == 0)
        SetUnhandledExceptionFilter(game_filter_quiet);
    else if (strcmp(mode, "chain") == 0)
        g_game_prev = SetUnhandledExceptionFilter(game_filter_chaining);
    else if (strcmp(mode, "continue") == 0)
        SetUnhandledExceptionFilter(resuming_filter);
    else if (strcmp(mode, "bypass") == 0)
        g_bypass_prev = real_set_filter()(bypass_filter);
    else if (strcmp(mode, "bypass-chain") == 0)
    {
        SetUnhandledExceptionFilter(game_filter);
        g_bypass_chains = TRUE;
        g_bypass_prev = real_set_filter()(bypass_filter);
    }
    else if (strcmp(mode, "bypass-restore") == 0)
    {
        /* Installed around the patch, re-asserted over, then uninstalled by restoring what it
         * saved: it must not run at the crash. */
        SetUnhandledExceptionFilter(game_filter);
        g_bypass_prev = real_set_filter()(bypass_filter);
        Sleep(3500);
        real_set_filter()(g_bypass_prev);
        printf("bypass-restore: restored %p\n", (void*)g_bypass_prev);
    }
    else if (strcmp(mode, "none") != 0 && strcmp(mode, "exitthread") != 0 && strcmp(mode, "overflow-loud") != 0)
    {
        printf("unknown mode\n");
        return 2;
    }

    /* Long enough for the reporter thread to finish its quiet start (1.5 s), hash the exe and
     * re-assert (once a second after that). */
    Sleep(3500);
    printf("crashing now\n");

    if (strncmp(mode, "overflow", 8) == 0)
        return recurse(0);

    if (strcmp(mode, "exitthread") == 0)
    {
        outer(argc);
        return 0;
    }

    int r = outer(argc);

    if (strcmp(mode, "continue") == 0)
    {
        printf("continue: resumed after the fault (%d)\n", r);
        Sleep(1500);            /* the helper appends the outcome and logs it */
        return 47;
    }

    return r;
}
