#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "lomhd_match.h"
#include "lodepng.h"

/* Finding known images in a frame. Pure -- see lomhd_match.h.
 *
 * Pack formats, little-endian, both "count u32, then records":
 *   LOMHDPK1  name; original (u16 w, u16 h, 768 palette, w*h indices); upscale in the same shape.
 *             Every original is the same width (portraits, 70x67).
 *   LOMHDPK2  name; original as above; upscale as u16 hw, u16 hh, u32 zlen, zlib(hw*hh*3 RGB).
 *             Any widths. Full colour: the overlay draws its own texture, so the upscale need not
 *             be squeezed into the original's 256 colours -- that remap, and the despeckle before
 *             it, were what lost detail in the first building upscales (2026-09-22). */

#define HASH_BASE 1000003ULL

static WORD rgb565_truncate(const BYTE* c)
{
    return (WORD)(((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3));
}

static WORD rgb565_round(const BYTE* c)
{
    int r = (c[0] + 4) >> 3, g = (c[1] + 2) >> 2, b = (c[2] + 4) >> 3;
    return (WORD)(((r > 31 ? 31 : r) << 11) | ((g > 63 ? 63 : g) << 5) | (b > 31 ? 31 : b));
}

/* Rolling polynomial hash over one run of pixels. `+ 1` so a run of black (0x0000) still moves the
 * hash; without it every all-black window would hash to zero and collide. */
static unsigned long long run_hash(const WORD* px, int n)
{
    unsigned long long h = 0;

    for (int i = 0; i < n; i++)
        h = h * HASH_BASE + px[i] + 1;

    return h;
}

static void table_insert(PROBE* table, unsigned long long hash, int portrait, int rule, int row, int col)
{
    for (int i = (int)(hash & (LOMHD_TABLE - 1)); ; i = (i + 1) & (LOMHD_TABLE - 1))
    {
        if (table[i].portrait < 0)
        {
            table[i].hash = hash;
            table[i].portrait = portrait;
            table[i].rule = rule;
            table[i].row = row;
            table[i].col = col;
            return;
        }
    }
}

/* Where in a row to take the probe slice: the 32 pixels with the most distinct colours. A flat
 * slice -- black, sky, bare parchment -- also occurs all over the frame, and every such hit costs a
 * full comparison of a large image: the first multi-width build took ~10 s a frame against 2.5 ms
 * (2026-09-22). Ties go to the middle, away from shared frame borders. */
static int busiest_slice(const WORD* row, int w)
{
    int best = (w - LOMHD_PROBE_W) / 2, best_distinct = -1, mid = best;

    for (int col = 0; col + LOMHD_PROBE_W <= w; col += 2)
    {
        int distinct = 0;

        for (int i = 0; i < LOMHD_PROBE_W; i++)
        {
            BOOL repeat = FALSE;

            for (int j = 0; j < i && !repeat; j++)
                repeat = row[col + j] == row[col + i];

            distinct += !repeat;
        }

        int dist = col > mid ? col - mid : mid - col, best_dist = best > mid ? best - mid : mid - best;

        if (distinct > best_distinct || (distinct == best_distinct && dist < best_dist))
        {
            best = col;
            best_distinct = distinct;
        }
    }

    return best;
}

void lomhd_pack_free(LOMHD_PACK* pack)
{
    if (pack->portraits)
    {
        for (int p = 0; p < pack->allocated; p++)
        {
            for (int rule = 0; rule < 2; rule++)
                if (pack->portraits[p].templ[rule])
                    HeapFree(GetProcessHeap(), 0, pack->portraits[p].templ[rule]);

            free(pack->portraits[p].hd_rgb);        /* malloc'd: lodepng allocates with malloc */
        }

        HeapFree(GetProcessHeap(), 0, pack->portraits);
    }

    if (pack->table)
        HeapFree(GetProcessHeap(), 0, pack->table);

    memset(pack, 0, sizeof(*pack));
}

/* One image block: u16 w, u16 h, then (palette form) 768 + w*h, or (zlib form) u32 zlen + zlen.
 * Every bound is computed in 64 bits: the pack is untrusted input, and w * h reaches 2^32.
 * (32-bit wraparound found by cross-model review, 2026-09-22.) */
static BOOL read_dims(const BYTE* data, DWORD size, DWORD pos, int* w, int* h)
{
    if ((unsigned long long)pos + 4 > size)
        return FALSE;

    *w = *(const WORD*)(data + pos);
    *h = *(const WORD*)(data + pos + 2);
    return *w > 0 && *h > 0;
}

BOOL lomhd_pack_parse(const BYTE* data, DWORD size, LOMHD_PACK* pack, DWORD* bad_offset)
{
    DWORD pos = 12;
    memset(pack, 0, sizeof(*pack));
    *bad_offset = 0;

    if (size < 12)
        return FALSE;

    if (memcmp(data, "LOMHDPK1", 8) == 0)
        pack->version = 1;
    else if (memcmp(data, "LOMHDPK2", 8) == 0)
        pack->version = 2;
    else
        return FALSE;

    DWORD raw_count = *(const DWORD*)(data + 8);

    /* count x 2 rules x LOMHD_PROBES entries must leave the table well under half full, or every
     * miss walks a long probe chain on every pixel of every frame. 64-bit, so a huge count cannot
     * overflow the multiply and pass. */
    if (raw_count == 0 || (unsigned long long)raw_count * 2 * LOMHD_PROBES > LOMHD_TABLE / 2)
    {
        *bad_offset = 8;
        return FALSE;
    }

    int count = (int)raw_count;

    pack->portraits = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(PORTRAIT) * count);
    pack->allocated = pack->portraits ? count : 0;
    pack->table = HeapAlloc(GetProcessHeap(), 0, sizeof(PROBE) * LOMHD_TABLE);

    /* An allocation failure is a refusal like any other, freed like any other. A test that ran
     * 40,000 parses without freeing exhausted a 32-bit heap, and every case after it then
     * "passed" by running out of memory -- caught only because a control run disagreed. */
    if (!pack->portraits || !pack->table)
        goto corrupt;

    for (int i = 0; i < LOMHD_TABLE; i++)
        pack->table[i].portrait = -1;

    for (int p = 0; p < count; p++)
    {
        PORTRAIT* r = &pack->portraits[p];

        if ((unsigned long long)pos + 1 > size || (unsigned long long)pos + 1 + data[pos] > size)
            goto corrupt;

        int n = data[pos];
        memcpy(r->name, data + pos + 1, n < (int)sizeof(r->name) - 1 ? n : (int)sizeof(r->name) - 1);
        pos += 1 + n;

        /* The original: always palette form. It is what the frame is matched against. */
        int w, h;

        if (!read_dims(data, size, pos, &w, &h) ||
            (unsigned long long)pos + 4 + 768 + (unsigned long long)w * h > size)
            goto corrupt;

        /* A probe hashes LOMHD_PROBE_W pixels of LOMHD_PROBES distinct rows; smaller images are
         * refused rather than half-matched. v1 packs also keep their one fixed width. */
        if (w < LOMHD_PROBE_W || h < LOMHD_PROBES + 1 || (pack->version == 1 && p > 0 && w != pack->portraits[0].w))
            goto corrupt;

        const BYTE* pal = data + pos + 4;
        const BYTE* idx = pal + 768;
        r->w = w;
        r->h = h;

        for (int rule = 0; rule < 2; rule++)
        {
            r->templ[rule] = HeapAlloc(GetProcessHeap(), 0, sizeof(WORD) * w * h);

            if (!r->templ[rule])
                goto corrupt;

            for (int i = 0; i < w * h; i++)
                r->templ[rule][i] = rule ? rgb565_round(pal + idx[i] * 3) : rgb565_truncate(pal + idx[i] * 3);
        }

        pos += 4 + 768 + w * h;

        /* The upscale. Anything bigger than the draw's per-placement buffer would be packed and
         * then silently never drawn, so it is refused here instead. */
        int hw, hh;

        if (!read_dims(data, size, pos, &hw, &hh) || hw > LOMHD_MAX_HD_SIDE || hh > LOMHD_MAX_HD_SIDE)
            goto corrupt;

        size_t rgb_size = (size_t)hw * hh * 3;

        if (pack->version == 1)
        {
            if ((unsigned long long)pos + 4 + 768 + (unsigned long long)hw * hh > size)
                goto corrupt;

            const BYTE* hpal = data + pos + 4;
            const BYTE* hidx = hpal + 768;
            r->hd_rgb = malloc(rgb_size);

            if (!r->hd_rgb)
                goto corrupt;

            for (int i = 0; i < hw * hh; i++)
                memcpy(r->hd_rgb + i * 3, hpal + hidx[i] * 3, 3);

            pos += 4 + 768 + hw * hh;
        }
        else
        {
            if ((unsigned long long)pos + 8 > size)
                goto corrupt;

            DWORD zlen = *(const DWORD*)(data + pos + 4);

            if ((unsigned long long)pos + 8 + zlen > size)
                goto corrupt;

            /* This lodepng has no output cap, so a crafted stream could allocate far more than
             * rgb_size before the size check below refuses it. Accepted: the pack is built on the
             * player's own machine by lomhd_setup.py and sits beside ddraw.dll, so whoever can
             * write a hostile pack can replace the DLL itself. The check still refuses anything
             * that is not exact. */
            unsigned char* out = NULL;
            size_t out_size = 0;
            unsigned err = lodepng_zlib_decompress(&out, &out_size, data + pos + 8, zlen,
                &lodepng_default_decompress_settings);

            if (err || out_size != rgb_size)
            {
                free(out);
                goto corrupt;
            }

            /* lodepng grows its buffer 1.5x at a time; keep only what is used, in a 32-bit
             * process holding ~110 MB of these for the whole session. */
            unsigned char* fitted = realloc(out, rgb_size);
            r->hd_rgb = fitted ? fitted : out;
            pos += 8 + zlen;
        }

        r->hw = hw;
        r->hh = hh;
    }

    if (pos != size)
        goto corrupt;

    pack->probe_width = LOMHD_PROBE_W;
    pack->pow = 1;

    for (int i = 1; i < LOMHD_PROBE_W; i++)
        pack->pow *= HASH_BASE;

    for (int p = 0; p < count; p++)
    {
        PORTRAIT* r = &pack->portraits[p];

        for (int k = 1; k <= LOMHD_PROBES; k++)
        {
            /* Several probe rows, so a cursor sitting on one does not hide the image. */
            int row = r->h * k / (LOMHD_PROBES + 1);
            int col = busiest_slice(r->templ[0] + row * r->w, r->w);

            for (int rule = 0; rule < 2; rule++)
                table_insert(pack->table, run_hash(r->templ[rule] + row * r->w + col, LOMHD_PROBE_W),
                    p, rule, row, col);
        }
    }

    pack->count = count;
    return TRUE;

corrupt:
    *bad_offset = pos;
    lomhd_pack_free(pack);
    return FALSE;
}

