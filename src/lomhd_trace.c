#include <windows.h>
#include <stdio.h>
#include "lomhd.h"

/* A record of the game's surface traffic, for designing the HD terrain composite. Off unless a file
 * named lomhd_trace sits beside the game. Each distinct call (operation, surfaces, rectangle,
 * position, flags) is one row with a count, so a run of any length stays a few hundred lines: the
 * rows new or grown in the last five seconds are appended to lomhd_trace.log. Called on the game's
 * threads, so it takes its own lock and never the renderer's. */

#define TRACE_ROWS 1024
#define TRACE_SURFACES 256
#define TRACE_EVERY_MS 5000

typedef struct
{
    char op;
    int dst, src;
    RECT r;
    long x, y;
    DWORD flags;
    DWORD count, logged;
} TRACE_ROW;

static TRACE_ROW g_rows[TRACE_ROWS];
static int g_row_count;
static DWORD g_dropped;
static const void* g_surface[TRACE_SURFACES];
static int g_surface_count;
static volatile LONG g_state;        /* 0 unknown, 1 off, 2 on */
static volatile LONG g_lock;
static DWORD g_last_dump;

static void trace_path(char* out, size_t size)
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

    _snprintf(out, size, "%s\\%s", exe, "lomhd_trace");
    out[size - 1] = 0;
}

static BOOL trace_on(void)
{
    if (g_state == 0)
    {
        char path[MAX_PATH];
        trace_path(path, sizeof(path));
        InterlockedExchange(&g_state, GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES ? 2 : 1);
    }

    return g_state == 2;
}

static void trace_lock(void)
{
    while (InterlockedCompareExchange(&g_lock, 1, 0) != 0)
        Sleep(0);
}

static void trace_unlock(void)
{
    InterlockedExchange(&g_lock, 0);
}

static void trace_write(const char* text, int len)
{
    char path[MAX_PATH + 8];
    trace_path(path, MAX_PATH);
    strcat(path, ".log");

    HANDLE f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
        OPEN_ALWAYS, 0, NULL);

    if (f == INVALID_HANDLE_VALUE)
        return;

    DWORD written;
    WriteFile(f, text, (DWORD)len, &written, NULL);
    CloseHandle(f);
}

/* Lock held. Surfaces are numbered in order of creation; 0 is none, -1 one made before tracing. */
static int trace_id(const void* s)
{
    if (!s)
        return 0;

    for (int i = g_surface_count - 1; i >= 0; i--)
        if (g_surface[i] == s)
            return i + 1;

    return -1;
}

/* Lock held. */
static void trace_dump_if_due(void)
{
    DWORD now = GetTickCount();

    if (now - g_last_dump < TRACE_EVERY_MS)
        return;

    g_last_dump = now;

    char buf[160];
    int len = _snprintf(buf, sizeof(buf), "-- t=%lu rows=%d dropped=%lu\r\n", now, g_row_count,
        g_dropped);
    trace_write(buf, len);

    for (int i = 0; i < g_row_count; i++)
    {
        TRACE_ROW* w = &g_rows[i];

        if (w->count == w->logged)
            continue;

        len = _snprintf(buf, sizeof(buf), "%c s%d <- s%d rect %ld,%ld,%ld,%ld at %ld,%ld flags %lx n=%lu (+%lu)\r\n",
            w->op, w->dst, w->src, w->r.left, w->r.top, w->r.right, w->r.bottom, w->x, w->y,
            w->flags, w->count, w->count - w->logged);

        if (len < 0 || len >= (int)sizeof(buf))
            len = (int)sizeof(buf) - 1;

        trace_write(buf, len);
        w->logged = w->count;
    }
}

void lomhd_trace_surface(const void* s, DWORD width, DWORD height, DWORD bpp, DWORD caps, LONG pitch,
    BOOL caller_memory)
{
    if (!trace_on())
        return;

    trace_lock();

    int id = -1;

    if (g_surface_count < TRACE_SURFACES)
    {
        g_surface[g_surface_count++] = s;
        id = g_surface_count;
    }

    char buf[160];
    int len = _snprintf(buf, sizeof(buf), "surface s%d %lux%lu bpp %lu caps %lx pitch %ld%s t=%lu\r\n",
        id, width, height, bpp, caps, pitch, caller_memory ? " caller-memory" : "", GetTickCount());
    trace_write(buf, len);

    trace_unlock();
}

void lomhd_trace_op(char op, const void* dst, const void* src, const RECT* r, long x, long y, DWORD flags)
{
    if (!trace_on())
        return;

    trace_lock();

    TRACE_ROW key = { 0 };
    key.op = op;
    key.dst = trace_id(dst);
    key.src = trace_id(src);

    if (r)
        key.r = *r;
    else
        SetRect(&key.r, -1, -1, -1, -1);

    key.x = x;
    key.y = y;
    key.flags = flags;

    int i;

    for (i = 0; i < g_row_count; i++)
    {
        TRACE_ROW* w = &g_rows[i];

        if (w->op == key.op && w->dst == key.dst && w->src == key.src && EqualRect(&w->r, &key.r) &&
            w->x == key.x && w->y == key.y && w->flags == key.flags)
            break;
    }

    if (i < g_row_count)
        g_rows[i].count++;
    else if (g_row_count < TRACE_ROWS)
    {
        key.count = 1;
        g_rows[g_row_count++] = key;
    }
    else
        g_dropped++;

    trace_dump_if_due();
    trace_unlock();
}
