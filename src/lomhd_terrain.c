#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "dd.h"
#include "ddsurface.h"
#include "IDirectDrawSurface.h"
#include "lomhd.h"
#include "lomhd_terrain.h"

/* The DirectDraw side of the HD terrain composite: see lomhd_terrain.h for the idea and
 * lords-of-magic-modding's terrain-hybrid-2x.toml for the exe half. Everything here keys off the
 * game's own surface pointers, re-read on every call, because the game recreates the map surface
 * when a save loads (seen in the 2026-09-24 trace) and may restore it after a lost surface. */

/* lomse.exe GS5R3 (sha256 a505f399...), from the static analysis of 2026-09-24. */
#define LOM_MAP_SURFACE  0x585174        /* IDirectDrawSurface*: the map render surface */
#define LOM_BACK_SURFACE 0x585170        /* the back buffer */
#define LOM_LOCK_HELPER_LO 0x4755e0      /* 4755e0 is the game's Lock helper ... */
#define LOM_LOCK_HELPER_HI 0x4756f0
#define LOM_START_SCREEN_LOCK 0x4545c4   /* ... called here to copy strt3_bk.lbm in at 1x */
#define LOM_RASTER_LOCK_A 0x5123c2       /* ... and here, by the terrain rasterizer */
#define LOM_RASTER_LOCK_B 0x518c79

/* Two edits of the hybrid set, read from the running exe: this code is right for that build and
 * wrong for every other one, stock included, so it stays off unless both are there. */
static const struct { DWORD va; BYTE bytes[3]; } g_marks[] = {
    { 0x512446, { 0xc1, 0xe5, 0x11 } },   /* shl ebp,0x11: vertex x at 2x */
    { 0x47532e, { 0x8d, 0x04, 0x95 } },   /* lea eax,[edx*4+...]: map surface height 4m+768 */
};

static volatile LONG g_state;            /* 0 unknown, 1 off, 2 on */
static CRITICAL_SECTION g_cs;
static LOMHD_TERRAIN g_t;
static BOOL g_t_ready;
static __thread int g_nesting;           /* this thread is inside a call made on our behalf */
static void* g_map_1x;                   /* the map surface while it holds a 1x picture */
static volatile LONG g_gen;              /* bumped whenever the record changes */
static volatile LONG g_copies, g_builds, g_starts;
static DWORD g_callers[8];
static volatile LONG g_caller_count;

/* Render thread only. */
static BYTE* g_mask;
static WORD* g_hd;
static int g_drawn;
static LONG g_built_gen = -1;
static LONG g_snap_gen;

static BOOL marks_present(void)
{
    for (size_t i = 0; i < sizeof(g_marks) / sizeof(g_marks[0]); i++)
    {
        BYTE got[3];
        SIZE_T n = 0;

        if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)g_marks[i].va, got, sizeof(got), &n) ||
            n != sizeof(got) || memcmp(got, g_marks[i].bytes, sizeof(got)) != 0)
            return FALSE;
    }

    return TRUE;
}

BOOL lomhd_terrain_active(void)
{
    LONG s = g_state;

    if (s == 0)
    {
        /* Decided once, on whichever thread gets here first; the section is made before the
         * state says "on", so no caller can see "on" without it. */
        static volatile LONG deciding;

        if (InterlockedCompareExchange(&deciding, 1, 0) == 0)
        {
            BOOL on = marks_present();

            if (on)
                InitializeCriticalSection(&g_cs);

            InterlockedExchange(&g_state, on ? 2 : 1);
        }
        else
        {
            while (g_state == 0)
                Sleep(0);
        }

        s = g_state;
    }

    return s == 2;
}

static IDirectDrawSurfaceImpl* game_surface(DWORD va)
{
    return *(IDirectDrawSurfaceImpl* volatile*)va;
}

static BOOL is_primary(IDirectDrawSurfaceImpl* s)
{
    return s && (s->caps & DDSCAPS_PRIMARYSURFACE);
}

/* Per thread: another thread's wrapped call must neither look nested nor end our nesting, and a
 * nested call can pump window messages (util_pull_messages) whose handler nests again. (Claude and
 * Codex review.) */
static BOOL nested(void)
{
    return g_nesting > 0;
}

static DWORD enter_nested(void)
{
    g_nesting++;
    return 0;
}

static void leave_nested(DWORD unused)
{
    (void)unused;
    g_nesting--;
}

/* g_cs held. The record needs the frame's size, which only the back buffer knows. */
static BOOL record_ready(IDirectDrawSurfaceImpl* back)
{
    if (g_t_ready)
        return g_t.w == (int)back->width && g_t.h == (int)back->height;

    g_t_ready = lt_init(&g_t, back->width, back->height);
    return g_t_ready;
}

