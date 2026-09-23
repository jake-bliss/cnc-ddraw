/* lomhd_find on synthetic frames: which candidate wins at a spot, and what a partly covered image
 * still matches. Added after cross-model review (2026-09-22) found that nothing asserted the
 * best-match rule: lomhd_match_test only prints what it finds in captured frames.
 *
 * Every image shares one grey palette, so index i draws as RGB565 rule 0 of (i, i, i), and
 * index i + 128 is always a different colour -- that is how a case "damages" pixels. */
#include <stdio.h>
#include "lomhd_test_pack.h"

#define FW 640
#define FH 480

static int failures;
static BYTE frame_idx[FH][FW];
static WORD frame[FH * FW];
static int frame_rule;              /* 0: truncating RGB565, 1: rounding */

/* A picture's indices from a seed: busy enough that every 32-pixel slice is distinct. */
static void picture(BYTE* out, int w, int h, unsigned seed)
{
    unsigned s = seed * 2654435761u + 1;
    for (int i = 0; i < w * h; i++) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; out[i] = (BYTE)(s % 120 + 4); }
}

static void begin(int n) { (void)n; tp_begin(); }
static void record(const char* name, int w, int h, const BYTE* idx) { tp_add(name, w, h, idx); }

static void clear(void) { memset(frame_idx, 0, sizeof frame_idx); }

static void draw(const BYTE* idx, int w, int h, int x, int y)
{
    for (int j = 0; j < h; j++) memcpy(&frame_idx[y + j][x], idx + j * w, w);
}

static void damage_rows(int x, int y, int w, int from, int to)
{
    for (int j = from; j < to; j++)
        for (int i = 0; i < w; i++) frame_idx[y + j][x + i] = (BYTE)(frame_idx[y + j][x + i] + 128);
}

static TP_MEM mem;

static int find(PLACEMENT* out, LOMHD_PACK* pack)
{
    for (int y = 0; y < FH; y++)
        for (int x = 0; x < FW; x++)
        {
            int v = frame_idx[y][x], r = v >> 3, g = v >> 2;
            if (frame_rule) { r = (v + 4) >> 3; g = (v + 2) >> 2; r = r > 31 ? 31 : r; g = g > 63 ? 63 : g; }
            frame[y * FW + x] = (WORD)((r << 11) | (g << 5) | r);
        }
    if (!tp_open(tp_finish(), pack, &mem)) { printf("pack refused\n"); return -1; }
    return lomhd_find(pack, frame, FW, FH, FW, out, LOMHD_MAX_PLACEMENTS);
}

/* The placements found, as "name@x,y" sorted-insensitive: each wanted entry must appear, and
 * nothing else. */
