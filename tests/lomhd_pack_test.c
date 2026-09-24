/* The pack parser's refusal paths, asserted. The pack is untrusted input: every malformed shape
 * here must be refused, and a valid one accepted. First written after cross-model review built a
 * 140 KB pack that the first parser accepted with a 65535x65535 upscale inside it; rewritten for
 * format 3, whose upscales are read lazily and whose inflation is bounded; format 4 adds sprites,
 * format 5 animated and mirrored ones. */
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

/* tp_buf with `gap` bytes of zeros inserted at `split`: a pack larger than memory. */
typedef struct { DWORD split, gap; } BIG_MEM;

static BOOL big_read(void* ctx, DWORD off, DWORD len, BYTE* out)
{
    BIG_MEM* m = ctx;
    for (DWORD i = 0; i < len; i++)
    {
        unsigned long long at = (unsigned long long)off + i;
        if (at < m->split) out[i] = tp_buf[at];
        else if (at < (unsigned long long)m->split + m->gap) out[i] = 0;
        else if (at - m->gap < tp_len) out[i] = tp_buf[at - m->gap];
        else return FALSE;
    }
    return TRUE;
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
    check("a count over LOMHD_MAX_IMAGES is refused", !opens(tp_len));

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
        DWORD index_end = 12 + 2 * (2 + 8 + 2 + 2 + 768 + 8);
        DWORD idx_len;
        memcpy(&idx_len, tp_buf + 12 + 2 + 8 + 2 + 2 + 768, 4);
        tp_buf[index_end + idx_len - 1] ^= 0xFF;
        check("an index stream failing its checksum is refused", !opens(tp_len));
    }

    two(); tp_finish();
    {
        /* Lengths that no longer add up to the file: a's index stream claims one byte more. */
        DWORD len;
        memcpy(&len, tp_buf + 12 + 2 + 8 + 2 + 2 + 768, 4);
        len++;
        memcpy(tp_buf + 12 + 2 + 8 + 2 + 2 + 768, &len, 4);
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

    /* What the loader asks before opening: this build's format opens, any other LOMHDPK version is
     * "made by an older setup", anything else is not a pack. */
    {
        two(); tp_finish();
        BOOL current = lomhd_pack_version(tp_buf, 8) == LOMHD_PACK_CURRENT && opens(tp_len);
        BYTE older[8]; memcpy(older, "LOMHDPK3", 8);
        check("the loader's version check passes the pack this reader opens", current);
        check("... names an older LOMHDPK version as one", lomhd_pack_version(older, 8) == LOMHD_PACK_OTHER_VERSION);
        check("... and anything else as no pack", lomhd_pack_version((const BYTE*)"PNG....x", 8) == LOMHD_PACK_UNKNOWN &&
            lomhd_pack_version(tp_buf, 7) == LOMHD_PACK_UNKNOWN);
    }

    /* Sprites. A sprite is probed on LOMHD_SPRITE_PROBE_W-pixel opaque runs; indices 0 (its key
     * here) and 1 (the shadow) are not part of it. */
    {
        static BYTE s[24 * 20];
        for (int i = 0; i < 24 * 20; i++) s[i] = (BYTE)(i * 13 % 120 + 4);

        tp_begin(); tp_add_sprite("s", 24, 20, s, 0); tp_finish();
        LOMHD_PACK pack; TP_MEM mem;
        BOOL ok = tp_open(tp_len, &pack, &mem);
        BYTE* hd = ok ? lomhd_load_upscale(&pack, 0, tp_read, &mem) : NULL;
        check("a sprite opens and its upscale loads as RGBA", ok && hd && pack.portraits[0].masked &&
            pack.sprites == 1 && pack.portraits[0].opaque == 24 * 20);
        free(hd);
        lomhd_pack_free(&pack);

        tp_begin(); tp_add_sprite("s", 24, 20, s, 0)->hd_bytes = 48 * 40 * 3; tp_finish();
        ok = tp_open(tp_len, &pack, &mem);
        hd = ok ? lomhd_load_upscale(&pack, 0, tp_read, &mem) : NULL;
        check("a sprite's upscale the size of an RGB one will not load", ok && !hd);
        free(hd);
        lomhd_pack_free(&pack);

        /* Opaque runs, W = LOMHD_SPRITE_PROBE_W: rows 0-1 hold a W-pixel run, the rest only W - 1,
         * broken by the key. Row 2 is then given its break, at column W - 1 (a run of W - 1) or W
         * (a run of W), made of `gap`. The sprite is W + 4 wide, so nothing right of a break is W. */
        #define SW LOMHD_SPRITE_PROBE_W
        static BYTE runs[(SW + 4) * 20];
        #define RUNS(key, col, gap) do { \
            for (int j = 0; j < 20; j++) \
                for (int i = 0; i < SW + 4; i++) \
                    runs[j * (SW + 4) + i] = (BYTE)(i == (j < 2 ? SW : SW - 1) ? (key) : i * 7 % 100 + 10); \
            for (int i = 0; i < SW + 4; i++) runs[2 * (SW + 4) + i] = (BYTE)(i == (col) ? (gap) : i * 7 % 100 + 10); \
            tp_begin(); tp_add_sprite("s", SW + 4, 20, runs, key); tp_finish(); } while (0)

        RUNS(0, SW, 0);
        check("a sprite with three rows holding a full-probe opaque run is accepted", opens(tp_len));
        RUNS(0, SW - 1, 0);
        check("a sprite with only two such rows is refused", !opens(tp_len));
        RUNS(5, SW, 1);
        check("... with key 5, a row broken at W by the shadow index still counts", opens(tp_len));
        RUNS(5, SW - 1, 1);
        check("... and one broken at W - 1 by the shadow index does not", !opens(tp_len));
        RUNS(5, SW - 1, 0);
        check("... while index 0 is opaque when it is not the key", opens(tp_len));

        tp_begin(); tp_add_sprite("s", SW - 1, 20, s, 0); tp_finish();
        check("a sprite narrower than its probe is refused", !opens(tp_len));

        tp_begin(); tp_add("a", 70, 67, idx_a)->flags = 4; tp_finish();
        check("an unknown flag bit is refused", !opens(tp_len));

        tp_begin(); tp_add("a", 70, 67, idx_a)->flags = LOMHD_FLAG_MIRROR; tp_finish();
        check("a picture marked mirror is refused", !opens(tp_len));

        tp_begin(); tp_add("a", 70, 67, idx_a)->group = 1; tp_finish();
        check("a picture in a group is refused", !opens(tp_len));

        tp_begin(); tp_add("a", 70, 67, idx_a)->key = 3; tp_finish();
        check("a picture with a colour key is refused", !opens(tp_len));

        static BYTE big[260 * 260];
        for (int i = 0; i < 260 * 260; i++) big[i] = (BYTE)(i % 97 + 4);
        tp_begin(); tp_add_sprite("big", 260, 260, big, 0)->hw = 520; tp_finish();
        check("a sprite larger than a large picture's threshold is refused", !opens(tp_len));

        /* Groups: an animated sprite's frames, consecutive. Frames 1-3 of group 7, then a picture,
         * then group 9. The same group again after another record is refused. */
        tp_begin();
        tp_add_frame("g7a", 24, 20, s, 0, 7, TRUE);
        tp_add_frame("g7b", 24, 20, s, 0, 7, TRUE);
        tp_add_frame("g7c", 24, 20, s, 0, 7, TRUE);
        tp_add("a", 70, 67, idx_a);
        tp_add_frame("g9a", 24, 20, s, 0, 9, FALSE);
        tp_finish();
        ok = tp_open(tp_len, &pack, &mem);
        check("a group's frames know its first record and its size", ok &&
            pack.portraits[1].group_first == 0 && pack.portraits[2].group_count == 3 &&
            pack.portraits[4].group_first == 4 && pack.portraits[4].group_count == 1 &&
            pack.portraits[0].mirror && !pack.portraits[4].mirror);

        /* Loading ahead: from the last frame of group 7 the next frames are its first two, and a
         * frame alone in its group or in none has nothing ahead. (Codex review, 2026-09-23.) */
        int ahead[8], n = ok ? lomhd_group_ahead(&pack, 2, 8, ahead) : -1;
        check("ahead of a group's last frame come its first frames", n == 2 && ahead[0] == 0 && ahead[1] == 1);
        n = ok ? lomhd_group_ahead(&pack, 1, 1, ahead) : -1;
        check("... no more than asked for", n == 1 && ahead[0] == 2);
        check("... none for a one-frame group or a picture",
            ok && lomhd_group_ahead(&pack, 4, 8, ahead) == 0 && lomhd_group_ahead(&pack, 3, 8, ahead) == 0);

        /* Palettes: the three frames of group 7 share one; the picture and group 9 have their own
         * (the picture's is the same colours, but a picture keys nothing). */
        check("consecutive frames with one palette share it", ok && pack.palette_count == 3 &&
            pack.portraits[0].pal == pack.portraits[2].pal && pack.portraits[3].pal != pack.portraits[2].pal);
        lomhd_pack_free(&pack);

        tp_begin();
        tp_add_frame("g7a", 24, 20, s, 0, 7, FALSE);
        tp_add("a", 70, 67, idx_a);
        tp_add_frame("g7b", 24, 20, s, 0, 7, FALSE);
        tp_finish();
        check("a group split by another record is refused", !opens(tp_len));

        tp_begin();
        tp_add_sprite("k0", 24, 20, s, 0);
        tp_add_sprite("k5", 24, 20, s, 5);
        tp_finish();
        ok = tp_open(tp_len, &pack, &mem);
        check("the same colours with another key are another palette", ok && pack.palette_count == 2 &&
            pack.portraits[1].pal->skip[5] && !pack.portraits[0].pal->skip[5]);
        lomhd_pack_free(&pack);

        /* A sprite's probes hold LOMHD_SPRITE_MIN_COLOURS colours or are not made. Every row of this
         * one is a single colour: it opens (it is a valid sprite) with no probe at all. */
        static BYTE flat[24 * 20];
        for (int j = 0; j < 20; j++) for (int i = 0; i < 24; i++) flat[j * 24 + i] = (BYTE)(j * 3 + 10);
        tp_begin(); tp_add_sprite("flat", 24, 20, flat, 0); tp_finish();
        ok = tp_open(tp_len, &pack, &mem);
        check("a sprite of one-colour rows opens with no probes", ok && pack.probes == 0);
        lomhd_pack_free(&pack);
    }

    /* Probe capacity: LOMHD_MAX_PROBES. A 48x8 mirrored sprite with busy rows takes 24 (4 rows x 3
     * bands x both ways round; one colour rule). Every image's pixels differ, so probes do not
     * pile onto one hash chain, and the upscales are 1x1 to keep the pack small. A picture takes 6
     * (3 rows x 2 rules). */
    {
        #define CAP_SPRITES (LOMHD_MAX_PROBES / 24)
        static BYTE many[CAP_SPRITES + 1][48 * 8];
        unsigned s = 12345;
        for (int k = 0; k <= CAP_SPRITES; k++)
            for (int i = 0; i < 48 * 8; i++) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; many[k][i] = (BYTE)(s % 240 + 8); }

        char name[16];
        tp_begin();
        for (int k = 0; k < 5461; k++) { snprintf(name, sizeof name, "p%d", k); tp_add(name, 32, 4, many[k])->hw = 1; tp_records[k].hh = 1; }
        tp_finish();
        check("5,461 pictures open", opens(tp_len));

        for (int extra = 0; extra < 2; extra++)
        {
            tp_begin();
            for (int k = 0; k < CAP_SPRITES + extra; k++)
            {
                snprintf(name, sizeof name, "s%d", k);
                TP_RECORD* r = tp_add_frame(name, 48, 8, many[k], 0, 0, TRUE);
                r->hw = r->hh = 1;
            }
            tp_finish();
            LOMHD_PACK pack; TP_MEM mem;
            BOOL ok = tp_open(tp_len, &pack, &mem);
            if (!extra)
                check("sprites of 24 probes each open up to LOMHD_MAX_PROBES", ok && pack.probes == CAP_SPRITES * 24);
            else
                check("... and one more is refused", !ok);
            lomhd_pack_free(&pack);
        }
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

    {
        /* A stream past 2 GB: an animated pack reaches 1.8 GB, so offsets must stay unsigned all the
         * way to the read. Record a's upscale is claimed 2 GB longer than written; the reader serves
         * zeros for the gap, and b's indices, now past 0x80000000, must still load exactly.
         * (Codex review, 2026-09-23.) The Win32 seek in lomhd.c is not reachable from here. */
        two(); tp_finish();
        const DWORD entry = 1 + 1 + 8 + 4 + 768 + 8, first = 12 + 2 * entry, gap = 0x80000000u;
        DWORD idx_len, hd_len;
        memcpy(&idx_len, tp_buf + 12 + entry - 8, 4);
        memcpy(&hd_len, tp_buf + 12 + entry - 4, 4);
        DWORD grown = hd_len + gap;
        memcpy(tp_buf + 12 + entry - 4, &grown, 4);
        BIG_MEM big = { first + idx_len + hd_len, gap };
        LOMHD_PACK pack; DWORD bad;
        BOOL ok = lomhd_pack_open(big_read, &big, tp_len + gap, &pack, &bad);
        BYTE* idx = ok ? lomhd_load_indices(&pack, 1, big_read, &big) : NULL;
        check("a stream starting past 2 GB loads exactly",
            ok && pack.portraits[1].idx_off >= gap && idx && memcmp(idx, idx_b, sizeof idx_b) == 0);
        if (ok) lomhd_pack_free(&pack);
    }

    {
        /* Erosion: the eight neighbours of an unset pixel clear, and no further -- a pixel cleared
         * in the pass must not clear its own neighbours. Outside the image counts as set. */
        BYTE m[6 * 5], want[6 * 5];
        memset(m, 255, sizeof m);
        m[2 * 6 + 2] = 0;
        memcpy(want, m, sizeof m);
        for (int y = 1; y <= 3; y++) for (int x = 1; x <= 3; x++) want[y * 6 + x] = 0;
        lomhd_erode_mask(m, 6, 5);
        check("erosion clears the eight neighbours of an unset pixel, no more", memcmp(m, want, sizeof m) == 0);

        memset(m, 255, sizeof m);
        lomhd_erode_mask(m, 6, 5);
        int all = 1;
        for (int i = 0; i < 30; i++) all &= m[i] == 255;
        check("... and leaves a mask with nothing unset whole, border included", all);

        memset(m, 255, sizeof m);
        m[0] = 0;
        lomhd_erode_mask(m, 6, 5);
        check("... and at a corner clears only the neighbours inside",
            m[1] == 0 && m[6] == 0 && m[7] == 0 && m[2] == 255 && m[12] == 255 && m[14] == 255);
    }

    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
