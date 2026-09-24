#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "lomhd.h"

/* Serves the HD terrain art from a plain folder instead of pic.mpq. The engine has no override:
 * every archive read goes through one loader (0x4fe720) that asks Storm for a member of one
 * archive by handle, and loose files are only a fallback after a miss, which terrain reads never
 * allow (static analysis, 2026-09-24). So a release would have to rewrite pic.mpq -- hundreds of
 * MB, no Python writer, a full backup. Instead this answers the game's own Storm calls for til\*
 * members from lomhd_terrain\til\ beside the exe, by replacing four import slots.
 *
 * It switches on only when the hybrid exe is running (lomhd_terrain_active) AND the folder exists.
 * If Steam restores the original exe, the art is simply not served, and the stock terrain draws. */

/* lomse.exe GS5R3: STORM.dll's import slots, reached through jmp thunks at 0x5338d6..0x5338e8. */
#define SLOT_OPEN_FILE_EX 0x54d21c   /* ordinal 268 SFileOpenFileEx(archive, name, scope, &file) */
#define SLOT_GET_FILE_SIZE 0x54d274  /* ordinal 265 SFileGetFileSize(file, &high) */
#define SLOT_READ_FILE 0x54d220      /* ordinal 269 SFileReadFile(file, buf, n, &read, overlapped) */
#define SLOT_CLOSE_FILE 0x54d264     /* ordinal 253 SFileCloseFile(file) */

typedef BOOL (WINAPI* OPEN_FILE_EX)(HANDLE, const char*, DWORD, HANDLE*);
typedef DWORD (WINAPI* GET_FILE_SIZE)(HANDLE, DWORD*);
typedef BOOL (WINAPI* READ_FILE)(HANDLE, void*, DWORD, DWORD*, void*);
typedef BOOL (WINAPI* CLOSE_FILE)(HANDLE);

static OPEN_FILE_EX real_open;
static GET_FILE_SIZE real_size;
static READ_FILE real_read;
static CLOSE_FILE real_close;

static char g_dir[MAX_PATH];             /* ...\lomhd_terrain\ */
static volatile LONG g_served;

/* Our handles, so a Storm handle is never mistaken for one: a small set, not a tag in memory the
 * hook would have to dereference. The game has one file open at a time (0x4fe720 opens, reads,
 * closes). */
#define MAX_OPEN 16
static HANDLE g_open[MAX_OPEN];
static CRITICAL_SECTION g_cs;

static int slot_of(HANDLE h)
{
    for (int i = 0; i < MAX_OPEN; i++)
        if (h && g_open[i] == h)
            return i;

    return -1;
}

/* "til\name.ext", with either slash and any case; nothing that could climb out of the folder. */
static BOOL is_terrain_member(const char* name)
{
    if (!name || _strnicmp(name, "til", 3) != 0 || (name[3] != '\\' && name[3] != '/'))
        return FALSE;

    const char* rest = name + 4;

    if (!*rest || strlen(rest) > 64)
        return FALSE;

    for (const char* p = rest; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':' || (p[0] == '.' && p[1] == '.'))
            return FALSE;

    return TRUE;
}

static BOOL WINAPI hook_open(HANDLE archive, const char* name, DWORD scope, HANDLE* out)
{
    if (out && is_terrain_member(name))
    {
        char path[MAX_PATH + 80];
        _snprintf(path, sizeof(path), "%stil\\%s", g_dir, name + 4);
        path[sizeof(path) - 1] = 0;

        HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);

        if (f != INVALID_HANDLE_VALUE)
        {
            EnterCriticalSection(&g_cs);
            int i;

            for (i = 0; i < MAX_OPEN && g_open[i]; i++)
                ;

            if (i < MAX_OPEN)
                g_open[i] = f;

            LeaveCriticalSection(&g_cs);

            if (i < MAX_OPEN)
            {
                InterlockedIncrement(&g_served);
                *out = f;
                return TRUE;
            }

            CloseHandle(f);
        }
    }

    return real_open(archive, name, scope, out);
}