/* How many pixels agree, or -1 as soon as fewer than `needed` can. The early exit matters: this
 * runs on the render thread under g_ddraw.cs, which the game's own ddraw calls also take. */
static int count_matches(const WORD* frame, int pitch_px, int x, int y, const PORTRAIT* r, int rule,
    int needed)
{
    const WORD* t = r->templ[rule];
    int allowed_misses = r->w * r->h - needed, misses = 0;

    for (int j = 0; j < r->h; j++)
    {
        const WORD* row = frame + (y + j) * pitch_px + x;

        for (int i = 0; i < r->w; i++)
            if (row[i] != t[j * r->w + i] && ++misses > allowed_misses)
                return -1;
    }

    return r->w * r->h - misses;
}

/* A 1-in-16 sample of the image, held to a looser bar than the full count. It only exists to reject
 * a false hit cheaply: a false hit misses most samples and fails after a few dozen pixels. The bar
 * is loose because the sample grid over-weights the top and left edges -- held to the full 85%, a
 * tooltip over a portrait's top 10 rows failed the sample (54 misses, 46 allowed) while the full
 * count passed (700 misses, 704 allowed). Cross-model review, 2026-09-22. It can still refuse a
 * cover striped exactly along the grid (every 4th row); covers on screen are solid blocks. */
