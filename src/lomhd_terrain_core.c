#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "lomhd_terrain.h"

/* The pure part of the HD terrain composite; see lomhd_terrain.h. */

BOOL lt_init(LOMHD_TERRAIN* t, int w, int h)
{
    memset(t, 0, sizeof(*t));
    t->w = w;
    t->h = h;

    size_t n = (size_t)w * h;
    t->exp_b = calloc(n, sizeof(WORD));
    t->hd_b = calloc(n * 4, sizeof(WORD));
    t->valid_b = calloc(n, 1);
    t->exp_p = calloc(n, sizeof(WORD));
    t->hd_p = calloc(n * 4, sizeof(WORD));
    t->valid_p = calloc(n, 1);

    if (!t->exp_b || !t->hd_b || !t->valid_b || !t->exp_p || !t->hd_p || !t->valid_p)
    {
        lt_free(t);
        return FALSE;
    }

    return TRUE;
}

void lt_free(LOMHD_TERRAIN* t)
{
    free(t->exp_b);
    free(t->hd_b);
    free(t->valid_b);
    free(t->exp_p);
    free(t->hd_p);
    free(t->valid_p);
    memset(t, 0, sizeof(*t));
}

static LONG clampl(LONG v, LONG lo, LONG hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

BOOL lt_clip_fast(const RECT* src_in, int src_w, int src_h, long dx, long dy, int dst_w, int dst_h,
    RECT* src_out, POINT* dst_out)
{
    RECT s = { 0, 0, src_w, src_h };

    if (src_in)
        s = *src_in;

    if (dx < 0)
    {
        s.left -= dx;
        dx = 0;
    }

    if (dy < 0)
    {
        s.top -= dy;
        dy = 0;
    }

    s.right = clampl(s.right, 0, src_w);
    s.bottom = clampl(s.bottom, 0, src_h);
    s.left = clampl(s.left, 0, s.right);
    s.top = clampl(s.top, 0, s.bottom);

    long w = s.right - s.left, h = s.bottom - s.top;

    if (dx + w > dst_w)
        w = dst_w - dx;

    if (dy + h > dst_h)
        h = dst_h - dy;

    if (w <= 0 || h <= 0)
        return FALSE;

    SetRect(src_out, s.left, s.top, s.left + w, s.top + h);
    dst_out->x = dx;
    dst_out->y = dy;
    return TRUE;
}

/* r clipped to the frame; FALSE when empty. */
static BOOL frame_rect(const LOMHD_TERRAIN* t, const RECT* r, RECT* out)
{
    if (!r)
    {
        SetRect(out, 0, 0, t->w, t->h);
        return TRUE;
    }

    SetRect(out, clampl(r->left, 0, t->w), clampl(r->top, 0, t->h), clampl(r->right, 0, t->w),
        clampl(r->bottom, 0, t->h));
    return out->right > out->left && out->bottom > out->top;
}

static void forget(const LOMHD_TERRAIN* t, BYTE* valid, const RECT* r)
{
    RECT c;

    if (!frame_rect(t, r, &c))
        return;

    for (LONG y = c.top; y < c.bottom; y++)
        memset(valid + (size_t)y * t->w + c.left, 0, c.right - c.left);
}

void lt_forget_back(LOMHD_TERRAIN* t, const RECT* r)
{
    forget(t, t->valid_b, r);
}

void lt_forget_primary(LOMHD_TERRAIN* t, const RECT* r)
{
    forget(t, t->valid_p, r);
}

int lt_map_to_1x(LOMHD_TERRAIN* t, const WORD* map, int map_pitch_px, int map_w, int map_h,
    const RECT* src_1x, long dx, long dy, WORD* dst, int dst_pitch_px, int dst_w, int dst_h,
    BOOL dst_is_back, BOOL record)
{
    RECT s;
    POINT d;

    /* Clipped in 1x units against the map's 1x extent, exactly as the stock game's call would be. */
    if (!lt_clip_fast(src_1x, map_w / 2, map_h / 2, dx, dy, dst_w, dst_h, &s, &d))
        return 0;

    int w = s.right - s.left, h = s.bottom - s.top;
    int hw = t->w * 2;
    BOOL keep = dst_is_back && record && t->exp_b && dst_w == t->w && dst_h == t->h;

    for (int y = 0; y < h; y++)
    {
        const WORD* row0 = map + (size_t)(2 * (s.top + y)) * map_pitch_px + 2 * s.left;
        const WORD* row1 = row0 + map_pitch_px;
        WORD* out = dst + (size_t)(d.y + y) * dst_pitch_px + d.x;

        for (int x = 0; x < w; x++)
            out[x] = row0[2 * x];

        if (!keep)
            continue;

        size_t o = (size_t)(d.y + y) * t->w + d.x;
        memcpy(t->exp_b + o, out, w * sizeof(WORD));
        memset(t->valid_b + o, 1, w);

        WORD* hd = t->hd_b + (size_t)(2 * (d.y + y)) * hw + 2 * d.x;
        memcpy(hd, row0, 2 * w * sizeof(WORD));
        memcpy(hd + hw, row1, 2 * w * sizeof(WORD));
    }

    if (dst_is_back && !keep)
    {
        RECT gone = { d.x, d.y, d.x + w, d.y + h };
        lt_forget_back(t, &gone);
    }

    return w * h;
}

void lt_back_to_primary(LOMHD_TERRAIN* t, const RECT* src, POINT dst)
{
    RECT to = { dst.x, dst.y, dst.x + (src->right - src->left), dst.y + (src->bottom - src->top) };
    RECT c;

    if (!frame_rect(t, &to, &c))
        return;

    if (dst.x != src->left || dst.y != src->top)
    {
        lt_forget_primary(t, &c);
        return;
    }

    int hw = t->w * 2;
    int w = c.right - c.left;

    for (LONG y = c.top; y < c.bottom; y++)
    {
        size_t o = (size_t)y * t->w + c.left;
        memcpy(t->exp_p + o, t->exp_b + o, w * sizeof(WORD));
        memcpy(t->valid_p + o, t->valid_b + o, w);

        size_t ho = (size_t)(2 * y) * hw + 2 * c.left;
        memcpy(t->hd_p + ho, t->hd_b + ho, 2 * w * sizeof(WORD));
        memcpy(t->hd_p + ho + hw, t->hd_b + ho + hw, 2 * w * sizeof(WORD));
    }
}

int lt_build(LOMHD_TERRAIN* t, const WORD* frame, int frame_pitch_px, BYTE* mask, WORD* hd,
    int min_permille)
{
    int w = t->w, h = t->h;
    long recorded = 0, matched = 0;

    /* Pass 1: raw state, into mask: 1 matches, 0 recorded but no longer matches, 2 not terrain. */
    for (int y = 0; y < h; y++)
    {
        const WORD* f = frame + (size_t)y * frame_pitch_px;
        size_t o = (size_t)y * w;

        for (int x = 0; x < w; x++)
        {
            BYTE m = 2;

            if (t->valid_p[o + x])
            {
                recorded++;
                m = f[x] == t->exp_p[o + x];
                matched += m;
            }

            mask[o + x] = m;
        }
    }

    if (!recorded || matched * 1000 < recorded * (long)min_permille)
    {
        if (recorded)
            lt_forget_primary(t, NULL);

        memset(mask, 0, (size_t)w * h);
        return 0;
    }

    /* Pass 2: keep a match only when none of its four neighbours is terrain that stopped matching.
     * A neighbour off the frame or never terrain (the interface panel under the map) is no evidence
     * either way. In place, a row behind: a lagging copy of the previous raw row is kept, and the
     * row below is still raw when it is read. */
    BYTE* prev = calloc(w, 1);
    BYTE* cur = calloc(w, 1);
    int drawn = 0;

    if (!prev || !cur)
    {
        free(prev);
        free(cur);
        memset(mask, 0, (size_t)w * h);
        return 0;
    }

    for (int y = 0; y < h; y++)
    {
        BYTE* row = mask + (size_t)y * w;
        const BYTE* below = y + 1 < h ? row + w : NULL;
        memcpy(cur, row, w);

        for (int x = 0; x < w; x++)
        {
            BOOL keep = cur[x] == 1 &&
                (x == 0 || cur[x - 1]) && (x + 1 == w || cur[x + 1]) &&
                (y == 0 || prev[x]) && (!below || below[x]);

            row[x] = keep ? 255 : 0;
            drawn += keep;
        }

        BYTE* swap = prev;
        prev = cur;
        cur = swap;
    }

    free(prev);
    free(cur);

    if (hd)
        memcpy(hd, t->hd_p, (size_t)w * h * 4 * sizeof(WORD));

    return drawn;
}
