/* Runs the shipped matcher (src/lomhd_match.c) over raw frames captured by the wrapper.
 *
 *     lomhd_match_test PACK FRAME.raw ...
 *
 * Prints every image found in each frame and how long the search took, so a change to the matcher
 * can be checked against real frames without a person sitting at the game. Plain C file I/O and
 * clock(), so it builds natively (with a windows.h shim for the types) as well as for Windows. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "lomhd_match.h"

static BOOL file_read(void* ctx, DWORD off, DWORD len, BYTE* out)
{
    FILE* f = ctx;
    return fseek(f, (long)off, SEEK_SET) == 0 && fread(out, 1, len, f) == len;
}

static BYTE* read_all(const char* path, long* size)
{
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); *size = ftell(f); rewind(f);
    BYTE* data = malloc(*size ? *size : 1);
    BOOL ok = data && fread(data, 1, *size, f) == (size_t)*size;
    fclose(f);
    if (!ok) { free(data); return NULL; }
    return data;
}

static double now_ms(void) { return clock() * 1000.0 / CLOCKS_PER_SEC; }

int main(int argc, char** argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s PACK FRAME.raw ...\n", argv[0]); return 2; }

    FILE* pf = fopen(argv[1], "rb");
    if (!pf) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    fseek(pf, 0, SEEK_END);
    DWORD size = (DWORD)ftell(pf), bad;
    LOMHD_PACK pack;
    double t0 = now_ms();
    if (!lomhd_pack_open(file_read, pf, size, &pack, &bad))
    { fprintf(stderr, "pack refused near byte %lu\n", (unsigned long)bad); return 1; }
    size_t resident = 0; int large = 0;
    for (int i = 0; i < pack.count; i++)
    {
        const PORTRAIT* r = &pack.portraits[i];
        resident += sizeof(*r) + (r->idx ? (size_t)r->w * r->h : 0) + (size_t)r->sw * r->sh;
        large += r->idx == NULL;
    }
    printf("pack: %d images (%d large), probe width %d, opened in %.0f ms, %.1f MB resident\n",
        pack.count, large, pack.probe_width, now_ms() - t0,
        (resident + sizeof(PROBE) * LOMHD_TABLE) / 1048576.0);

    double worst = 0, total = 0; int frames = 0;

    for (int a = 2; a < argc; a++)
    {
        long fsize; BYTE* f = read_all(argv[a], &fsize);
        if (!f || fsize < 20 || memcmp(f, "LOMHDRAW", 8) != 0) { printf("%s: not a frame\n", argv[a]); continue; }
        DWORD w = *(DWORD*)(f + 8), h = *(DWORD*)(f + 12), bpp = *(DWORD*)(f + 16);
        if (bpp != 16) { printf("%s: %lu bpp, skipped\n", argv[a], (unsigned long)bpp); free(f); continue; }
        if ((unsigned long long)w * h * 2 + 20 > (unsigned long long)fsize) { printf("%s: truncated\n", argv[a]); free(f); continue; }

        PLACEMENT out[LOMHD_MAX_PLACEMENTS];
        int n = 0;
        double s = now_ms();
        for (int rep = 0; rep < 5; rep++)         /* clock() is coarse: time five searches */
            n = lomhd_find(&pack, (const WORD*)(f + 20), (int)w, (int)h, (int)w, out, LOMHD_MAX_PLACEMENTS);
        double ms = (now_ms() - s) / 5;
        total += ms; frames++; if (ms > worst) worst = ms;

        const char* name = strrchr(argv[a], '/'); name = name ? name + 1 : argv[a];
        const char* bs = strrchr(name, '\\'); name = bs ? bs + 1 : name;
        printf("%s %d", name, n);
        for (int i = 0; i < n; i++)
            printf("  %s@(%d,%d)%s", pack.portraits[out[i].portrait].name, out[i].x, out[i].y, out[i].rule ? "R" : "T");
        printf("\n");
        free(f);
    }

    printf("search time: mean %.2f ms, worst %.2f ms over %d frames\n", frames ? total / frames : 0, worst, frames);
    lomhd_pack_free(&pack);
    fclose(pf);
    return 0;
}
