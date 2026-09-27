/* A program that loads the built ddraw.dll and misbehaves on purpose, to exercise the crash and hang
 * reporter (src/lomhd_crash.c) under Wine or Windows. A test tool only: never shipped.
 *
 *     i686-w64-mingw32-gcc -O1 -o lomhd_crash_victim.exe tests/lomhd_crash_victim.c
 *     (copy it and ddraw.dll into an empty folder, create a file named lomhd_crash_reports there
 *     -- the reporter is otherwise on only for lomse.exe -- and run it with one mode)
 *
 * Modes:
 *   av        installs its own filter through its import table (the path cnc-ddraw patches), then
 *             writes through a near-NULL pointer. Expect a report, then "victim filter called".
 *   bypass    installs its filter through GetProcAddress, around the patched import, waits for the
 *             reporter to re-assert, then crashes. Expect a report, then "victim filter called".
 *   none      no filter of its own; crashes. Expect a report, then Wine's/Windows' usual crash.
 *   handled   raises access violations that are caught (IsBadReadPtr/IsBadWritePtr), then exits
 *             0. Expect NO report: a first-chance exception must never produce one.
 *   hang      makes a window, hands it to DirectDraw, pumps for two seconds, then stops pumping
 *             for 40. Expect one lomhd_hang_*.txt after about 20 s while the window is in front.
 *   idle      the same window, pumping messages but making no DirectDraw calls for 40 s, as a menu
 *             waiting for input does. Expect NO hang report: silent is not hung.
 *   minimized the hang, but with the window minimized first (through the real ShowWindow: cnc-ddraw
 *             patches the victim's import). Expect NO hang report.
 *   background the hang, but with another window in front. Expect NO hang report.
 * The victim's filter returns EXCEPTION_EXECUTE_HANDLER, so those runs end without a debugger. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef HRESULT(WINAPI* DIRECTDRAWCREATE)(GUID*, void**, IUnknown*);

static LONG WINAPI victim_filter(EXCEPTION_POINTERS* ep)
{
    printf("victim filter called: %08lx at %p\n", ep->ExceptionRecord->ExceptionCode,
        ep->ExceptionRecord->ExceptionAddress);
    fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}

/* A few frames of the victim's own on the stack, so the report has return addresses to annotate. */
static __attribute__((noinline)) int deepest(volatile int* p, int v)
{
    *p = v;
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
        printf("hang: setup failed\n");
        return 2;
    }

    /* IDirectDraw::SetCooperativeLevel is slot 20 of the vtable. */
    typedef HRESULT(WINAPI* SETCOOP)(void*, HWND, DWORD);
    SETCOOP set_coop = (SETCOOP)((void**)*dd)[20];
    HRESULT hr = set_coop(dd, hwnd, 8 /* DDSCL_NORMAL */);

    SetForegroundWindow(hwnd);
    pump(2000);

    if (strcmp(mode, "minimized") == 0)
    {
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
    printf("%s: SetCooperativeLevel %08lx, foreground %s, minimized %s; %s for 40 s\n", mode,
        (unsigned long)hr, GetForegroundWindow() == hwnd ? "yes" : "no", IsIconic(hwnd) ? "yes" : "no",
        idle ? "pumping, no DirectDraw calls" : "not pumping");
    fflush(stdout);

    if (idle)
        pump(40000);
    else
        Sleep(40000);

    /* The real GetForegroundWindow: cnc-ddraw patches the victim's import to answer with the game
     * window, which is what the first line above printed (found 2026-09-27; the watchdog in
     * ddraw.dll calls the real one and saw the other window in front). */
    HWND(WINAPI * foreground)(void) = (void*)GetProcAddress(GetModuleHandleA("user32.dll"), "GetForegroundWindow");
    printf("%s: after 40 s, game window really in front %s, minimized %s\n", mode,
        foreground() == hwnd ? "yes" : "no", IsIconic(hwnd) ? "yes" : "no");
    pump(500);
    printf("hang: done\n");
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

    char path[MAX_PATH];
    GetModuleFileNameA(ddraw, path, sizeof(path));
    printf("loaded %s, mode %s\n", path, mode);

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

    if (strcmp(mode, "av") == 0)
    {
        LPTOP_LEVEL_EXCEPTION_FILTER prev = SetUnhandledExceptionFilter(victim_filter);
        printf("av: filter set through the import table, previous %p\n", prev);
    }
    else if (strcmp(mode, "bypass") == 0)
    {
        LPTOP_LEVEL_EXCEPTION_FILTER(WINAPI * set)(LPTOP_LEVEL_EXCEPTION_FILTER) =
            (void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetUnhandledExceptionFilter");
        LPTOP_LEVEL_EXCEPTION_FILTER prev = set(victim_filter);
        printf("bypass: filter set around the import table, previous %p\n", prev);
    }

    /* Long enough for the reporter thread to hash the exe, snapshot modules and re-assert. */
    Sleep(2500);
    printf("crashing now\n");
    return outer(argc);
}