#define LOMHD_SPARSE_FRACTION 0.6

static BOOL sparse_agrees(const WORD* frame, int pitch_px, int x, int y, const PORTRAIT* r, int rule)
{
    const WORD* t = r->templ[rule];
    int samples = ((r->h + 3) / 4) * ((r->w + 3) / 4);
    int allowed_misses = samples - (int)(LOMHD_SPARSE_FRACTION * samples), misses = 0;

    /* Too few samples to judge: one covered row of an 8-row image is half its sample rows. */
    if (samples < 64)
        return TRUE;

    for (int j = 0; j < r->h; j += 4)
        for (int i = 0; i < r->w; i += 4)
            if (frame[(y + j) * pitch_px + x + i] != t[j * r->w + i] && ++misses > allowed_misses)
                return FALSE;

    return TRUE;
}

/* Two placements are alternatives for one spot when they share at least half of the smaller one.
 * Upgrade levels of a building share nearly all of it; two different pictures that merely touch,
 * or share a border pixel, are both drawn. */
static BOOL same_spot(const PLACEMENT* a, const PORTRAIT* ra, int x, int y, const PORTRAIT* rb)
{
    int right = a->x + ra->w < x + rb->w ? a->x + ra->w : x + rb->w;
    int bottom = a->y + ra->h < y + rb->h ? a->y + ra->h : y + rb->h;
    int w = right - (a->x > x ? a->x : x), h = bottom - (a->y > y ? a->y : y);
    int smaller = ra->w * ra->h < rb->w * rb->h ? ra->w * ra->h : rb->w * rb->h;

    if (w <= 0 || h <= 0)
        return FALSE;

    return 2 * w * h >= smaller;
}

