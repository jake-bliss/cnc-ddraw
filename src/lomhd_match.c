#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "lomhd_match.h"
#include "lodepng.h"

/* Finding known images in a frame. Pure -- see lomhd_match.h. The pack format (3) is described at
 * lomhd_pack_open. Formats 1 and 2 held every upscale in memory at once; with full-screen art that
 * is ~1.8 GB in a 32-bit process, so format 3 keeps upscales in the file until they are drawn, and
 * the pack is rebuilt by the setup script rather than read in an older shape.
 *
 * Upscales are full colour: the overlay draws its own texture, so the upscale need not be squeezed
 * into the original's 256 colours -- that remap, and the despeckle before it, were what lost
 * detail in the first building upscales (2026-09-22). */

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
            free(pack->portraits[p].idx);
            free(pack->portraits[p].sample);
        }

        HeapFree(GetProcessHeap(), 0, pack->portraits);
    }

    if (pack->table)
        HeapFree(GetProcessHeap(), 0, pack->table);

    memset(pack, 0, sizeof(*pack));
}

/* Inflate exactly `size` bytes or nothing. The bound is enforced while inflating, not after. */
static BYTE* load_exact(LOMHD_READ read, void* ctx, DWORD off, DWORD len, size_t size)
{
    BYTE* z = malloc(len ? len : 1);
    unsigned char* out = NULL;
    size_t got = 0;

    if (z && read(ctx, off, len, z) &&
        lodepng_zlib_decompress_bounded(&out, &got, z, len, size) == 0 && got == size)
    {
        free(z);
        return out;
    }

    free(z);
    free(out);
    return NULL;
}

BYTE* lomhd_load_indices(const LOMHD_PACK* pack, int p, LOMHD_READ read, void* ctx)
{
    const PORTRAIT* r = &pack->portraits[p];
    return load_exact(read, ctx, r->idx_off, r->idx_len, (size_t)r->w * r->h);
}

BYTE* lomhd_load_upscale(const LOMHD_PACK* pack, int p, LOMHD_READ read, void* ctx)
{
    const PORTRAIT* r = &pack->portraits[p];
    return load_exact(read, ctx, r->hd_off, r->hd_len, (size_t)r->hw * r->hh * 3);
}

static DWORD u32_at(const BYTE* b) { return b[0] | b[1] << 8 | b[2] << 16 | (DWORD)b[3] << 24; }
static int u16_at(const BYTE* b) { return b[0] | b[1] << 8; }

/* Format 3, little-endian:
 *   "LOMHDPK3", u32 count, then count index records:
 *     u8 name_len, name, u16 w, u16 h, u16 hw, u16 hh, 768-byte palette, u32 idx_len, u32 hd_len
 *   then the streams, in record order with nothing between them: zlib(w*h indices), then
 *   zlib(hw*hh*3 RGB), for each record. The last stream ends at the end of the file.
 * No offsets are stored, so none can point anywhere odd: each is the sum of the lengths before it,
 * and every stream is checked to lie inside the file. Every image's indices are inflated here (to
 * build its probes and sample), so a damaged index stream refuses the pack; a damaged upscale is
 * found only when it is first drawn, and that image alone is then switched off. */
