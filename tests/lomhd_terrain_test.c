/* The pure half of the HD terrain composite (src/lomhd_terrain_core.c) on synthetic surfaces.
 *
 *     cc -Itests/native -Iinc -std=c99 -Wall -o /tmp/lt tests/lomhd_terrain_test.c \
 *         src/lomhd_terrain_core.c && /tmp/lt
 *
 * tests/native/windows.h stands in for the real one, so this needs no Windows and no Wine.
 * The map is built so each 2x2 block's four pixels differ and its top-left is a known 1x value,
 * which makes a wrong block, a wrong phase or a wrong half show up as a wrong number. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lomhd_terrain.h"

#define W 640
#define H 480
#define MW 1520                 /* the hybrid map surface at margin 60: 4m+1280 x 4m+768 */
#define MH 1008

static int g_fail;

#define CHECK(cond, ...) do { if (!(cond)) { g_fail++; printf("FAIL %s:%d ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

/* The 1x value at a 1x map position; never 0, so a cleared pixel is visibly wrong. */
static WORD one_x(int x, int y)
{
    return (WORD)(((x * 7 + y * 13) & 0x3fff) | 0x4000);
}

static WORD two_x(int X, int Y)
{
    WORD base = one_x(X / 2, Y / 2);
    return (X & 1) || (Y & 1) ? (WORD)(base ^ (0x8000 | (X & 1) | ((Y & 1) << 1))) : base;
}

static WORD* make_map(void)
{
    WORD* m = malloc(sizeof(WORD) * MW * MH);

    for (int y = 0; y < MH; y++)
        for (int x = 0; x < MW; x++)
            m[y * MW + x] = two_x(x, y);

    return m;
}

static void test_clip(void)
{
    RECT s;
    POINT d;
    RECT in = { 10, 10, 110, 60 };

    CHECK(lt_clip_fast(&in, 760, 504, -5, -3, 640, 480, &s, &d), "negative point");
    CHECK(s.left == 15 && s.top == 13 && s.right == 110 && s.bottom == 60 && d.x == 0 && d.y == 0,
        "negative point moves the source: %d,%d,%d,%d at %d,%d", s.left, s.top, s.right, s.bottom, d.x, d.y);

    CHECK(lt_clip_fast(&in, 760, 504, 600, 470, 640, 480, &s, &d), "past the edge");
    CHECK(s.right - s.left == 40 && s.bottom - s.top == 10, "clipped to the destination: %dx%d",
        s.right - s.left, s.bottom - s.top);

    RECT past = { 700, 0, 900, 10 };
    CHECK(lt_clip_fast(&past, 760, 504, 0, 0, 640, 480, &s, &d) && s.right == 760, "clipped to the source");

    CHECK(!lt_clip_fast(&in, 760, 504, 640, 0, 640, 480, &s, &d), "nothing left");
    CHECK(lt_clip_fast(NULL, 760, 504, 0, 0, 640, 480, &s, &d) && s.right == 640 && s.bottom == 480,
        "NULL = whole source, clipped");
}

static void test_map_to_back(LOMHD_TERRAIN* t, const WORD* map, WORD* back)
{
    /* A dirty rect from the 2026-09-24 trace, in stock coordinates. */
    RECT r = { 1076 / 2, 444 / 2, 1166 / 2, 489 / 2 };
    int n = lt_map_to_1x(t, map, MW, MW, MH, &r, 508, 192, back, W, W, H, TRUE, TRUE);
    CHECK(n == 45 * 22, "pixels written %d", n);

    int bad = 0, bad_hd = 0;

    for (int y = 0; y < 22; y++)
        for (int x = 0; x < 45; x++)
        {
            int bx = 508 + x, by = 192 + y, mx = r.left + x, my = r.top + y;
            bad += back[by * W + bx] != one_x(mx, my);
            bad += t->exp_b[by * W + bx] != one_x(mx, my) || !t->valid_b[by * W + bx];

            for (int k = 0; k < 4; k++)
                bad_hd += t->hd_b[(2 * by + k / 2) * 2 * W + 2 * bx + k % 2] !=
                    two_x(2 * mx + k % 2, 2 * my + k / 2);
        }

    CHECK(!bad, "downsample or record wrong at %d pixels", bad);
    CHECK(!bad_hd, "2x block wrong at %d pixels", bad_hd);
    CHECK(back[191 * W + 508] == 0 && back[192 * W + 507] == 0 && !t->valid_b[192 * W + 553],
        "wrote outside the rect");
}

