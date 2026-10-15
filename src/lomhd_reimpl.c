#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "lomhd.h"
#include "lomhd_reimpl.h"
#include "reimpl.h"

/* Redirects rewritten lomse.exe functions to their C versions in src/reimpl by overwriting the
 * first five bytes of each original with `jmp rel32`. Nothing is patched unless ALL hold:
 *   1. a folder lomhd_reimpl\ exists beside the exe (the flag; absent = this file does nothing);
 *   2. the image at 0x400000 is the known lomse.exe (PE timestamp and SizeOfImage);
 *   3. the whole original function hashes to the table's FNV-1a (else that entry is skipped);
 *   4. the entry is enabled: a file named `all` in the folder enables every entry, or a file named
 *      like the address (`0x4026a0`, lower case) enables just that one.
 * Each reimpl bumps lom_reimpl_calls[i]; the counts are logged every 10 s while they change and at
 * exit. Reimpls call un-rewritten originals by address and never the function they replace, so a
 * redirect cannot recurse. */

typedef struct {
    uint32_t addr, len, fnv;
    const char *name;
    void *fn;
    int counter;
} Entry;

#define ROW(a, l, h, n, f, c) { a, l, h, n, (void *)f, c },
static const Entry g_table[] = { LOMHD_REIMPL_LIST(ROW) };
#undef ROW

static volatile LONG g_patched;
static int g_patched_flag[LOMHD_REIMPL_COUNT];
static int32_t g_last[LOMHD_REIMPL_COUNT];

static void logf1(const char *fmt, unsigned a)
{
    char line[160];

    _snprintf(line, sizeof(line), fmt, a);
    line[sizeof(line) - 1] = 0;
    lomhd_log(line);
}

static BOOL file_exists(const char *dir, const char *name)
{
    char path[MAX_PATH];

    _snprintf(path, sizeof(path), "%s%s", dir, name);
    path[sizeof(path) - 1] = 0;
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

void lomhd_reimpl_install(void)
{
    static volatile LONG done;

    if (InterlockedExchange(&done, 1))
        return;

    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, sizeof(exe));
    char *slash = NULL;

    for (DWORD i = 0; i < n && i < sizeof(exe); i++)
        if (exe[i] == '\\' || exe[i] == '/')
            slash = &exe[i];

    if (!slash)
        return;

    slash[1] = 0;

    char dir[MAX_PATH];
    _snprintf(dir, sizeof(dir), "%slomhd_reimpl\\", exe);
    dir[sizeof(dir) - 1] = 0;

    DWORD attributes = GetFileAttributesA(dir);

    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
        return;

    lomhd_log("reimpl: lomhd_reimpl folder found");

    if (!lomhd_reimpl_header_ok((const uint8_t *)LOMHD_EXE_BASE, 0x1000))
    {
        lomhd_log("reimpl: not the known lomse.exe (header differs) -- nothing patched");
        return;
    }

    BOOL all = file_exists(dir, "all");

    for (size_t i = 0; i < sizeof(g_table) / sizeof(g_table[0]); i++)
    {
        const Entry *e = &g_table[i];
        char name[16];

        _snprintf(name, sizeof(name), "0x%x", (unsigned)e->addr);

        if (!all && !file_exists(dir, name))
        {
            logf1("reimpl: 0x%x not enabled", e->addr);
            continue;
        }

        BYTE *code = (BYTE *)(uintptr_t)e->addr;

        if (!lomhd_reimpl_code_ok(code, e->len, e->fnv))
        {
            logf1("reimpl: skipped 0x%x: bytes differ", e->addr);
            continue;
        }

        BYTE jmp[5];
        DWORD old;

        lomhd_reimpl_make_jmp(jmp, e->addr, (uint32_t)(uintptr_t)e->fn);

        if (!VirtualProtect(code, 5, PAGE_EXECUTE_READWRITE, &old))
        {
            logf1("reimpl: skipped 0x%x: VirtualProtect failed", e->addr);
            continue;
        }

        memcpy(code, jmp, 5);
        FlushInstructionCache(GetCurrentProcess(), code, 5);
        VirtualProtect(code, 5, old, &old);
        g_patched_flag[e->counter] = 1;
        InterlockedIncrement(&g_patched);
        logf1("reimpl: patched 0x%x ok", e->addr);
    }
}

int lomhd_reimpl_active(void)
{
    return g_patched != 0;
}

/* Logs the call counts. At exit every patched function; from the periodic caller only when a
 * count has changed since the last line. */
static void report(BOOL only_changed)
{
    for (size_t i = 0; i < sizeof(g_table) / sizeof(g_table[0]); i++)
    {
        const Entry *e = &g_table[i];
        int32_t c = lom_reimpl_calls[e->counter];

        if (!g_patched_flag[e->counter] || (only_changed && c == g_last[e->counter]))
            continue;

        g_last[e->counter] = c;

        char line[120];
        _snprintf(line, sizeof(line), "reimpl: 0x%x %s called %ld times", (unsigned)e->addr, e->name, (long)c);
        line[sizeof(line) - 1] = 0;
        lomhd_log(line);
    }
}

void lomhd_reimpl_report(void)
{
    if (g_patched)
        report(FALSE);
}

void lomhd_reimpl_tick(void)
{
    static DWORD last;
    DWORD now = GetTickCount();

    if (g_patched && now - last >= 10000)
    {
        last = now;
        report(TRUE);
    }
}
