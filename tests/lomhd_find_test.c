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

/* A sprite: a busy picture inside an ellipse, key (0) outside it, and a shadow (1) patch low down. */
static void sprite(BYTE* out, int w, int h, unsigned seed)
{
    picture(out, w, h, seed);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
        {
            double dx = (i + 0.5 - w / 2.0) / (w / 2.0), dy = (j + 0.5 - h / 2.0) / (h / 2.0);
            if (dx * dx + dy * dy > 1) out[j * w + i] = 0;
            else if (j > h * 3 / 4 && i < w / 3) out[j * w + i] = 1;
        }
}

/* Every pixel a different value from any sprite's at the same place is not guaranteed, only
 * likely; `background` values are drawn from the same 4..123 range the sprites use. */
static void background(unsigned seed) { picture(&frame_idx[0][0], FW, FH, seed + 1000); }

static void draw_sprite(const BYTE* idx, int w, int h, int x, int y)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            if (idx[j * w + i] > 1) frame_idx[y + j][x + i] = idx[j * w + i];
}

static int kept_percent(const BYTE* idx, int w, int h, int x, int y)
{
    int opaque = 0, same = 0;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            if (idx[j * w + i] > 1) { opaque++; same += frame_idx[y + j][x + i] == idx[j * w + i]; }
    return 100 * same / opaque;
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

    /* Sprites (format 4): index 0 is the key here, 1 the shadow. Only opaque pixels are drawn over
     * a busy background, so everything around and under a sprite differs from it. */
    static BYTE t1[40 * 36], t2[40 * 36], narrow[20 * 24];
    sprite(t1, 40, 36, 11);
    sprite(t2, 40, 36, 12);
    sprite(narrow, 20, 24, 13);

    begin(1); tp_add_sprite("t1", 40, 36, t1, 0);
    background(21); draw_sprite(t1, 40, 36, 100, 100);
    expect("a sprite on a busy background is found once", (const char*[]){ "t1@100,100" }, 1);

    begin(1); tp_add_sprite("n", 20, 24, narrow, 0);
    background(22); draw_sprite(narrow, 20, 24, 300, 50);
    expect("a sprite too narrow for a picture's probe is found", (const char*[]){ "n@300,50" }, 1);

    /* Trees in front of trees: the second copy covers part of the first, and both are there. */
    begin(1); tp_add_sprite("t1", 40, 36, t1, 0);
    background(23); draw_sprite(t1, 40, 36, 100, 100); draw_sprite(t1, 40, 36, 124, 106);
    printf("    (the first copy keeps %d%% of its opaque pixels)\n", kept_percent(t1, 40, 36, 100, 100));
    expect("two overlapping copies of a sprite are both found",
        (const char*[]){ "t1@100,100", "t1@124,106" }, 2);

    /* ... even when their boxes share half: a mostly transparent sprite (a post, opaque in its left
     * quarter) stands over the right half of a wide one. Under the picture rule these are rivals
     * of similar size and only the better (the post) would be kept. */
    static BYTE wide[96 * 24], post[96 * 24];
    picture(wide, 96, 24, 14);
    picture(post, 96, 24, 15);
    for (int j = 0; j < 24; j++) for (int i = 24; i < 96; i++) post[j * 96 + i] = 0;
    begin(2); tp_add_sprite("wide", 96, 24, wide, 0); tp_add_sprite("post", 96, 24, post, 0);
    background(29); draw_sprite(wide, 96, 24, 100, 300); draw_sprite(post, 96, 24, 148, 300);
    printf("    (the wide one keeps %d%%)\n", kept_percent(wide, 96, 24, 100, 300));
    expect("sprites whose boxes share half are both found",
        (const char*[]){ "wide@100,300", "post@148,300" }, 2);

    /* Its middle covered, a sprite is found by a probe in a side band: with one band per row, all
     * probes sit in the middle. Columns 35-60 of 96 covered on every row. */
    begin(1); tp_add_sprite("wide", 96, 24, wide, 0);
    background(30); draw_sprite(wide, 96, 24, 200, 100);
    for (int j = 0; j < 24; j++) for (int i = 35; i <= 60; i++) frame_idx[100 + j][200 + i] ^= 128;
    printf("    (keeps %d%%)\n", kept_percent(wide, 96, 24, 200, 100));
    expect("a sprite with its middle covered is found by a side probe", (const char*[]){ "wide@200,100" }, 1);

    /* Covered only on the rows a 1-in-16 sample reads: 83% of it still matches, so it is found. A
     * sampled pre-check at 50% rejected it (Codex review, 2026-09-23). */
    {
        static BYTE solid[40 * 36];
        picture(solid, 40, 36, 16);
        begin(1); tp_add_sprite("solid", 40, 36, solid, 0);
        background(32); draw_sprite(solid, 40, 36, 400, 100);
        for (int j = 4; j <= 24; j += 4) for (int i = 0; i < 40; i++) frame_idx[100 + j][400 + i] ^= 128;
        printf("    (keeps %d%%)\n", kept_percent(solid, 40, 36, 400, 100));
        expect("a sprite covered only on its sampled rows is found", (const char*[]){ "solid@400,100" }, 1);
    }

    /* Only opaque pixels count toward the bar. The post is three quarters transparent; with 40% of
     * its opaque pixels covered it must not pass, though 90% of its box still "matches". */
    begin(1); tp_add_sprite("post", 96, 24, post, 0);
    background(31); draw_sprite(post, 96, 24, 300, 200);
    for (int j = 0; j < 10; j++) for (int i = 0; i < 24; i++) frame_idx[200 + j][300 + i] ^= 128;
    printf("    (keeps %d%%)\n", kept_percent(post, 96, 24, 300, 200));
    expect("a mostly transparent sprite with 40% of it covered is not found", (const char*[]){ 0 }, 0);

    /* Covered by something that is not a sprite: 25% of its rows still passes 70%, 40% does not. */
    begin(1); tp_add_sprite("t1", 40, 36, t1, 0);
    background(24); draw_sprite(t1, 40, 36, 200, 200); damage_rows(200, 200, 40, 0, 9);
    printf("    (keeps %d%%)\n", kept_percent(t1, 40, 36, 200, 200));
    expect("a sprite with its top quarter covered is still found", (const char*[]){ "t1@200,200" }, 1);
    begin(1); tp_add_sprite("t1", 40, 36, t1, 0);
    background(24); draw_sprite(t1, 40, 36, 200, 200); damage_rows(200, 200, 40, 0, 16);
    printf("    (keeps %d%%)\n", kept_percent(t1, 40, 36, 200, 200));
    expect("... with its top 44% covered it is not", (const char*[]){ 0 }, 0);

    /* Look-alikes at one spot -- three library levels matched at one place in a capture -- are
     * rivals, and the one drawn wins in either pack order. */
    static BYTE t1b[40 * 36];
    memcpy(t1b, t1, sizeof t1);
    for (int i = 30 * 40; i < 36 * 40; i++) if (t1b[i] > 1) t1b[i] = (BYTE)(t1b[i] + 128);
    begin(2); tp_add_sprite("t1", 40, 36, t1, 0); tp_add_sprite("t1b", 40, 36, t1b, 0);
    background(25); draw_sprite(t1, 40, 36, 60, 60);
    expect("of two look-alike sprites at one spot the drawn one wins", (const char*[]){ "t1@60,60" }, 1);
    begin(2); tp_add_sprite("t1b", 40, 36, t1b, 0); tp_add_sprite("t1", 40, 36, t1, 0);
    background(25); draw_sprite(t1b, 40, 36, 60, 60);
    expect("... in either pack order", (const char*[]){ "t1b@60,60" }, 1);

    /* A forest: as many copies as there are placements. */
    {
        static char names[LOMHD_MAX_PLACEMENTS][16];
        const char* want[LOMHD_MAX_PLACEMENTS];
        begin(1); tp_add_sprite("t2", 40, 36, t2, 0);
        background(26);
        for (int k = 0; k < 64; k++)
        {
            int x = 4 + (k % 8) * 78, y = 4 + (k / 8) * 58;
            draw_sprite(t2, 40, 36, x, y);
            snprintf(names[k], sizeof names[k], "t2@%d,%d", x, y);
            want[k] = names[k];
        }
        expect("64 sprites on one screen are all found", want, 64);
    }

    /* A picture and sprites in one pack: each is found by its own probe width. */
    begin(2); record("a", 70, 67, a); tp_add_sprite("t1", 40, 36, t1, 0);
    background(27); draw(a, 70, 67, 400, 300); draw_sprite(t1, 40, 36, 100, 100);
    expect("a picture and a sprite in one pack are both found", (const char*[]){ "a@400,300", "t1@100,100" }, 2);

    /* A sprite at a picture's top-left is on the picture, not a rival to it: both are kept. */
    begin(2); record("screen", 640, 480, screen); tp_add_sprite("t1", 40, 36, t1, 0);
    clear(); draw(screen, 640, 480, 0, 0); draw_sprite(t1, 40, 36, 0, 0);
    expect("a sprite at a screen's top-left: both are kept", (const char*[]){ "screen@0,0", "t1@0,0" }, 2);

    /* A frame narrower than a picture's probe still finds a sprite. */
    {
        LOMHD_PACK pack; PLACEMENT out[LOMHD_MAX_PLACEMENTS];
        static WORD narrow_frame[24 * 30];
        for (int j = 0; j < 30; j++)
            for (int i = 0; i < 24; i++)
            {
                int v = j < 24 && i < 20 && narrow[j * 20 + i] > 1 ? narrow[j * 20 + i] : (i * 31 + j * 17) % 120 + 4;
                narrow_frame[j * 24 + i] = (WORD)(((v >> 3) << 11) | ((v >> 2) << 5) | (v >> 3));
            }
        tp_begin(); tp_add_sprite("n", 20, 24, narrow, 0);
        int n = tp_open(tp_finish(), &pack, &mem) ? lomhd_find(&pack, narrow_frame, 24, 30, 24, out, LOMHD_MAX_PLACEMENTS) : -1;
        BOOL ok = n == 1 && out[0].x == 0 && out[0].y == 0;
        printf("%-66s %s\n", "a frame narrower than a picture's probe still finds a sprite", ok ? "ok" : "FAIL");
        if (!ok) failures++;
        if (n >= 0) lomhd_pack_free(&pack);
    }

    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