static void test_not_recorded(LOMHD_TERRAIN* t, const WORD* map, WORD* back)
{
    /* The same rect, but a caller that must not be remembered (the record could not be set up):
     * the pixels still arrive, the old record there is dropped. */
    RECT r = { 0, 0, 10, 10 };
    lt_map_to_1x(t, map, MW, MW, MH, &r, 508, 192, back, W, W, H, TRUE, FALSE);
    CHECK(!t->valid_b[192 * W + 508] && t->valid_b[192 * W + 520], "unrecorded copy forgets only its rect");
    CHECK(back[192 * W + 508] == one_x(0, 0), "unrecorded copy still writes");
}

static void test_present_and_build(LOMHD_TERRAIN* t, const WORD* map, WORD* back)
{
    RECT whole = { 60, 60, 700, 444 };        /* the stock full viewport blit */
    lt_map_to_1x(t, map, MW, MW, MH, &whole, 0, 0, back, W, W, H, TRUE, TRUE);

    WORD* prim = calloc(W * H, sizeof(WORD));
    BYTE* mask = malloc(W * H);
    WORD* hd = malloc(sizeof(WORD) * W * H * 4);

    /* Nothing presented yet: nothing to draw. */
    CHECK(lt_build(t, prim, W, mask, hd, 200) == 0, "drew before any present");

    /* A present copied somewhere else: forgotten, not carried. */
    RECT part = { 0, 0, 100, 100 };
    POINT elsewhere = { 10, 0 };
    lt_back_to_primary(t, &part, elsewhere);
    CHECK(!t->valid_p[0 * W + 50], "an off-place present carried the record");

    /* The in-place present, as the game does it. */
    memcpy(prim, back, sizeof(WORD) * W * H);
    RECT view = { 0, 0, 640, 384 };
    POINT at = { 0, 0 };
    lt_back_to_primary(t, &view, at);

    int drawn = lt_build(t, prim, W, mask, hd, 200);
    CHECK(drawn == 640 * 384, "clean frame: drew %d, want %d", drawn, 640 * 384);
    CHECK(hd[(2 * 100 + 1) * 2 * W + 2 * 200 + 1] == two_x(2 * 260 + 1, 2 * 160 + 1), "hd snapshot");

    /* A unit drawn over the terrain: its pixels and a one-pixel ring go back to the game's. */
    for (int y = 100; y < 105; y++)
        for (int x = 200; x < 205; x++)
            prim[y * W + x] ^= 0x1234;

    drawn = lt_build(t, prim, W, mask, hd, 200);
    CHECK(drawn == 640 * 384 - 7 * 7 + 4, "unit: drew %d, want %d", drawn, 640 * 384 - 7 * 7 + 4);
    CHECK(mask[102 * W + 202] == 0 && mask[99 * W + 202] == 0 && mask[98 * W + 202] == 255,
        "unit mask: inside %d ring %d outside %d", mask[102 * W + 202], mask[99 * W + 202], mask[98 * W + 202]);

    /* A full-screen menu over it: a few pixels happen to equal the terrain once there. None of them
     * may show terrain, and the record goes. */
    for (int i = 0; i < W * H; i++)
        prim[i] = (WORD)(t->exp_p[i] + 1 + (i % 3));

    for (int i = 0; i < W * H; i += 97)
        prim[i] = t->exp_p[i];

    drawn = lt_build(t, prim, W, mask, hd, 200);
    CHECK(drawn == 0, "menu: drew %d", drawn);
    CHECK(!t->valid_p[100 * W + 100], "menu: record kept");

    free(prim);
    free(mask);
    free(hd);
}

static void test_isolated_coincidence(void)
{
    LOMHD_TERRAIN t;
    CHECK(lt_init(&t, 8, 8), "init");
    memset(t.valid_p, 1, 64);

    WORD frame[64];

    for (int i = 0; i < 64; i++)
        frame[i] = (WORD)(t.exp_p[i] + (i == 30 ? 0 : 1));

    for (int i = 0; i < 64; i++)            /* most match: past the threshold, one pixel differs by chance */
        if (i % 8 < 4)
            frame[i] = t.exp_p[i];

    BYTE mask[64];
    lt_build(&t, frame, 8, mask, NULL, 200);
    CHECK(mask[30] == 0, "an isolated match was drawn");
    CHECK(mask[8 * 3 + 1] == 255 && mask[8 * 3 + 3] == 0, "erosion: inner %d edge %d", mask[25], mask[27]);
    lt_free(&t);
}


/* dds_BltFast's own clipping, copied from src/ddsurface.c (the stock lines, without the surfaces),
 * so lt_clip_fast is held to what cnc-ddraw actually copies rather than to hand-picked numbers.
 * (Claude review.) */
