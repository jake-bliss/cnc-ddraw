/* Runs the shipped portrait matcher (src/lomhd_match.c) over raw frames captured by the wrapper.
 *
 *     lomhd_match_test.exe PACK FRAME.raw ...
 *
 * Prints every portrait found in each frame and how long the search took, so a change to the
 * matcher can be checked against real frames without a person sitting at the game. */
#include <windows.h>
#include <stdio.h>
#include "lomhd_match.h"

static BYTE* read_all(const char* path, DWORD* size)
{
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    *size = GetFileSize(f, NULL);
    BYTE* data = HeapAlloc(GetProcessHeap(), 0, *size ? *size : 1);
    DWORD got = 0;
    BOOL ok = data && ReadFile(f, data, *size, &got, NULL) && got == *size;
    CloseHandle(f);
    return ok ? data : NULL;
}

int main(int argc, char** argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s PACK FRAME.raw ...\n", argv[0]); return 2; }

    DWORD size, bad;
    BYTE* bytes = read_all(argv[1], &size);
    LOMHD_PACK pack;
    if (!bytes || !lomhd_pack_parse(bytes, size, &pack, &bad))
    { fprintf(stderr, "pack refused near byte %lu\n", bad); return 1; }
    printf("pack: %d portraits, probe width %d\n", pack.count, pack.probe_width);

    LARGE_INTEGER freq; QueryPerformanceFrequency(&freq);
    double worst = 0, total = 0; int frames = 0;

    for (int a = 2; a < argc; a++)
    {
        DWORD fsize; BYTE* f = read_all(argv[a], &fsize);
        if (!f || fsize < 20 || memcmp(f, "LOMHDRAW", 8) != 0) { printf("%s: not a frame\n", argv[a]); continue; }
        DWORD w = *(DWORD*)(f + 8), h = *(DWORD*)(f + 12), bpp = *(DWORD*)(f + 16);
        if (bpp != 16) { printf("%s: %lu bpp, skipped\n", argv[a], bpp); continue; }

        PLACEMENT out[LOMHD_MAX_PLACEMENTS];
        LARGE_INTEGER t0, t1; QueryPerformanceCounter(&t0);
        int n = lomhd_find(&pack, (const WORD*)(f + 20), (int)w, (int)h, (int)w, out, LOMHD_MAX_PLACEMENTS);
        QueryPerformanceCounter(&t1);
        double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;
        total += ms; frames++; if (ms > worst) worst = ms;

        const char* name = strrchr(argv[a], '\\'); name = name ? name + 1 : argv[a];
        printf("%s %d", name, n);
        for (int i = 0; i < n; i++)
            printf("  %s@(%d,%d)%s", pack.portraits[out[i].portrait].name, out[i].x, out[i].y, out[i].rule ? "R" : "T");
        printf("\n");
        HeapFree(GetProcessHeap(), 0, f);
    }

    printf("search time: mean %.2f ms, worst %.2f ms over %d frames\n", frames ? total / frames : 0, worst, frames);
    return 0;
}