/* Something other than the map covered part of a frame surface. g_cs taken here. */
static void forget(IDirectDrawSurfaceImpl* dst, IDirectDrawSurfaceImpl* back, const RECT* r)
{
    EnterCriticalSection(&g_cs);

    if (g_t_ready)
    {
        if (dst == back)
            lt_forget_back(&g_t, r);
        else if (is_primary(dst))
            lt_forget_primary(&g_t, r);

        InterlockedIncrement(&g_gen);
    }

    LeaveCriticalSection(&g_cs);
}

static BOOL still_1x(IDirectDrawSurfaceImpl* map)
{
    if (g_map_1x && g_map_1x != map)
        g_map_1x = NULL;                 /* a new map surface starts out 2x */

    return g_map_1x != NULL;
}

static void describe(LT_CALL* c, IDirectDrawSurfaceImpl* This, IDirectDrawSurfaceImpl* src,
    IDirectDrawSurfaceImpl* map, IDirectDrawSurfaceImpl* back, BOOL map_1x, BOOL src_key, BOOL dst_key)
{
    c->src_map = src && src == map;
    c->dst_map = This == map;
    c->src_back = src && src == back;
    c->dst_back = back && This == back;
    c->dst_primary = is_primary(This);
    c->map_1x = map_1x;
    c->src_key = src_key;
    c->dst_key = dst_key;
    c->both_16bit = src && src->bpp == 16 && This->bpp == 16;
}

/* The part of dst a BltFast of src_w x src_h at (x, y) covers, clipped as dds_BltFast clips. */
static BOOL fast_covers(const RECT* rect, int src_w, int src_h, DWORD x, DWORD y,
    IDirectDrawSurfaceImpl* dst, RECT* s, RECT* covered)
{
    POINT d;

    if (!lt_clip_fast(rect, src_w, src_h, (long)x, (long)y, dst->width, dst->height, s, &d))
        return FALSE;

    SetRect(covered, d.x, d.y, d.x + (s->right - s->left), d.y + (s->bottom - s->top));
    return TRUE;
}

