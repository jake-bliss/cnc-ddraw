#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "dd.h"
#include "ddsurface.h"
#include "lomhd.h"

/* Lords of Magic HD overlay -- development instrument, stage 1.
 *
 * THE ONE RULE: the render thread never touches a file. It holds g_ddraw.cs while it calls
 * lomhd_on_frame, and the first version of this file did its logging and its screenshots right
 * there. Under Wine that wedged both the render thread and a second thread writing the same log,
 * after a dozen dumps, while the game carried on. So the render thread only counts frames and,
 * when asked, copies the frame into memory. One worker thread owns every file operation: it polls
 * for the trigger, writes the frame dumps, and writes every log line. */

static HANDLE g_worker_wake;           /* set by the render thread when a frame copy is ready */
static volatile LONG g_frames;         /* frames seen since the worker last reported */
static volatile LONG g_want_frame;     /* worker -> render thread: copy the next frame */
static volatile LONG g_frame_ready;    /* render thread -> worker: the copy below is complete */
static volatile const char* g_renderer;

static BYTE* g_frame;                  /* one frame, tightly packed, owned by whoever holds the flag */
static DWORD g_frame_w, g_frame_h, g_frame_bpp;

/* Paths resolve from lomse.exe's own directory, never the working directory: an early build used
 * a relative path and its trigger was never seen. */
static void lomhd_path(char* out, size_t size, const char* name)
{
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, sizeof(exe));
    char* slash = NULL;

    for (DWORD i = 0; i < n && i < sizeof(exe); i++)
        if (exe[i] == '\\' || exe[i] == '/')
            slash = &exe[i];

    if (slash)
        *slash = 0;
    else
        exe[0] = 0;

    _snprintf(out, size, "%s\\%s", exe, name);
    out[size - 1] = 0;
}

/* Worker thread only. Shared read AND write, so a reader on either side can never block it. */
static void lomhd_log(const char* line)
{
    char path[MAX_PATH];
    lomhd_path(path, sizeof(path), "lomhd.log");

    HANDLE f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
        OPEN_ALWAYS, 0, NULL);

    if (f == INVALID_HANDLE_VALUE)
        return;

    SYSTEMTIME t;
    GetLocalTime(&t);

    char stamped[256];
    int len = _snprintf(stamped, sizeof(stamped), "%02d:%02d:%02d %s\r\n",
        t.wHour, t.wMinute, t.wSecond, line);

    if (len < 0 || len >= (int)sizeof(stamped))
        len = (int)sizeof(stamped) - 1;

    DWORD written;
    WriteFile(f, stamped, (DWORD)len, &written, NULL);
    CloseHandle(f);
}

/* Worker thread only. Format: "LOMHDRAW", then u32 width, height, bits per pixel, then the pixels,
 * rows packed with no padding. Raw rather than PNG so an analysis reads the exact frame values
 * with no decoder in between. */
static void lomhd_write_frame(void)
{
    SYSTEMTIME t;
    GetLocalTime(&t);

    char name[64];
    _snprintf(name, sizeof(name), "lomhd_frame_%02d%02d%02d_%03d.raw",
        t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    name[sizeof(name) - 1] = 0;

    char path[MAX_PATH];
    lomhd_path(path, sizeof(path), name);

    HANDLE f = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);

    if (f == INVALID_HANDLE_VALUE)
    {
        lomhd_log("frame: could not create the dump file");
        return;
    }

    DWORD header[3] = { g_frame_w, g_frame_h, g_frame_bpp };
    DWORD bytes = g_frame_w * g_frame_h * (g_frame_bpp / 8);
    DWORD written;

    WriteFile(f, "LOMHDRAW", 8, &written, NULL);
    WriteFile(f, header, sizeof(header), &written, NULL);
    WriteFile(f, g_frame, bytes, &written, NULL);
    CloseHandle(f);

    char line[160];
    _snprintf(line, sizeof(line), "frame: wrote %s (%lux%lu, %lu bpp)",
        name, g_frame_w, g_frame_h, g_frame_bpp);
    line[sizeof(line) - 1] = 0;
    lomhd_log(line);
}

static DWORD WINAPI lomhd_worker(LPVOID unused)
{
    (void)unused;

    DWORD last_report = GetTickCount();
    const char* reported_renderer = NULL;
    char trigger[MAX_PATH];
    lomhd_path(trigger, sizeof(trigger), "lomhd_dump");

    for (;;)
    {
        WaitForSingleObject(g_worker_wake, 250);

        const char* renderer = (const char*)g_renderer;

        if (renderer != reported_renderer)
        {
            char line[160];
            _snprintf(line, sizeof(line), "renderer: %s%s", renderer ? renderer : "?",
                reported_renderer ? " (CHANGED)" : "");
            line[sizeof(line) - 1] = 0;
            lomhd_log(line);
            reported_renderer = renderer;
        }

        if (InterlockedCompareExchange(&g_frame_ready, 0, 1) == 1)
            lomhd_write_frame();

        if (GetFileAttributesA(trigger) != INVALID_FILE_ATTRIBUTES)
        {
            DeleteFileA(trigger);
            InterlockedExchange(&g_want_frame, 1);
        }

        DWORD now = GetTickCount();

        if (now - last_report >= 5000)
        {
            char line[96];
            _snprintf(line, sizeof(line), "watchdog: frames=%ld want_frame=%ld",
                InterlockedExchange(&g_frames, 0), g_want_frame);
            line[sizeof(line) - 1] = 0;
            lomhd_log(line);
            last_report = now;
        }
    }

    return 0;
}

/* Render thread, g_ddraw.cs held. Memory only: no file, no log, no allocation after the first. */
static void lomhd_copy_frame_if_wanted(void)
{
    if (!g_want_frame || g_frame_ready)
        return;

    IDirectDrawSurfaceImpl* primary = g_ddraw.primary;

    if (!primary)
        return;

    DWORD bytes_pp = primary->bytes_pp;
    DWORD row = primary->width * bytes_pp;

    if (!g_frame)
        g_frame = HeapAlloc(GetProcessHeap(), 0, 1920 * 1440 * 4);

    if (!g_frame || row * primary->height > 1920 * 1440 * 4)
        return;

    BYTE* src = dds_GetBuffer(primary);

    if (!src)
        return;

    for (DWORD y = 0; y < primary->height; y++)
        memcpy(g_frame + y * row, src + y * primary->pitch, row);

    g_frame_w = primary->width;
    g_frame_h = primary->height;
    g_frame_bpp = bytes_pp * 8;

    InterlockedExchange(&g_want_frame, 0);
    InterlockedExchange(&g_frame_ready, 1);
    SetEvent(g_worker_wake);
}

void lomhd_on_frame(const char* renderer)
{
    if (!g_worker_wake)
    {
        g_worker_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
        CloseHandle(CreateThread(NULL, 0, lomhd_worker, NULL, 0, NULL));
    }

    g_renderer = renderer;
    InterlockedIncrement(&g_frames);
    lomhd_copy_frame_if_wanted();
}