static BOOL stock_clip(const RECT* in, int src_w, int src_h, int dst_x, int dst_y, DWORD dst_w_max,
    DWORD dst_h_max, int* sx, int* sy, RECT* out)
{
    RECT src_rect = { 0, 0, src_w, src_h };

    if (in)
        src_rect = *in;

    if (dst_x < 0) { src_rect.left += abs(dst_x); dst_x = 0; }
    if (dst_y < 0) { src_rect.top += abs(dst_y); dst_y = 0; }
    if (src_rect.right < 0) src_rect.right = 0;
    if (src_rect.left < 0) src_rect.left = 0;
    if (src_rect.bottom < 0) src_rect.bottom = 0;
    if (src_rect.top < 0) src_rect.top = 0;
    if (src_rect.right > src_w) src_rect.right = src_w;
    if (src_rect.left > src_rect.right) src_rect.left = src_rect.right;
    if (src_rect.bottom > src_h) src_rect.bottom = src_h;
    if (src_rect.top > src_rect.bottom) src_rect.top = src_rect.bottom;

    *sx = src_rect.left;
    *sy = src_rect.top;
    RECT d = { dst_x, dst_y, (src_rect.right - src_rect.left) + dst_x, (src_rect.bottom - src_rect.top) + dst_y };
    if (d.right < 0) d.right = 0;
    if (d.left < 0) d.left = 0;
    if (d.bottom < 0) d.bottom = 0;
    if (d.top < 0) d.top = 0;
    if (d.right > (LONG)dst_w_max) d.right = dst_w_max;
    if (d.left > d.right) d.left = d.right;
    if (d.bottom > (LONG)dst_h_max) d.bottom = dst_h_max;
    if (d.top > d.bottom) d.top = d.bottom;
    *out = d;
    return d.right > d.left && d.bottom > d.top;
}

static int rnd(unsigned* seed, int n)
{
    *seed = *seed * 1103515245u + 12345u;
    return (int)((*seed >> 8) % (unsigned)n);
}

static void test_clip_matches_stock(void)
{
    unsigned seed = 12345;
    int bad = 0;

    for (int i = 0; i < 200000; i++)
    {
        int sw = 1 + rnd(&seed, 800), sh = 1 + rnd(&seed, 600);
        int dw = 1 + rnd(&seed, 800), dh = 1 + rnd(&seed, 600);
        RECT r;
        r.left = rnd(&seed, 900) - 100;
        r.top = rnd(&seed, 700) - 100;
        r.right = rnd(&seed, 900) - 100;
        r.bottom = rnd(&seed, 700) - 100;
        int x = rnd(&seed, 900) - 150;
        int y = rnd(&seed, 700) - 150;
        BOOL whole = rnd(&seed, 8) == 0;
        int sx, sy;
        RECT want;
        BOOL stock = stock_clip(whole ? NULL : &r, sw, sh, x, y, dw, dh, &sx, &sy, &want);

        RECT s;
        POINT d;
        BOOL ours = lt_clip_fast(whole ? NULL : &r, sw, sh, x, y, dw, dh, &s, &d);

        if (stock != ours || (ours && (s.left != sx || s.top != sy || d.x != want.left || d.y != want.top ||
            s.right - s.left != want.right - want.left || s.bottom - s.top != want.bottom - want.top)))
        {
            if (bad++ < 3)
                printf("  clip differs: rect %d,%d,%d,%d src %dx%d at %d,%d dst %dx%d (stock %d, ours %d)\n",
                    r.left, r.top, r.right, r.bottom, sw, sh, x, y, dw, dh, stock, ours);
        }
    }

    CHECK(!bad, "lt_clip_fast differs from dds_BltFast in %d of 200000 cases", bad);
}

static LT_CALL call(void)
{
    LT_CALL c;
    memset(&c, 0, sizeof(c));
    c.both_16bit = TRUE;
    return c;
}