static BOOL ours(HANDLE h)
{
    EnterCriticalSection(&g_cs);
    BOOL found = slot_of(h) >= 0;
    LeaveCriticalSection(&g_cs);
    return found;
}

static DWORD WINAPI hook_size(HANDLE h, DWORD* high)
{
    if (ours(h))
        return GetFileSize(h, high);

    return real_size(h, high);
}

static BOOL WINAPI hook_read(HANDLE h, void* buf, DWORD n, DWORD* got, void* overlapped)
{
    if (ours(h))
    {
        DWORD read = 0;
        BOOL ok = ReadFile(h, buf, n, &read, NULL);

        if (got)
            *got = read;

        return ok && read == n;
    }

    return real_read(h, buf, n, got, overlapped);
}

static BOOL WINAPI hook_close(HANDLE h)
{
    EnterCriticalSection(&g_cs);
    int i = slot_of(h);

    if (i >= 0)
        g_open[i] = NULL;

    LeaveCriticalSection(&g_cs);

    if (i >= 0)
        return CloseHandle(h);

    return real_close(h);
}

/* Each slot must still point where the game's jmp thunk reads it, and into STORM.dll: anything else
 * means a different exe, or someone else hooked it first. */
static BOOL slot_is_storm(DWORD slot, HMODULE storm, void** out)
{
    void* target = *(void* volatile*)slot;
    HMODULE owner = NULL;

    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)target, &owner) || owner != storm)
        return FALSE;

    *out = target;
    return TRUE;
}

static BOOL write_slot(DWORD slot, void* value)
{
    DWORD old;

    if (!VirtualProtect((void*)slot, sizeof(void*), PAGE_READWRITE, &old))
        return FALSE;

    *(void* volatile*)slot = value;
    VirtualProtect((void*)slot, sizeof(void*), old, &old);
    return TRUE;
}

/* DllMain, process attach. ddraw.dll is loaded as an import of lomse.exe, after STORM.dll has been
 * bound (both are the exe's imports; Storm has no reason to load ddraw), so the slots hold Storm's
 * addresses by now -- and slot_is_storm checks that rather than assuming it. */
void lomhd_files_install(void)
{
    /* Tried at attach and again on the first blit (long before a map loads), in case this loader
     * runs DllMain before the exe's imports are bound. Once only after it succeeds. */
    static volatile LONG done;

    if (done || !lomhd_terrain_active())
        return;

    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, sizeof(exe));
    char* slash = NULL;

    for (DWORD i = 0; i < n && i < sizeof(exe); i++)
        if (exe[i] == '\\' || exe[i] == '/')
            slash = &exe[i];

    if (!slash)
        return;

    slash[1] = 0;
    _snprintf(g_dir, sizeof(g_dir), "%slomhd_terrain\\", exe);
    g_dir[sizeof(g_dir) - 1] = 0;

    DWORD attributes = GetFileAttributesA(g_dir);

    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
        return;

    HMODULE storm = GetModuleHandleA("storm.dll");
    void *o, *s, *r, *c;

    if (!storm || !slot_is_storm(SLOT_OPEN_FILE_EX, storm, &o) || !slot_is_storm(SLOT_GET_FILE_SIZE, storm, &s) ||
        !slot_is_storm(SLOT_READ_FILE, storm, &r) || !slot_is_storm(SLOT_CLOSE_FILE, storm, &c))
        return;

    if (InterlockedExchange(&done, 1))
        return;

    InitializeCriticalSection(&g_cs);
    real_open = (OPEN_FILE_EX)o;
    real_size = (GET_FILE_SIZE)s;
    real_read = (READ_FILE)r;
    real_close = (CLOSE_FILE)c;

    /* Close, read and size first: once open is replaced, a handle of ours can reach any of them. */
    write_slot(SLOT_CLOSE_FILE, (void*)hook_close);
    write_slot(SLOT_READ_FILE, (void*)hook_read);
    write_slot(SLOT_GET_FILE_SIZE, (void*)hook_size);
    write_slot(SLOT_OPEN_FILE_EX, (void*)hook_open);
}

long lomhd_files_served(void)
{
    return g_served;
}

BOOL lomhd_files_on(void)
{
    return real_open != NULL;
}