BOOL lomhd_pack_open(LOMHD_READ read, void* ctx, DWORD size, LOMHD_PACK* pack, DWORD* bad_offset)
{
    BYTE head[12], rec[8 + 768 + 8];
    unsigned long long pos = 12;
    memset(pack, 0, sizeof(*pack));
    *bad_offset = 0;

    if (size < 12 || !read(ctx, 0, 12, head) || memcmp(head, "LOMHDPK3", 8) != 0)
        return FALSE;

    DWORD raw_count = u32_at(head + 8);

    /* count x 2 rules x LOMHD_PROBES entries must leave the table well under half full, or every
     * miss walks a long probe chain on every pixel of every frame. */
    if (raw_count == 0 || (unsigned long long)raw_count * 2 * LOMHD_PROBES > LOMHD_TABLE / 2)
    {
        *bad_offset = 8;
        return FALSE;
    }

    int count = (int)raw_count;
    pack->portraits = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(PORTRAIT) * count);
    pack->allocated = pack->portraits ? count : 0;
    pack->table = HeapAlloc(GetProcessHeap(), 0, sizeof(PROBE) * LOMHD_TABLE);

    /* An allocation failure is a refusal like any other, freed like any other. */
    if (!pack->portraits || !pack->table)
        goto corrupt;

    for (int i = 0; i < LOMHD_TABLE; i++)
        pack->table[i].portrait = -1;

    for (int p = 0; p < count; p++)
    {
        PORTRAIT* r = &pack->portraits[p];
        BYTE n;

        if (pos + 1 > size || !read(ctx, (DWORD)pos, 1, &n) || pos + 1 + n + sizeof(rec) > size)
            goto corrupt;

        char name[256];

        if (!read(ctx, (DWORD)pos + 1, n, (BYTE*)name) || !read(ctx, (DWORD)(pos + 1 + n), sizeof(rec), rec))
            goto corrupt;

        memcpy(r->name, name, n < (int)sizeof(r->name) - 1 ? n : (int)sizeof(r->name) - 1);
        r->w = u16_at(rec);
        r->h = u16_at(rec + 2);
        r->hw = u16_at(rec + 4);
        r->hh = u16_at(rec + 6);
        r->idx_len = u32_at(rec + 8 + 768);
        r->hd_len = u32_at(rec + 8 + 768 + 4);

        /* A probe hashes LOMHD_PROBE_W pixels of LOMHD_PROBES distinct rows; smaller images are
         * refused rather than half-matched. An upscale over the side limit would never be drawn. */
        if (r->w < LOMHD_PROBE_W || r->h < LOMHD_PROBES + 1 || r->hw < 1 || r->hh < 1 ||
            r->hw > LOMHD_MAX_HD_SIDE || r->hh > LOMHD_MAX_HD_SIDE || !r->idx_len || !r->hd_len)
            goto corrupt;

        for (int rule = 0; rule < 2; rule++)
            for (int i = 0; i < 256; i++)
                r->lut[rule][i] = rule ? rgb565_round(rec + 8 + i * 3) : rgb565_truncate(rec + 8 + i * 3);

        pos += 1 + n + sizeof(rec);
    }

    /* The streams: back to back after the index, ending exactly at the end of the file. */
    for (int p = 0; p < count; p++)
    {
        PORTRAIT* r = &pack->portraits[p];
        r->idx_off = (DWORD)pos;
        pos += r->idx_len;
        r->hd_off = (DWORD)pos;
        pos += r->hd_len;

        if (pos > size)
            goto corrupt;
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
        BYTE* idx = lomhd_load_indices(pack, p, read, ctx);
        WORD* row565 = malloc(sizeof(WORD) * r->w);

        pos = r->idx_off;

        if (!idx || !row565)
        {
            free(idx);
            free(row565);
            goto corrupt;
        }

        r->sw = (r->w + 3) / 4;
        r->sh = (r->h + 3) / 4;
        r->sample = malloc((size_t)r->sw * r->sh);

        if (!r->sample)
        {
            free(idx);
            free(row565);
            goto corrupt;
        }

        for (int j = 0; j < r->sh; j++)
            for (int i = 0; i < r->sw; i++)
                r->sample[j * r->sw + i] = idx[(j * 4) * r->w + i * 4];

        for (int k = 1; k <= LOMHD_PROBES; k++)
        {
            /* Several probe rows, so a cursor sitting on one does not hide the image. */
            int row = r->h * k / (LOMHD_PROBES + 1);

            for (int i = 0; i < r->w; i++)
                row565[i] = r->lut[0][idx[row * r->w + i]];

            int col = busiest_slice(row565, r->w);

            for (int rule = 0; rule < 2; rule++)
            {
                for (int i = 0; i < r->w; i++)
                    row565[i] = r->lut[rule][idx[row * r->w + i]];

                table_insert(pack->table, run_hash(row565 + col, LOMHD_PROBE_W), p, rule, row, col);
            }
        }

        free(row565);

        /* Small images keep every index for the full count; a large one keeps only its sample,
         * and its full indices are loaded when it is found and needs a mask. */
        if ((long long)r->w * r->h <= LOMHD_LARGE_PIXELS)
            r->idx = idx;
        else
            free(idx);
    }

    pack->count = count;
    return TRUE;

corrupt:
    *bad_offset = (DWORD)(pos > size ? size : pos);
    lomhd_pack_free(pack);
    return FALSE;
}

static BOOL is_large(const PORTRAIT* r)
{
    return (long long)r->w * r->h > LOMHD_LARGE_PIXELS;
}

/* How many pixels agree, or -1 as soon as fewer than `needed` can. The early exit matters: this
 * runs on the render thread under g_ddraw.cs, which the game's own ddraw calls also take. */
static int count_matches(const WORD* frame, int pitch_px, int x, int y, const PORTRAIT* r, int rule,
    int needed)
{
    const WORD* lut = r->lut[rule];
    int allowed_misses = r->w * r->h - needed, misses = 0;

    for (int j = 0; j < r->h; j++)
    {
        const WORD* row = frame + (y + j) * pitch_px + x;
        const BYTE* t = r->idx + j * r->w;

        for (int i = 0; i < r->w; i++)
            if (row[i] != lut[t[i]] && ++misses > allowed_misses)
                return -1;
    }

    return r->w * r->h - misses;
}

/* The same over the 1-in-16 sample: every 4th pixel of every 4th row. */
static int count_sample(const WORD* frame, int pitch_px, int x, int y, const PORTRAIT* r, int rule,
    int needed)
{
    const WORD* lut = r->lut[rule];
    int total = r->sw * r->sh, allowed_misses = total - needed, misses = 0;

    for (int j = 0; j < r->sh; j++)
    {
        const WORD* row = frame + (y + j * 4) * pitch_px + x;
        const BYTE* t = r->sample + j * r->sw;

        for (int i = 0; i < r->sw; i++)
            if (row[i * 4] != lut[t[i]] && ++misses > allowed_misses)
                return -1;
    }

    return total - misses;
}