BOOL lomhd_terrain_bltfast(IDirectDrawSurfaceImpl* This, DWORD dwX, DWORD dwY,
    IDirectDrawSurfaceImpl* src, LPRECT lpSrcRect, DWORD dwFlags, HRESULT* ret)
{
    if (!lomhd_terrain_active() || nested() || !src)
        return FALSE;

    lomhd_files_install();

    IDirectDrawSurfaceImpl* map = game_surface(LOM_MAP_SURFACE);
    IDirectDrawSurfaceImpl* back = game_surface(LOM_BACK_SURFACE);

    if (!map)
        return FALSE;

    BOOL src_key = (dwFlags & DDBLTFAST_SRCCOLORKEY) != 0;
    BOOL dst_key = (dwFlags & DDBLTFAST_DESTCOLORKEY) != 0;
    LT_CALL c;
    describe(&c, This, src, map, back, still_1x(map), src_key, dst_key);

    /* The source's extent as the game sees it: a 2x map is half its real size. */
    int src_w = src->width, src_h = src->height;

    if (c.src_map && !c.map_1x)
    {
        src_w /= 2;
        src_h /= 2;
    }

    RECT s, covered;
    DWORD was;

    switch (lt_route_bltfast(&c))
    {
    case LT_PASS:
        return FALSE;

    case LT_FORGET_PASS:
        if (fast_covers(lpSrcRect, src_w, src_h, dwX, dwY, This, &s, &covered))
            forget(This, back, &covered);

        return FALSE;

    case LT_STRETCH_OUT:
    {
        /* Never seen in the game: let cnc-ddraw stretch it, and remember nothing. */
        *ret = DD_OK;

        if (fast_covers(lpSrcRect, src_w, src_h, dwX, dwY, This, &s, &covered))
        {
            RECT s2;
            lt_double(&s, &s2);
            forget(This, back, &covered);
            was = enter_nested();
            *ret = dds_Blt(This, &covered, map, &s2, DDBLT_WAIT | (src_key ? DDBLT_KEYSRC : 0) |
                (dst_key ? DDBLT_KEYDEST : 0), NULL);
            leave_nested(was);
        }

        return TRUE;
    }

    case LT_DOWNSAMPLE:
    {
        WORD* dst_px = dds_GetBuffer(This);
        WORD* map_px = dds_GetBuffer(map);

        if (!dst_px || !map_px)
            return FALSE;

        /* A back buffer that no longer matches the record's size (a mode change) is copied to but
         * not recorded, and the rect it covers is forgotten -- the record describes a frame that
         * is gone, and lomhd_terrain_frame draws nothing while the sizes differ. (Codex review:
         * 887f0e5 left that stale record alone; forgetting it is the intended change.) */
        EnterCriticalSection(&g_cs);
        BOOL record = c.dst_back && record_ready(back);
        lt_map_to_1x(&g_t, map_px, map->pitch / 2, map->width, map->height, lpSrcRect, (long)dwX,
            (long)dwY, dst_px, This->pitch / 2, This->width, This->height, c.dst_back && g_t_ready, record);
        InterlockedIncrement(&g_gen);
        InterlockedIncrement(&g_copies);
        LeaveCriticalSection(&g_cs);

        *ret = DD_OK;
        return TRUE;
    }

    case LT_SELF:
    {
        /* The buffer scroll: the whole picture moves by the margin, in 2x pixels. */
        RECT s2;

        if (lpSrcRect)
            lt_double(lpSrcRect, &s2);

        was = enter_nested();
        *ret = dds_BltFast(map, dwX * 2, dwY * 2, map, lpSrcRect ? &s2 : NULL, dwFlags);
        leave_nested(was);
        return TRUE;
    }

    case LT_INTO_MAP:
    {
        /* A 1x picture copied in (the movie player saving the screen): doubled, so copying it
         * back out halves it to exactly what went in. Clipped as BltFast would before doubling:
         * dds_Blt given an out-of-range source rescales where BltFast crops. (Claude review.) */
        RECT d;
        *ret = DD_OK;

        if (lt_plan_into_map(lpSrcRect, src->width, src->height, (long)dwX, (long)dwY, map->width,
            map->height, &s, &d))
        {
            was = enter_nested();
            *ret = dds_Blt(map, &d, src, &s, DDBLT_WAIT | (src_key ? DDBLT_KEYSRC : 0) |
                (dst_key ? DDBLT_KEYDEST : 0), NULL);
            leave_nested(was);
        }

        return TRUE;
    }

    case LT_PRESENT:
    {
        /* Carry the record to the primary with the pixels, under one lock, so the render thread
         * never pairs new pixels with an old record or the reverse. */
        EnterCriticalSection(&g_cs);
        was = enter_nested();
        *ret = dds_BltFast(This, dwX, dwY, src, lpSrcRect, dwFlags);
        leave_nested(was);

        if (g_t_ready && fast_covers(lpSrcRect, src_w, src_h, dwX, dwY, This, &s, &covered))
        {
            /* A keyed copy skips pixels, and the record would claim them anyway. (Codex review.) */
            if (src_key || dst_key)
                lt_forget_primary(&g_t, &covered);
            else
            {
                POINT d = { covered.left, covered.top };
                lt_back_to_primary(&g_t, &s, d);
            }

            InterlockedIncrement(&g_gen);
        }

        LeaveCriticalSection(&g_cs);
        return TRUE;
    }

    default:
        return FALSE;
    }
}

BOOL lomhd_terrain_blt(IDirectDrawSurfaceImpl* This, LPRECT lpDestRect, IDirectDrawSurfaceImpl* src,
    LPRECT lpSrcRect, DWORD dwFlags, void* lpDDBltFx, HRESULT* ret)
{
    if (!lomhd_terrain_active() || nested())
        return FALSE;

    IDirectDrawSurfaceImpl* map = game_surface(LOM_MAP_SURFACE);
    IDirectDrawSurfaceImpl* back = game_surface(LOM_BACK_SURFACE);

    if (!map)
        return FALSE;

    LT_CALL c;
    describe(&c, This, src, map, back, still_1x(map), (dwFlags & DDBLT_KEYSRC) != 0,
        (dwFlags & DDBLT_KEYDEST) != 0);

    switch (lt_route_blt(&c))
    {
    case LT_DOUBLE:
    {
        RECT d2, s2;

        if (c.dst_map && lpDestRect)
            lt_double(lpDestRect, &d2);

        if (c.src_map && lpSrcRect)
            lt_double(lpSrcRect, &s2);

        if (!c.dst_map)
            forget(This, back, lpDestRect);

        DWORD was = enter_nested();
        *ret = dds_Blt(This, c.dst_map && lpDestRect ? &d2 : lpDestRect, src,
            c.src_map && lpSrcRect ? &s2 : lpSrcRect, dwFlags, (LPDDBLTFX)lpDDBltFx);
        leave_nested(was);
        return TRUE;
    }

    case LT_FORGET_PASS:
        forget(This, back, lpDestRect);
        return FALSE;

    default:
        return FALSE;
    }
}