static void test_routes(void)
{
    LT_CALL c = call();

    /* The calls the game makes (2026-09-24 trace and static analysis). */
    c.src_map = TRUE; c.dst_back = TRUE;
    CHECK(lt_route_bltfast(&c) == LT_DOWNSAMPLE, "map -> back");
    c.map_1x = TRUE;
    CHECK(lt_route_bltfast(&c) == LT_FORGET_PASS, "start screen map -> back");

    c = call(); c.src_map = TRUE; c.dst_map = TRUE;
    CHECK(lt_route_bltfast(&c) == LT_SELF, "buffer scroll");
    c.map_1x = TRUE;
    CHECK(lt_route_bltfast(&c) == LT_PASS, "start screen self copy");

    c = call(); c.dst_map = TRUE;
    CHECK(lt_route_bltfast(&c) == LT_INTO_MAP, "movie save into the map");

    c = call(); c.src_back = TRUE; c.dst_primary = TRUE;
    CHECK(lt_route_bltfast(&c) == LT_PRESENT, "present");

    /* Unusual ones: never recorded. */
    c = call(); c.src_map = TRUE; c.dst_back = TRUE; c.src_key = TRUE;
    CHECK(lt_route_bltfast(&c) == LT_STRETCH_OUT, "keyed map copy");
    c = call(); c.src_map = TRUE; c.dst_primary = TRUE;
    CHECK(lt_route_bltfast(&c) == LT_STRETCH_OUT, "map straight to the primary");
    c = call(); c.src_map = TRUE; c.both_16bit = FALSE;
    CHECK(lt_route_bltfast(&c) == LT_STRETCH_OUT, "map to an 8-bit surface");

    /* Something else onto the frame: forgotten unless it is a keyed sprite on the back buffer. */
    c = call(); c.dst_back = TRUE;
    CHECK(lt_route_bltfast(&c) == LT_FORGET_PASS, "opaque copy onto the back buffer");
    c.src_key = TRUE;
    CHECK(lt_route_bltfast(&c) == LT_PASS, "keyed sprite onto the back buffer");
    c = call(); c.dst_primary = TRUE;
    CHECK(lt_route_bltfast(&c) == LT_FORGET_PASS, "other source onto the primary");
    c = call();
    CHECK(lt_route_bltfast(&c) == LT_PASS, "surfaces the terrain does not care about");

    /* Blt: the map on either side doubles, except while it holds the start screen. */
    c = call(); c.dst_map = TRUE;
    CHECK(lt_route_blt(&c) == LT_DOUBLE, "colour fill on the map");
    c.map_1x = TRUE;
    CHECK(lt_route_blt(&c) == LT_PASS, "colour fill on the start screen");
    c = call(); c.src_map = TRUE; c.dst_back = TRUE;
    CHECK(lt_route_blt(&c) == LT_DOUBLE, "map Blt to the back buffer");
    c.map_1x = TRUE;
    CHECK(lt_route_blt(&c) == LT_FORGET_PASS, "start screen Blt to the back buffer");
    c = call(); c.dst_primary = TRUE;
    CHECK(lt_route_blt(&c) == LT_FORGET_PASS, "Blt onto the primary");
}

static void test_plan_into_map(void)
{
    /* The buffer scroll's self copy and every map-side Blt go through this. */
    RECT one = { 60, 1, 700, 383 }, two;
    lt_double(&one, &two);
    CHECK(two.left == 120 && two.top == 2 && two.right == 1400 && two.bottom == 766, "doubled %d,%d,%d,%d",
        two.left, two.top, two.right, two.bottom);

    /* The movie save: BltFast(map, 0, 0, surface 0, (0,0,640,480)) -> Blt to (0,0,1280,960). */
    RECT in = { 0, 0, 640, 480 }, s, d;
    CHECK(lt_plan_into_map(&in, 640, 480, 0, 0, MW, MH, &s, &d), "movie save");
    CHECK(s.right == 640 && s.bottom == 480 && d.right == 1280 && d.bottom == 960, "movie save rects %d,%d -> %d,%d",
        s.right, s.bottom, d.right, d.bottom);

    /* A source rect past its surface is cropped, not rescaled. */
    RECT over = { 600, 0, 700, 10 };
    CHECK(lt_plan_into_map(&over, 640, 480, 10, 20, MW, MH, &s, &d), "cropped");
    CHECK(s.right == 640 && d.left == 20 && d.top == 40 && d.right - d.left == 80, "cropped to %d, dst %d,%d w %d",
        s.right, d.left, d.top, d.right - d.left);

    /* Clipped by the map's 1x extent (760x504), then doubled. */
    CHECK(lt_plan_into_map(&in, 640, 480, 700, 0, MW, MH, &s, &d) && d.right == MW, "clipped to the map: %d", d.right);
}

int main(void)
{
    LOMHD_TERRAIN t;
    WORD* map = make_map();
    WORD* back = calloc(W * H, sizeof(WORD));

    CHECK(lt_init(&t, W, H), "init");
    test_clip();
    test_map_to_back(&t, map, back);
    test_not_recorded(&t, map, back);
    test_present_and_build(&t, map, back);
    test_isolated_coincidence();
    test_clip_matches_stock();
    test_routes();
    test_plan_into_map();

    lt_free(&t);
    free(map);
    free(back);
    printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail != 0;
}
