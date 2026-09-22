/* The pack parser's refusal paths, asserted. The pack is untrusted input: every malformed shape
 * here must be refused, and a valid one accepted. Added after cross-model review, which built a
 * 140 KB pack that the first parser accepted with a 65535x65535 upscale inside it. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "lomhd_match.h"

/* LOMHD_CONTROL_RUN: build this test against the parser from BEFORE the review fixes, to prove the
 * cases can fail. That parser has no lomhd_pack_free and leaks, so the control skips the 40,000-parse
 * truncation loop -- which would otherwise exhaust a 32-bit heap and make every later case "pass"
 * by running out of memory, the exact trap this test fell into first. */
#ifdef LOMHD_CONTROL_RUN
void lomhd_pack_free(LOMHD_PACK* pack)
{
    if (pack->portraits) HeapFree(GetProcessHeap(), 0, pack->portraits);
    if (pack->table) HeapFree(GetProcessHeap(), 0, pack->table);
    memset(pack, 0, sizeof(*pack));
}
#endif

static BYTE buf[1 << 22];
static DWORD len;
static int failures;

static void put(const void* p, DWORD n) { memcpy(buf + len, p, n); len += n; }
static void put_u16(WORD v) { put(&v, 2); }
static void put_u32(DWORD v) { put(&v, 4); }

static void put_image(WORD w, WORD h, DWORD pixels_actually_written)
{
    BYTE pal[768];
    for (int i = 0; i < 768; i++) pal[i] = (BYTE)(i * 7);
    put_u16(w); put_u16(h); put(pal, 768);
    for (DWORD i = 0; i < pixels_actually_written; i++) { BYTE b = (BYTE)(i * 13 + 1); put(&b, 1); }
}

static void begin(DWORD count) { len = 0; put("LOMHDPK1", 8); put_u32(count); }
static void record(const char* name, WORD w, WORD h, WORD hw, WORD hh)
{
    BYTE n = (BYTE)strlen(name); put(&n, 1); put(name, n);
    put_image(w, h, (DWORD)w * h);
    put_image(hw, hh, (DWORD)hw * hh);
}

static void expect(const char* what, BOOL want)
{
    LOMHD_PACK pack; DWORD bad;
    BOOL got = lomhd_pack_parse(buf, len, &pack, &bad);
    lomhd_pack_free(&pack);
    printf("%-60s %s\n", what, got == want ? "ok" : "FAIL");
    if (got != want) failures++;
}

int main(void)
{
    begin(2); record("a.lbm", 70, 67, 140, 134); record("b.lbm", 70, 67, 140, 134);
    expect("a valid two-portrait pack is accepted", TRUE);

#ifndef LOMHD_CONTROL_RUN
    DWORD full = len; int truncations_accepted = 0;
    for (DWORD cut = 0; cut < full; cut++)
    {
        LOMHD_PACK pack; DWORD bad;
        if (lomhd_pack_parse(buf, cut, &pack, &bad)) truncations_accepted++;
        lomhd_pack_free(&pack);
    }
    printf("%-60s %s\n", "a pack truncated at ANY byte is refused", truncations_accepted ? "FAIL" : "ok");
    if (truncations_accepted) failures++;
#endif

    begin(2); record("a.lbm", 70, 67, 140, 134); record("b.lbm", 70, 67, 140, 134); buf[len++] = 0;
    expect("one trailing byte is refused", FALSE);

    begin(2); record("a.lbm", 70, 67, 140, 134); record("b.lbm", 71, 67, 140, 134);
    expect("an original of a different width is refused", FALSE);

    begin(1); record("a.lbm", 70, 3, 140, 6);
    expect("an original too short for three probe rows is refused", FALSE);

    begin(1); record("a.lbm", 70, 67, LOMHD_MAX_HD_SIDE + 1, 134);
    expect("an upscale wider than the draw can hold is refused", FALSE);

    begin(0);
    expect("a count of zero is refused", FALSE);

    begin(0xFFFFFFFFu); record("a.lbm", 70, 67, 140, 134);
    expect("a count that would overflow the table check is refused", FALSE);

    /* The review's wraparound, reproduced layout for layout (its generator: lomhd_rev/gen.py).
     * Record b declares a 65535x65535 upscale. The old 32-bit check `pos + 772 + w*h` wraps once
     * pos passes ~130 KB, pos jumps BACKWARDS into record a's upscale pixels, and a fake third
     * record planted there makes the whole pack parse as valid. A small pack cannot show this --
     * an earlier version of this test used one and passed against the broken parser too. */
    BOOL built = FALSE;
    DWORD hd1_pos_used = 0;
    for (DWORD H = 320; H < 400 && !built; H++)
    {
        DWORD orig_len = 4 + 768 + 70 * 67;
        DWORD rec0_head = 2 + orig_len;
        DWORD hd0_data_pos = 12 + rec0_head + 772;
        DWORD hd0_len = 400 * H;
        DWORD rec1 = 2 + orig_len;
        DWORD hd1_pos = hd0_data_pos + hd0_len + rec1;
        DWORD wrap_to = (DWORD)(hd1_pos + 772 + 65535ULL * 65535ULL);
        if (!(hd0_data_pos <= wrap_to && wrap_to < hd0_data_pos + hd0_len)) continue;
        DWORD fake_hd_pos = wrap_to + 1 + orig_len;
        DWORD size = hd1_pos + 772;
        DWORD remain = size - (fake_hd_pos + 772);
        WORD fw = 0, fh = 0;
        for (DWORD w = 2; w < 65536 && !fw; w++)
            if (remain % w == 0 && remain / w < 65536) { fw = (WORD)w; fh = (WORD)(remain / w); }
        if (!fw) continue;

        begin(3);
        BYTE one = 1; put(&one, 1); put("a", 1); put_image(70, 67, 70 * 67);
        put_u16(400); put_u16((WORD)H); { BYTE pal[768] = {0}; put(pal, 768); }
        DWORD hd0_at = len; memset(buf + len, 0, hd0_len); len += hd0_len;
        put(&one, 1); put("b", 1); put_image(70, 67, 70 * 67);
        put_u16(65535); put_u16(65535); { BYTE pal[768] = {0}; put(pal, 768); }

        /* the fake record, planted where the wrapped pos lands */
        DWORD save = len; len = hd0_at + (wrap_to - hd0_data_pos);
        BYTE zero = 0; put(&zero, 1); put_image(70, 67, 70 * 67); put_u16(fw); put_u16(fh);
        len = save;
        built = len == size;
        hd1_pos_used = hd1_pos;
    }
    printf("%-60s %s\n", "(the wraparound pack could be built)", built ? "ok" : "FAIL");
    if (!built) failures++;
    expect("the review's wraparound pack is refused", FALSE);

    /* ...and refused BY THE BOUNDS CHECK, at the start of the 65535x65535 image. The 512 cap would
     * refuse this pack too, so a bare FALSE passed with the 64-bit check reverted: the 32-bit check
     * let pos wrap, and the cap then refused it at the wrapped offset. (Cross-model review, second
     * pass, 2026-09-22.) */
    {
        LOMHD_PACK pack; DWORD bad = 0;
        BOOL got = lomhd_pack_parse(buf, len, &pack, &bad);
        lomhd_pack_free(&pack);
        BOOL right = !got && bad == hd1_pos_used;
        printf("%-60s %s\n", "...refused at the oversized image, before pos can wrap", right ? "ok" : "FAIL");
        if (!right) failures++;
    }

    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
