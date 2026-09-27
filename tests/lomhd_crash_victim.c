/* A program that loads the built ddraw.dll and misbehaves on purpose, to exercise the crash and hang
 * reporter (src/lomhd_crash.c) under Wine or Windows. A test tool only: never shipped. Run it through
 * tests/lomhd_crash_victim.sh, which asserts each mode's exit code and report files.
 *
 * Its filters stand in for a game's: game_filter is installed through the victim's import table
 * (the path cnc-ddraw patches), the "bypass" ones through GetProcAddress, around the patch. Each
 * filter that ends the process does so with its own exit code, so the exit code says which ran:
 *   3  a vectored handler caught the fault and exited (mode caught)
 *   43 game_filter called its predecessor, which returned (mode chain)
 *   44 game_filter ran
 *   45 bypass_filter ran
 *   46 bypass_filter called its predecessor, which returned without ending the process
 *   47 execution resumed after the fault (mode continue)
 *   0  no fault, or the hang modes ran to the end */
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

static LONG WINAPI catch_and_exit(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
    {
        printf("caught: %08lx, calling ExitProcess\n", ep->ExceptionRecord->ExceptionCode);
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

    BOOL idle = strcmp(mode, "idle") == 0;
    printf("%s: %s for 40 s\n", mode, idle ? "pumping, no DirectDraw calls" : "not pumping");

    if (idle)
        pump(40000);
    else
        Sleep(40000);

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

    if (strcmp(mode, "hang") == 0 || strcmp(mode, "idle") == 0 || strcmp(mode, "minimized") == 0 ||
        strcmp(mode, "background") == 0)
        return hang(ddraw, mode);

    if (strcmp(mode, "handled") == 0)
    {
        BOOL r = IsBadReadPtr((void*)0x10, 4);
        BOOL w = IsBadWritePtr((void*)0x10, 4);
        printf("handled: IsBadReadPtr %d, IsBadWritePtr %d\n", r, w);
        Sleep(1500);
        return 0;
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
        Sleep(2500);
        real_set_filter()(g_bypass_prev);
        printf("bypass-restore: restored %p\n", (void*)g_bypass_prev);
    }
    else if (strcmp(mode, "none") != 0)
    {
        printf("unknown mode\n");
        return 2;
    }

    /* Long enough for the reporter thread to hash the exe and re-assert. */
    Sleep(2500);
    printf("crashing now\n");

    if (strncmp(mode, "overflow", 8) == 0)
        return recurse(0);

    int r = outer(argc);

    if (strcmp(mode, "continue") == 0)
    {
        printf("continue: resumed after the fault (%d)\n", r);
        Sleep(1500);            /* the helper appends the outcome and logs it */
        return 47;
    }

    return r;
}
