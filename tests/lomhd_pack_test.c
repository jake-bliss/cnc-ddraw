/* The pack parser's refusal paths, asserted. The pack is untrusted input: every malformed shape
 * here must be refused, and a valid one accepted. First written after cross-model review built a
 * 140 KB pack that the first parser accepted with a 65535x65535 upscale inside it; rewritten for
 * format 3, whose upscales are read lazily and whose inflation is bounded. */
#include <stdio.h>
#include "lomhd_test_pack.h"
#include "lodepng.h"

static int failures;
static BYTE idx_a[70 * 67], idx_b[70 * 67];

static void check(const char* what, BOOL ok)
{
    printf("%-66s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

static BOOL opens(DWORD size)
{
    LOMHD_PACK pack; TP_MEM mem;
    BOOL ok = tp_open(size, &pack, &mem);
    lomhd_pack_free(&pack);
    return ok;
}

static void two(void)
{
    tp_begin(); tp_add("a", 70, 67, idx_a); tp_add("b", 70, 67, idx_b);
}

int main(void)
{
    for (int i = 0; i < 70 * 67; i++) { idx_a[i] = (BYTE)(i * 13 + 1); idx_b[i] = (BYTE)(i * 7 + 3); }

    two(); DWORD full = tp_finish();
    check("a valid two-image pack is accepted", opens(full));

    {
        LOMHD_PACK pack; TP_MEM mem;
        BOOL ok = tp_open(full, &pack, &mem);
        BYTE* hd = ok ? lomhd_load_upscale(&pack, 1, tp_read, &mem) : NULL;
        BYTE* idx = ok ? lomhd_load_indices(&pack, 0, tp_read, &mem) : NULL;
        check("its upscale and indices load at their exact sizes",
            hd && idx && memcmp(idx, idx_a, sizeof idx_a) == 0 && pack.portraits[0].idx != NULL);
        free(hd); free(idx);
        lomhd_pack_free(&pack);
    }

    int truncations_accepted = 0;
    for (DWORD cut = 0; cut < full; cut += (cut < 2000 ? 1 : 97))
        truncations_accepted += opens(cut);
    check("a pack truncated anywhere is refused", truncations_accepted == 0);

    two(); tp_finish(); tp_buf[tp_len++] = 0;
    check("one trailing byte is refused", !opens(tp_len));

    tp_begin(); tp_finish();
    check("a count of zero is refused", !opens(tp_len));

    two(); tp_finish(); *(DWORD*)(tp_buf + 8) = 0xFFFFFFFFu;
    check("a count too large for the probe table is refused", !opens(tp_len));

    tp_begin(); tp_add("a", LOMHD_PROBE_W - 1, 67, idx_a); tp_finish();
    check("an original narrower than the probe slice is refused", !opens(tp_len));

    tp_begin(); tp_add("a", 70, 3, idx_a); tp_finish();
    check("an original too short for three probe rows is refused", !opens(tp_len));

    tp_begin(); tp_add("a", 70, 67, idx_a)->hw = LOMHD_MAX_HD_SIDE + 1; tp_finish();
    check("an upscale wider than the limit is refused", !opens(tp_len));

    tp_begin(); tp_add("a", 70, 67, idx_a)->idx_bytes = 70 * 67 - 1; tp_finish();
    check("an index stream one byte short is refused", !opens(tp_len));

    tp_begin(); tp_add("a", 70, 67, idx_a)->idx_bytes = 70 * 67 + 1; tp_finish();
    check("an index stream one byte long is refused", !opens(tp_len));

    tp_begin(); tp_add("a", 70, 67, idx_a)->idx_bytes = 4 << 20; tp_finish();
    check("an index stream inflating to 4 MB is refused", !opens(tp_len));

    two(); tp_finish();
    {
        /* The first stream is a's indices: its last byte is the adler32's last byte. */
        DWORD index_end = 12 + 2 * (2 + 8 + 768 + 8);
        DWORD idx_len = *(DWORD*)(tp_buf + 12 + 2 + 8 + 768);
        tp_buf[index_end + idx_len - 1] ^= 0xFF;
        check("an index stream failing its checksum is refused", !opens(tp_len));
    }

    two(); tp_finish();
    {
        /* Lengths that no longer add up to the file: a's index stream claims one byte more. */
        (*(DWORD*)(tp_buf + 12 + 2 + 8 + 768))++;
        check("stream lengths that do not add up to the file are refused", !opens(tp_len));
    }

    /* An upscale is only read when it is drawn: a damaged one opens, then fails to load. */
    tp_begin(); tp_add("a", 70, 67, idx_a)->hd_bytes = 140 * 134 * 3 - 3; tp_finish();
    {
        LOMHD_PACK pack; TP_MEM mem;
        BOOL ok = tp_open(tp_len, &pack, &mem);
        BYTE* hd = ok ? lomhd_load_upscale(&pack, 0, tp_read, &mem) : NULL;
        check("a short upscale stream opens but will not load", ok && !hd);
        free(hd);
        lomhd_pack_free(&pack);
    }

    tp_begin(); tp_add("a", 70, 67, idx_a)->hd_bytes = 6 << 20; tp_finish();
    {
        LOMHD_PACK pack; TP_MEM mem;
        BOOL ok = tp_open(tp_len, &pack, &mem);
        BYTE* hd = ok ? lomhd_load_upscale(&pack, 0, tp_read, &mem) : NULL;
        check("an upscale stream inflating to 6 MB will not load", ok && !hd);
        free(hd);
        lomhd_pack_free(&pack);
    }

    {
        static BYTE big[640 * 480];
        for (int i = 0; i < 640 * 480; i++) big[i] = (BYTE)((i * 2654435761u) >> 24);
        tp_begin(); tp_add("screen", 640, 480, big); tp_finish();
        LOMHD_PACK pack; TP_MEM mem;
        BOOL ok = tp_open(tp_len, &pack, &mem);
        const PORTRAIT* r = ok ? &pack.portraits[0] : NULL;
        check("a 640x480 screen keeps only its sample resident",
            ok && r->idx == NULL && r->sample && r->sw == 160 && r->sh == 120 && r->sample[1] == big[4]);
        lomhd_pack_free(&pack);
    }

    {
        /* The bound itself, not the size check after it: the cases above are refused by the exact
         * size check even with no bound, so they cannot catch a stream that exhausts a 32-bit
         * process while inflating. (Claude review, 2026-09-23: a mutant with no bound passed.) */
        static BYTE z[(1 << 20) + 1024];
        DWORD zl = tp_zlib(z, NULL, 1 << 20);
        unsigned char* out = NULL; size_t got = 0;
        unsigned err = lodepng_zlib_decompress_bounded(&out, &got, z, zl, 4096);
        check("a 1 MB stream bounded at 4 KB stops at the bound", err == 83 && got <= 4096);
        free(out);
    }

    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