/* A small image is scored on every pixel, after a cheap pre-check on its sample at a loose 60%: the
 * sample only has to reject false hits, and its grid over-weights the top and left edges -- held to
 * the full 85%, a tooltip over a portrait's top 10 rows failed the sample while the full count
 * passed (cross-model review, 2026-09-22). Under 64 samples the pre-check is skipped.
 *
 * A large image (a full screen) is scored on its sample alone, at LOMHD_LARGE_FRACTION. Captured
 * 2026-09-23: the start screen matched 99.5% of its pixels, a library page with text on it 76%,
 * the main interface bar under the live map 30-38%. Nothing else in a frame matches 30% of a
 * 20,000-point sample of a 640x480 picture exactly, and the overlay only draws where the frame
 * still shows the original's exact pixel, so a partly covered screen still draws correctly. */
#define LOMHD_SPARSE_FRACTION 0.6

typedef struct
{
    int total, needed;                  /* pixels scored (all, or the sample), and the bar */
} SCORING;

static SCORING scoring(const PORTRAIT* r)
{
    SCORING sc;

    if (is_large(r))
    {
        sc.total = r->sw * r->sh;
        sc.needed = (int)(LOMHD_LARGE_FRACTION * sc.total);
    }
    else
    {
        sc.total = r->w * r->h;
        sc.needed = (int)(LOMHD_MATCH_FRACTION * sc.total);
    }

    return sc;
}

static int score(const WORD* frame, int pitch_px, int x, int y, const PORTRAIT* r, int rule, int needed)
{
    if (is_large(r))
        return count_sample(frame, pitch_px, x, y, r, rule, needed);

    int samples = r->sw * r->sh;

    if (samples >= 64 &&
        count_sample(frame, pitch_px, x, y, r, rule, (int)(LOMHD_SPARSE_FRACTION * samples)) < 0)
        return -1;

    return count_matches(frame, pitch_px, x, y, r, rule, needed);
}

/* Two placements are alternatives for one spot when they share at least half of the smaller one
 * AND are of a similar size (neither more than twice the other's area). Upgrade levels of a
 * building are both; a portrait drawn on a full screen, or two pictures that touch, are not. */
static BOOL same_spot(const PLACEMENT* a, const PORTRAIT* ra, int x, int y, const PORTRAIT* rb)
{
    int right = a->x + ra->w < x + rb->w ? a->x + ra->w : x + rb->w;
    int bottom = a->y + ra->h < y + rb->h ? a->y + ra->h : y + rb->h;
    int w = right - (a->x > x ? a->x : x), h = bottom - (a->y > y ? a->y : y);
    int area_a = ra->w * ra->h, area_b = rb->w * rb->h;
    int smaller = area_a < area_b ? area_a : area_b, larger = area_a < area_b ? area_b : area_a;

    if (w <= 0 || h <= 0 || larger > 2 * smaller)
        return FALSE;

    return 2 * w * h >= smaller;
}

int lomhd_find(const LOMHD_PACK* pack, const WORD* frame, int width, int height, int pitch_px,
    PLACEMENT* out, int max)
{
    const int W = LOMHD_PROBE_W;
    int matched_of[LOMHD_MAX_PLACEMENTS], total_of[LOMHD_MAX_PLACEMENTS];
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

                /* Every candidate at the same spot must be beaten, not just the first found; the
                 * same image seen again (another probe row, the other RGB565 rule) is just another
                 * candidate. Needing more than the best rival lets the count stop early on repeats.
                 * Scores are compared as fractions by cross-multiplying, in integers. */
                SCORING sc = scoring(r);
                int needed = sc.needed;

                for (int k = 0; k < found; k++)
                {
                    const PORTRAIT* rk = &pack->portraits[out[k].portrait];
                    int beat = (int)((long long)matched_of[k] * sc.total / total_of[k]) + 1;

                    if (same_spot(&out[k], rk, left, top, r) && beat > needed)
                        needed = beat;
                }

                if (needed > sc.total)
                    continue;

                int matched = score(frame, pitch_px, left, top, r, probe->rule, needed);

                if (matched < 0)
                    continue;

                /* It beats everything at its spot: drop those, then add it. */
                int kept = 0;

                for (int k = 0; k < found; k++)
                {
                    if (same_spot(&out[k], &pack->portraits[out[k].portrait], left, top, r))
                        continue;

                    out[kept] = out[k];
                    matched_of[kept] = matched_of[k];
                    total_of[kept] = total_of[k];
                    kept++;
                }

                found = kept;

                if (found == max)
                    continue;

                out[found] = (PLACEMENT){ probe->portrait, probe->rule, left, top };
                matched_of[found] = matched;
                total_of[found] = sc.total;
                found++;
            }
        }
    }

    return found;
}