void lomhd_terrain_lock(IDirectDrawSurfaceImpl* This, void* return_address, void* frame)
{
    if (!lomhd_terrain_active())
        return;

    IDirectDrawSurfaceImpl* map = game_surface(LOM_MAP_SURFACE);

    if (!map || This != map)
        return;

    /* At the Lock method's entry the game's stack reads: return into 4755e0, the five arguments,
     * 4755e0's saved esi and a pre-pushed constant, then 4755e0's own return address -- [esp+0x20].
     * frame is this method's frame pointer, so its entry esp is frame+4. */
    DWORD ra = (DWORD)return_address;
    DWORD caller = 0;

    if (ra >= LOM_LOCK_HELPER_LO && ra < LOM_LOCK_HELPER_HI && frame)
        caller = *(DWORD*)((BYTE*)frame + 4 + 0x20);

    lomhd_trace_op('K', This, NULL, NULL, (long)ra, (long)caller, 0);

    /* Game thread only, so no lock: the debug log may read a count one behind. */
    LONG n = g_caller_count;
    BOOL known = FALSE;

    for (LONG i = 0; i < n; i++)
        known |= g_callers[i] == caller;

    if (!known && n < 8)
    {
        g_callers[n] = caller;
        InterlockedExchange(&g_caller_count, n + 1);
    }

    if (caller == LOM_START_SCREEN_LOCK)
    {
        g_map_1x = map;
        InterlockedIncrement(&g_starts);
    }
    else if (caller == LOM_RASTER_LOCK_A || caller == LOM_RASTER_LOCK_B)
        g_map_1x = NULL;
}

void lomhd_terrain_flip(void)
{
    if (!lomhd_terrain_active())
        return;

    /* Never seen in the game, which presents by BltFast: the buffers swap, so neither record
     * describes its surface any more. */
    EnterCriticalSection(&g_cs);

    if (g_t_ready)
    {
        lt_forget_back(&g_t, NULL);
        lt_forget_primary(&g_t, NULL);
    }

    InterlockedIncrement(&g_gen);
    LeaveCriticalSection(&g_cs);
}

/* Render thread, g_ddraw.cs held. Takes g_cs and keeps it until lomhd_terrain_frame_done, after
 * the renderer has uploaded the frame: the game's presents take only g_cs, so without that the
 * frame drawn could be newer than the one the mask was built from, and a unit that moved in
 * between would be painted over by terrain for a frame. (Claude review.) */
static BOOL g_frame_held;

void lomhd_terrain_frame(void)
{
    if (!lomhd_terrain_active())
        return;

    EnterCriticalSection(&g_cs);
    g_frame_held = TRUE;

    IDirectDrawSurfaceImpl* primary = g_ddraw.primary;
    LONG gen = g_gen;

    if (!primary || primary->bpp != 16 || !g_t_ready || g_t.w != (int)primary->width ||
        g_t.h != (int)primary->height)
    {
        g_drawn = 0;                       /* nothing this frame, rather than the last mask */
        return;
    }

    if (gen == g_built_gen && !g_ddraw.render.surface_updated)
        return;

    WORD* frame = dds_GetBuffer(primary);

    if (!g_mask)
    {
        g_mask = malloc((size_t)g_t.w * g_t.h);
        g_hd = malloc((size_t)g_t.w * g_t.h * 4 * sizeof(WORD));
    }

    if (!frame || !g_mask || !g_hd)
    {
        g_drawn = 0;
        return;
    }

    g_drawn = lt_build(&g_t, frame, primary->pitch / 2, g_mask, g_hd, 200);
    g_built_gen = gen;
    g_snap_gen++;
    InterlockedIncrement(&g_builds);
}

void lomhd_terrain_frame_done(void)
{
    if (g_frame_held)
    {
        g_frame_held = FALSE;
        LeaveCriticalSection(&g_cs);
    }
}

/* Render thread. What lomhd_draw draws: NULL when nothing. The buffers stay valid until the next
 * lomhd_terrain_frame, which runs on this same thread. */
BOOL lomhd_terrain_snapshot(const BYTE** mask, const WORD** hd, int* w, int* h, LONG* gen)
{
    if (g_state != 2 || !g_drawn || !g_mask)
        return FALSE;

    *mask = g_mask;
    *hd = g_hd;
    *w = g_t.w;
    *h = g_t.h;
    *gen = g_snap_gen;
    return TRUE;
}

void lomhd_terrain_stats(long out[4])
{
    out[0] = InterlockedExchange(&g_copies, 0);
    out[1] = InterlockedExchange(&g_builds, 0);
    out[2] = g_drawn;
    out[3] = InterlockedExchange(&g_starts, 0);
}

int lomhd_terrain_lock_callers(DWORD* out, int max)
{
    int n = g_caller_count < max ? g_caller_count : max;
    memcpy(out, g_callers, n * sizeof(DWORD));
    return n;
}