static void expect(const char* what, const char* const* want, int wanted)
{
    LOMHD_PACK pack; PLACEMENT out[LOMHD_MAX_PLACEMENTS];
    int n = find(out, &pack), ok = n == wanted;
    for (int k = 0; ok && k < wanted; k++)
    {
        BOOL hit = FALSE;
        for (int i = 0; i < n && !hit; i++)
        {
            char got[64];
            snprintf(got, sizeof got, "%s@%d,%d", pack.portraits[out[i].portrait].name, out[i].x, out[i].y);
            hit = strcmp(got, want[k]) == 0;
        }
        ok = hit;
    }
    printf("%-66s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
    {
        failures++;
        for (int i = 0; i < n; i++)
            printf("    got %s@%d,%d\n", pack.portraits[out[i].portrait].name, out[i].x, out[i].y);
    }
    if (n >= 0) lomhd_pack_free(&pack);
}

int main(void)
{
    static BYTE a[70 * 67], a2[70 * 67], b[70 * 67], c[90 * 67];
    picture(a, 70, 67, 1);
    picture(b, 70, 67, 2);

    /* A tooltip over the top 10 rows: 700 of 4690 pixels differ, inside the 85% bar. The first
     * pre-check held its 1-in-16 sample to 85% too and refused this. */
    begin(1); record("a", 70, 67, a);
    clear(); draw(a, 70, 67, 50, 40); damage_rows(50, 40, 70, 0, 10);
    expect("a picture with its top 10 rows covered is still found", (const char*[]){ "a@50,40" }, 1);

    /* One picture, two colour rules and three probe rows: still one placement. */
    clear(); draw(a, 70, 67, 50, 40);
    expect("an exact picture is one placement", (const char*[]){ "a@50,40" }, 1);

    /* a2 is a with its bottom 8 rows changed: each is an 88% match where the other is drawn. The
     * one actually drawn must win, whichever the scan reaches first. */
    memcpy(a2, a, sizeof a);
    for (int i = 59 * 70; i < 67 * 70; i++) a2[i] = (BYTE)(a2[i] + 128);
    begin(2); record("a", 70, 67, a); record("a2", 70, 67, a2);
    clear(); draw(a, 70, 67, 50, 40);
    expect("of two candidates at one spot the better wins (a drawn)", (const char*[]){ "a@50,40" }, 1);
    begin(2); record("a2", 70, 67, a2); record("a", 70, 67, a);
    clear(); draw(a2, 70, 67, 50, 40);
    expect("of two candidates at one spot the better wins (a2 drawn)", (const char*[]){ "a2@50,40" }, 1);

    /* Pictures that touch, or share one column, are different things on screen: both are drawn. */
    begin(2); record("a", 70, 67, a); record("b", 70, 67, b);
    clear(); draw(a, 70, 67, 10, 100); draw(b, 70, 67, 79, 100);
    expect("two pictures sharing one column are both kept", (const char*[]){ "a@10,100", "b@79,100" }, 2);

    /* c spans more than half of a and of b, and matches 91%. a is drawn damaged (88%), b exactly.
     * c beats a but not b, so it must lose: drawing it would paint over part of b's exact match.
     * c sits 5 rows lower so the scan reaches it after both: the first version compared a new
     * candidate with only the first placement it overlapped, and replaced a with c. */
    clear(); draw(a, 70, 67, 0, 200); draw(b, 70, 67, 80, 200); damage_rows(0, 200, 70, 59, 67);
    for (int j = 0; j < 67; j++) memcpy(c + j * 90, &frame_idx[205 + j][30], 90);
    for (int i = 61 * 90; i < 67 * 90; i++) c[i] = (BYTE)(c[i] + 128);
    begin(3); record("a", 70, 67, a); record("b", 70, 67, b); record("c", 90, 67, c);
    expect("a candidate must beat every placement it overlaps", (const char*[]){ "a@0,200", "b@80,200" }, 2);
    begin(3); record("c", 90, 67, c); record("b", 70, 67, b); record("a", 70, 67, a);
    expect("... in either pack order", (const char*[]){ "a@0,200", "b@80,200" }, 2);

    /* The frame drawn with the ROUNDING rule. Index v looks the same under both rules when v % 8
     * is 0; rows 0-5 of r use v % 8 == 4, so the truncating rule scores it 91% and the rounding
     * rule 100%. r2 is r with rows 60-62 changed: 95.5%, between the two. Regression cover only:
     * the first version skipped r's second rule as "already seen", but recovered at r's next probe
     * row, so this case passes against it too. */
    static BYTE r[70 * 67], r2[70 * 67];
    picture(r, 70, 67, 3);
    for (int i = 0; i < 70 * 67; i++) r[i] = (BYTE)((r[i] & ~7) | (i < 6 * 70 ? 4 : 0));
    memcpy(r2, r, sizeof r);
    for (int i = 60 * 70; i < 63 * 70; i++) r2[i] = (BYTE)(r2[i] ^ 128);
    frame_rule = 1;
    begin(2); record("r", 70, 67, r); record("r2", 70, 67, r2);
    clear(); draw(r, 70, 67, 300, 300);
    expect("a picture's better colour rule counts, not its first", (const char*[]){ "r@300,300" }, 1);
    frame_rule = 0;

    /* Full screens: scored on their 1-in-16 sample at LOMHD_LARGE_FRACTION (30%). Captured
     * 2026-09-23, the main interface bar showed 30-38% of its pixels under the live map. */
    static BYTE screen[640 * 480];
    picture(screen, 640, 480, 9);
    begin(1); record("screen", 640, 480, screen);
    clear(); draw(screen, 640, 480, 0, 0); damage_rows(0, 0, 640, 0, 288);
    expect("a screen with its top 60% covered is still found", (const char*[]){ "screen@0,0" }, 1);
    begin(1); record("screen", 640, 480, screen);
    clear(); draw(screen, 640, 480, 0, 0); damage_rows(0, 0, 640, 0, 360);
    expect("a screen with 75% covered is not", (const char*[]){ 0 }, 0);

    /* A portrait on a screen is not an alternative to the screen: both are drawn. The first
     * version's rivalry rule (half of the smaller one shared) would have kept only one. */
    begin(2); record("screen", 640, 480, screen); record("a", 70, 67, a);
    clear(); draw(screen, 640, 480, 0, 0); draw(a, 70, 67, 100, 100);
    expect("a portrait drawn on a screen: both are kept", (const char*[]){ "screen@0,0", "a@100,100" }, 2);

    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