int lomhd_find(const LOMHD_PACK* pack, const WORD* frame, int width, int height, int pitch_px,
    PLACEMENT* out, int max)
{
    const int W = LOMHD_PROBE_W;
    int matched_of[LOMHD_MAX_PLACEMENTS]; /* pixels matched; a score is this over the area */
    int found = 0;

    if (!pack->count || width < W || max > LOMHD_MAX_PLACEMENTS)
        return 0;

    for (int y = 0; y < height; y++)
    {
        const WORD* row = frame + y * pitch_px;
        unsigned long long h = run_hash(row, W);

        for (int x = 0; x + W <= width; x++)
        {
            if (x > 0)
                h = (h - (row[x - 1] + 1ULL) * pack->pow) * HASH_BASE + row[x + W - 1] + 1;

            for (int i = (int)(h & (LOMHD_TABLE - 1)); pack->table[i].portrait >= 0;
                 i = (i + 1) & (LOMHD_TABLE - 1))
            {
                const PROBE* probe = &pack->table[i];

                if (probe->hash != h)
                    continue;

                const PORTRAIT* r = &pack->portraits[probe->portrait];
                int left = x - probe->col, top = y - probe->row;

                if (left < 0 || top < 0 || left + r->w > width || top + r->h > height)
                    continue;

                /* Every candidate at the same spot must be beaten, not just the first found: a
                 * candidate beating a weak placement while overlapping a strong one would draw its
                 * worse upscale over the strong one. The same image, reached again from another
                 * probe row or the other RGB565 rule, is just another candidate here -- skipping it
                 * as already seen kept a 95% rule-0 match from its 100% rule-1 score. Needing more
                 * than the best rival lets count_matches stop early on every repeat. */
                int area = r->w * r->h;
                int needed = (int)(LOMHD_MATCH_FRACTION * area);

                /* In integers: as doubles, 4060/4690 and 232/268 -- equal -- let the tie through. */
                for (int k = 0; k < found; k++)
                {
                    const PORTRAIT* rk = &pack->portraits[out[k].portrait];
                    int beat = (int)((long long)matched_of[k] * area / (rk->w * rk->h)) + 1;

                    if (same_spot(&out[k], rk, left, top, r) && beat > needed)
                        needed = beat;
                }

                if (needed > area || !sparse_agrees(frame, pitch_px, left, top, r, probe->rule))
                    continue;

                int matched = count_matches(frame, pitch_px, left, top, r, probe->rule, needed);

                if (matched < 0)
                    continue;

                int kept = 0;

                /* It beats everything at its spot: drop those, then add it. */
                for (int k = 0; k < found; k++)
                {
                    if (same_spot(&out[k], &pack->portraits[out[k].portrait], left, top, r))
                        continue;

                    out[kept] = out[k];
                    matched_of[kept] = matched_of[k];
                    kept++;
                }

                found = kept;

                if (found == max)
                    continue;

                out[found] = (PLACEMENT){ probe->portrait, probe->rule, left, top };
                matched_of[found] = matched;
                found++;
            }
        }
    }

    return found;
}
