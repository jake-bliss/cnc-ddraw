#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "lomhd_match.h"
#include "lodepng.h"

/* Finding known images in a frame. Pure -- see lomhd_match.h. The pack format (5) is described at
 * lomhd_pack_open. Formats 1 and 2 held every upscale in memory at once; with full-screen art that
 * is ~1.8 GB in a 32-bit process, so format 3 keeps upscales in the file until they are drawn.
 * Format 4 adds sprites, which have pixels that are not part of them; format 5 animated sprites,
 * tens of thousands of frames, some drawn mirrored. The pack is rebuilt by the setup script rather
 * than read in an older shape.
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

/* A probe before the table exists: the table is sized once every probe is known. */
typedef struct
{
    unsigned long long hash;
    PROBE probe;
} PENDING;

static void table_insert(LOMHD_PACK* pack, const PENDING* e)
{
    for (DWORD i = (DWORD)e->hash & pack->table_mask; ; i = (i + 1) & pack->table_mask)
    {
        if (pack->table[i].portrait < 0)
        {
            pack->table[i] = e->probe;
            pack->table[i].check = (DWORD)(e->hash >> 32);
            return;
        }
    }
}

/* Where in a row to take the probe slice: the 32 pixels with the most distinct colours. A flat
 * slice -- black, sky, bare parchment -- also occurs all over the frame, and every such hit costs a
 * full comparison of a large image: the first multi-width build took ~10 s a frame against 2.5 ms
 * (2026-09-22). Ties go to the middle, away from shared frame borders.
 *
 * A sprite's slice must also be wholly opaque -- behind a transparent pixel the frame shows
 * whatever is underneath -- so `opaque` (NULL for a picture) limits the choice, every column is
 * tried rather than every other one, and -1 means the row has no such slice. */
static int busiest_slice(const WORD* row, int w, int width, const BYTE* opaque, int from, int to)
{
    int best = opaque ? -1 : (w - width) / 2, best_distinct = -1, mid = (from + to - 1) / 2;

    for (int col = from; col < to && col + width <= w; col += opaque ? 1 : 2)
    {
        int distinct = 0, solid = 1;

        for (int i = 0; opaque && i < width && solid; i++)
            solid = opaque[col + i];

        if (!solid)
            continue;

        for (int i = 0; i < width; i++)
        {
            BOOL repeat = FALSE;

            for (int j = 0; j < i && !repeat; j++)
                repeat = row[col + j] == row[col + i];

            distinct += !repeat;
        }

        int dist = col > mid ? col - mid : mid - col, best_dist = best > mid ? best - mid : mid - best;

        if (best < 0 || distinct > best_distinct || (distinct == best_distinct && dist < best_dist))
        {
            best = col;
            best_distinct = distinct;
        }
    }

    /* A sprite's slice of few colours -- a run of black -- occurs all over a frame. */
    return opaque && best_distinct < LOMHD_SPRITE_MIN_COLOURS ? -1 : best;
}

void lomhd_pack_free(LOMHD_PACK* pack)
{
    if (pack->portraits)
    {
        for (int p = 0; p < pack->allocated; p++)
        {
            free(pack->portraits[p].idx);
            free(pack->portraits[p].sample);
            free(pack->portraits[p].spans);
            free(pack->portraits[p].opix);
        }

        HeapFree(GetProcessHeap(), 0, pack->portraits);
    }

    if (pack->table)
        HeapFree(GetProcessHeap(), 0, pack->table);

    free(pack->palettes);

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
    return load_exact(read, ctx, r->hd_off, r->hd_len, (size_t)r->hw * r->hh * (r->masked ? 4 : 3));
}

static DWORD u32_at(const BYTE* b) { return b[0] | b[1] << 8 | b[2] << 16 | (DWORD)b[3] << 24; }
static int u16_at(const BYTE* b) { return b[0] | b[1] << 8; }

/* Format 5, little-endian:
 *   "LOMHDPK5", u32 count, then count index records:
 *     u8 name_len, name, u16 w, u16 h, u16 hw, u16 hh, u8 flags, u8 key, u16 group,
 *     768-byte palette, u32 idx_len, u32 hd_len
 *   then the streams, in record order with nothing between them: zlib(w*h indices), then
 *   zlib(hw*hh*3 RGB) -- or hw*hh*4 RGBA for a masked image -- for each record. The last stream
 *   ends at the end of the file.
 * flags bit 0 is MASKED, a sprite: pixels of index `key` (its colour key) or LOMHD_SHADOW_INDEX
 * are not part of it. Bit 1 is MIRROR: the game also draws this sprite flipped left to right, as it
 * does map armies facing the other way (2026-09-23), so it is searched for both ways round. Every
 * other bit must be clear; an unmasked image's key, MIRROR bit and group must be 0. `group` numbers
 * the frames of one animated sprite, whose records must be consecutive; 0 is a sprite or picture
 * on its own. A sprite needs LOMHD_PROBES rows with a LOMHD_SPRITE_PROBE_W opaque run, and is never
 * "large".
 * No offsets are stored, so none can point anywhere odd: each is the sum of the lengths before it,
 * and every stream is checked to lie inside the file. Every image's indices are inflated here (to
 * build its probes and sample), so a damaged index stream refuses the pack; a damaged upscale is
 * found only when it is first drawn, and that image alone is then switched off. */
int lomhd_pack_version(const BYTE* head, DWORD got)
{
    if (got < 8 || memcmp(head, LOMHD_PACK_MAGIC, 7) != 0)
        return LOMHD_PACK_UNKNOWN;

    return head[7] == (BYTE)LOMHD_PACK_MAGIC[7] ? LOMHD_PACK_CURRENT : LOMHD_PACK_OTHER_VERSION;
}

/* A sprite's opaque runs, which are all its comparison needs: a unit frame is mostly transparent,
 * and holding every index of 47,000 frames took 350 MB (2026-09-23). */
static BOOL build_spans(PORTRAIT* r, const BYTE* idx)
{
    int nspans = 0, nopaque = 0;

    for (int j = 0; j < r->h; j++)
        for (int i = 0; i < r->w; i++)
            if (!r->pal->skip[idx[j * r->w + i]])
            {
                nopaque++;
                nspans += i == 0 || r->pal->skip[idx[j * r->w + i - 1]];
            }

    r->spans = malloc(sizeof(WORD) * 3 * (nspans ? nspans : 1));
    r->opix = malloc(nopaque ? nopaque : 1);

    if (!r->spans || !r->opix)
        return FALSE;

    int k = 0, o = 0;

    for (int j = 0; j < r->h; j++)
        for (int i = 0; i < r->w; )
        {
            if (r->pal->skip[idx[j * r->w + i]])
            {
                i++;
                continue;
            }

            int start = i;

            while (i < r->w && !r->pal->skip[idx[j * r->w + i]])
                r->opix[o++] = idx[j * r->w + i++];

            r->spans[k * 3] = (WORD)j;
            r->spans[k * 3 + 1] = (WORD)start;
            r->spans[k * 3 + 2] = (WORD)(i - start);
            k++;
        }

    r->nspans = nspans;
    return TRUE;
}

/* Append a probe to the pending list, growing it. */
static BOOL pending_add(PENDING** list, int* n, int* cap, unsigned long long hash, int portrait,
    int width, int rule, int row, int col, int mirror)
{
    if (*n == *cap)
    {
        int grown = *cap ? *cap * 2 : 4096;
        PENDING* bigger = realloc(*list, sizeof(PENDING) * grown);

        if (!bigger)
            return FALSE;

        *list = bigger;
        *cap = grown;
    }

    PENDING* e = &(*list)[(*n)++];
    memset(e, 0, sizeof(*e));
    e->hash = hash;
    e->probe.portrait = portrait;
    e->probe.width = (BYTE)width;
    e->probe.rule = (BYTE)rule;
    e->probe.row = (WORD)row;
    e->probe.col = (WORD)col;
    e->probe.mirror = (BYTE)mirror;
    return TRUE;
}

BOOL lomhd_pack_open(LOMHD_READ read, void* ctx, DWORD size, LOMHD_PACK* pack, DWORD* bad_offset)
{
    BYTE head[12], rec[8 + 2 + 2 + 768 + 8];
    unsigned long long pos = 12;
    PENDING* pending = NULL;
    int npending = 0, cap = 0;
    memset(pack, 0, sizeof(*pack));
    *bad_offset = 0;

    if (size < 12 || !read(ctx, 0, 12, head) || memcmp(head, LOMHD_PACK_MAGIC, 8) != 0)
        return FALSE;

    DWORD raw_count = u32_at(head + 8);

    if (raw_count == 0 || raw_count > LOMHD_MAX_IMAGES)
    {
        *bad_offset = 8;
        return FALSE;
    }

    int count = (int)raw_count;
    pack->portraits = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(PORTRAIT) * count);
    pack->allocated = pack->portraits ? count : 0;
    int palette_cap = 0;

    /* An allocation failure is a refusal like any other, freed like any other. */
    if (!pack->portraits)
        goto corrupt;

    BYTE last_pal[768], last_key = 0, last_masked = 0;

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
        BYTE flags = rec[8], key = rec[9];
        r->group = u16_at(rec + 10);
        const BYTE* pal = rec + 12;
        r->masked = (flags & LOMHD_FLAG_MASKED) != 0;
        r->mirror = (flags & LOMHD_FLAG_MIRROR) != 0;
        r->idx_len = u32_at(rec + 12 + 768);
        r->hd_len = u32_at(rec + 12 + 768 + 4);

        /* A probe hashes LOMHD_PROBE_W pixels (LOMHD_SPRITE_PROBE_W for a sprite) of LOMHD_PROBES
         * distinct rows; smaller images are refused rather than half-matched. An upscale over the
         * side limit would never be drawn. */
        int min_w = r->masked ? LOMHD_SPRITE_PROBE_W : LOMHD_PROBE_W;

        if ((flags & ~(LOMHD_FLAG_MASKED | LOMHD_FLAG_MIRROR)) ||
            (!r->masked && (key || r->mirror || r->group)) ||
            (r->masked && (long long)r->w * r->h > LOMHD_LARGE_PIXELS) ||
            r->w < min_w || r->h < LOMHD_PROBES + 1 || r->hw < 1 || r->hh < 1 ||
            r->hw > LOMHD_MAX_HD_SIDE || r->hh > LOMHD_MAX_HD_SIDE || !r->idx_len || !r->hd_len)
            goto corrupt;

        /* A group's frames are consecutive: a group seen before, but not just before, is not. */
        if (r->group)
        {
            const PORTRAIT* prev = p ? &pack->portraits[p - 1] : NULL;

            if (prev && prev->group == r->group)
            {
                r->group_first = prev->group_first;
            }
            else
            {
                for (int q = 0; q < p; q++)
                    if (pack->portraits[q].group == r->group)
                        goto corrupt;

                r->group_first = p;
            }

            pack->portraits[r->group_first].group_count++;
        }

        /* Frames of one sprite share a palette: reuse the one before when it is the same. */
        if (p == 0 || memcmp(pal, last_pal, 768) != 0 || key != last_key || r->masked != last_masked)
        {
            if (pack->palette_count == palette_cap)
            {
                int grown = palette_cap ? palette_cap * 2 : 64;
                LOMHD_PALETTE* bigger = realloc(pack->palettes, sizeof(LOMHD_PALETTE) * grown);

                if (!bigger)
                    goto corrupt;

                pack->palettes = bigger;
                palette_cap = grown;
            }

            LOMHD_PALETTE* lp = &pack->palettes[pack->palette_count++];
            memset(lp->skip, 0, sizeof(lp->skip));

            if (r->masked)
                lp->skip[key] = lp->skip[LOMHD_SHADOW_INDEX] = 1;

            for (int rule = 0; rule < 2; rule++)
                for (int i = 0; i < 256; i++)
                    lp->lut[rule][i] = rule ? rgb565_round(pal + i * 3) : rgb565_truncate(pal + i * 3);

            memcpy(last_pal, pal, 768);
            last_key = key;
            last_masked = (BYTE)r->masked;
        }

        r->palette = pack->palette_count - 1;
        pos += 1 + n + sizeof(rec);
    }

    /* Every member of a group knows the group's extent, not just its first record. Palettes are
     * pointed at only now: the list moved while it grew. */
    for (int p = 0; p < count; p++)
        pack->portraits[p].pal = &pack->palettes[pack->portraits[p].palette];

    for (int p = 0; p < count; p++)
        if (pack->portraits[p].group)
            pack->portraits[p].group_count = pack->portraits[pack->portraits[p].group_first].group_count;

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
    pack->pow = pack->sprite_pow = 1;

    for (int i = 1; i < LOMHD_PROBE_W; i++)
        pack->pow *= HASH_BASE;

    for (int i = 1; i < LOMHD_SPRITE_PROBE_W; i++)
        pack->sprite_pow *= HASH_BASE;

    for (int p = 0; p < count; p++)
    {
        PORTRAIT* r = &pack->portraits[p];
        const LOMHD_PALETTE* lp = r->pal;
        BYTE* idx = lomhd_load_indices(pack, p, read, ctx);
        WORD* row565 = malloc(sizeof(WORD) * r->w);
        WORD* flipped = malloc(sizeof(WORD) * r->w);
        BYTE* opaque = malloc(r->w);
        int* rows = malloc(sizeof(int) * r->h);

        pos = r->idx_off;

        if (!idx || !row565 || !flipped || !opaque || !rows)
        {
            free(idx);
            free(row565);
            free(flipped);
            free(opaque);
            free(rows);
            goto corrupt;
        }

        /* The pixels that count, and the rows a probe can start from: every row for a picture; for
         * a sprite, the rows holding an opaque run a whole probe wide. */
        int width = r->masked ? LOMHD_SPRITE_PROBE_W : LOMHD_PROBE_W, eligible = 0;
        r->opaque = r->opaque_sample = 0;

        for (int j = 0; j < r->h; j++)
        {
            int run = 0, longest = 0;

            for (int i = 0; i < r->w; i++)
            {
                BOOL in = !lp->skip[idx[j * r->w + i]];
                r->opaque += in;
                run = in ? run + 1 : 0;
                longest = run > longest ? run : longest;
            }

            if (!r->masked || longest >= width)
                rows[eligible++] = j;
        }

        /* A picture's sample, for its pre-check (or, when large, its whole score). A sprite has no
         * pre-check (see score) and is compared through its spans instead. */
        BOOL ok = eligible >= LOMHD_PROBES;

        if (ok && r->masked)
        {
            ok = build_spans(r, idx);
        }
        else if (ok)
        {
            r->sw = (r->w + 3) / 4;
            r->sh = (r->h + 3) / 4;
            r->sample = malloc((size_t)r->sw * r->sh);
            ok = r->sample != NULL;

            for (int j = 0; ok && j < r->sh; j++)
                for (int i = 0; i < r->sw; i++)
                    r->sample[j * r->sw + i] = idx[(j * 4) * r->w + i * 4];

            for (int k = 0; ok && k < r->sw * r->sh; k++)
                r->opaque_sample += !lp->skip[r->sample[k]];
        }

        int nrows = r->masked ? (eligible < LOMHD_SPRITE_PROBE_ROWS ? eligible : LOMHD_SPRITE_PROBE_ROWS)
                              : LOMHD_PROBES;
        int bands = r->masked ? 3 : 1;

        for (int k = 1; ok && k <= nrows; k++)
        {
            /* Several probe rows, so a cursor sitting on one does not hide the image. A picture's
             * are at quarters of its height, as they always were, one slice each. A sprite's are
             * spread over its eligible rows, with a slice in each third of the row that has one:
             * trees overlap sideways, and a probe under the tree in front finds nothing. */
            int row = r->masked ? rows[(eligible - 1) * (k - 1) / (nrows > 1 ? nrows - 1 : 1)]
                                : r->h * k / (LOMHD_PROBES + 1);
            int starts = r->w - width + 1, last = -1;

            for (int i = 0; i < r->w; i++)
            {
                row565[i] = lp->lut[0][idx[row * r->w + i]];
                opaque[i] = !lp->skip[idx[row * r->w + i]];
            }

            for (int b = 0; ok && b < bands; b++)
            {
                int col = r->masked
                    ? busiest_slice(row565, r->w, width, opaque, starts * b / bands, starts * (b + 1) / bands)
                    : busiest_slice(row565, r->w, width, NULL, 0, starts);

                if (col < 0 || col == last)
                    continue;

                last = col;

                /* Both RGB565 rules for a picture. A sprite has only the truncating one: every
                 * sprite match in the captures used it, 1,876 of 1,876 (2026-09-23), and the
                 * second rule doubled the table. */
                for (int rule = 0; ok && rule < (r->masked ? 1 : 2); rule++)
                {
                    for (int i = 0; i < r->w; i++)
                        row565[i] = lp->lut[rule][idx[row * r->w + i]];

                    ok = pending_add(&pending, &npending, &cap, run_hash(row565 + col, width), p,
                        width, rule, row, col, 0);

                    /* Flipped, the same slice reads backwards and starts w - col - width in. */
                    if (ok && r->mirror)
                    {
                        for (int i = 0; i < width; i++)
                            flipped[i] = row565[col + width - 1 - i];

                        ok = pending_add(&pending, &npending, &cap, run_hash(flipped, width), p,
                            width, rule, row, r->w - col - width, 1);
                    }
                }

                for (int i = 0; i < r->w; i++)
                    row565[i] = lp->lut[0][idx[row * r->w + i]];
            }
        }

        free(row565);
        free(flipped);
        free(opaque);
        free(rows);

        if (!ok || npending > LOMHD_MAX_PROBES)
        {
            free(idx);
            goto corrupt;
        }

        pack->sprites += r->masked;

        /* Small pictures keep every index for the full count; a large one keeps only its sample,
         * and a sprite its spans. Their full indices are loaded when found, for the mask. */
        if (!r->masked && (long long)r->w * r->h <= LOMHD_LARGE_PIXELS)
            r->idx = idx;
        else
            free(idx);
    }

    /* The table, at most half full: a miss walks the chain to an empty slot, on every pixel of
     * every frame. */
    DWORD slots = 1024;

    while (slots < 2u * (DWORD)npending)
        slots *= 2;

    pack->table = HeapAlloc(GetProcessHeap(), 0, sizeof(PROBE) * slots);

    if (!pack->table)
        goto corrupt;

    pack->table_mask = slots - 1;

    for (DWORD i = 0; i < slots; i++)
        pack->table[i].portrait = -1;

    for (int i = 0; i < npending; i++)
        table_insert(pack, &pending[i]);

    pack->probes = npending;
    free(pending);

    for (int p = 0; p < count; p++)
        pack->portraits[p].window = pack->portraits[p].masked &&
            strncmp(pack->portraits[p].name, LOMHD_WINDOW_PREFIX, sizeof(LOMHD_WINDOW_PREFIX) - 1) == 0;

    pack->resident = sizeof(PORTRAIT) * count + sizeof(LOMHD_PALETTE) * pack->palette_count +
        sizeof(PROBE) * slots;

    for (int p = 0; p < count; p++)
    {
        const PORTRAIT* r = &pack->portraits[p];
        pack->resident += (r->idx ? (size_t)r->w * r->h : 0) + (size_t)r->sw * r->sh +
            sizeof(WORD) * 3 * r->nspans + (r->masked ? (size_t)r->opaque : 0);
    }

    pack->count = count;
    return TRUE;

corrupt:
    free(pending);
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
    const WORD* lut = r->pal->lut[rule];
    const BYTE* skip = r->pal->skip;
    int allowed_misses = r->opaque - needed, misses = 0;

    for (int j = 0; j < r->h; j++)
    {
        const WORD* row = frame + (y + j) * pitch_px + x;
        const BYTE* t = r->idx + j * r->w;

        for (int i = 0; i < r->w; i++)
            if (!skip[t[i]] && row[i] != lut[t[i]] && ++misses > allowed_misses)
                return -1;
    }

    return r->opaque - misses;
}

/* The same for a sprite, over its opaque runs only; mirrored, image column i is frame column
 * w - 1 - i. */
static int count_spans(const WORD* frame, int pitch_px, int x, int y, const PORTRAIT* r, int rule,
    int mirror, int needed)
{
    const WORD* lut = r->pal->lut[rule];
    const BYTE* t = r->opix;
    int allowed_misses = r->opaque - needed, misses = 0;

    for (int k = 0; k < r->nspans; k++)
    {
        const WORD* span = r->spans + k * 3;
        const WORD* row = frame + (y + span[0]) * pitch_px + x;
        int col = span[1], n = span[2];

        if (!mirror)
        {
            for (int i = 0; i < n; i++)
                if (row[col + i] != lut[t[i]] && ++misses > allowed_misses)
                    return -1;
        }
        else
        {
            for (int i = 0; i < n; i++)
                if (row[r->w - 1 - col - i] != lut[t[i]] && ++misses > allowed_misses)
                    return -1;
        }

        t += n;
    }

    return r->opaque - misses;
}

/* The same over the 1-in-16 sample: every 4th pixel of every 4th row. */
static int count_sample(const WORD* frame, int pitch_px, int x, int y, const PORTRAIT* r, int rule,
    int needed)
{
    const WORD* lut = r->pal->lut[rule];
    const BYTE* skip = r->pal->skip;
    int total = r->opaque_sample, allowed_misses = total - needed, misses = 0;

    for (int j = 0; j < r->sh; j++)
    {
        const WORD* row = frame + (y + j * 4) * pitch_px + x;
        const BYTE* t = r->sample + j * r->sw;

        for (int i = 0; i < r->sw; i++)
            if (!skip[t[i]] && row[i * 4] != lut[t[i]] && ++misses > allowed_misses)
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
 * still shows the original's exact pixel, so a partly covered screen still draws correctly.
 *
 * A sprite is scored on its opaque pixels only, at LOMHD_SPRITE_FRACTION: on the map, trees stand in
 * front of trees, and captured ones matched 81-88% (2026-09-23). It has no pre-check: a sprite is
 * small, and a sample can fail where the full count passes -- cover only the sampled rows and 83%
 * still matches (Codex review, 2026-09-23). Transparent and shadow pixels show whatever is behind,
 * so they cannot count. */
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
        sc.total = r->opaque;
        sc.needed = (int)((r->masked ? LOMHD_SPRITE_FRACTION : LOMHD_MATCH_FRACTION) * sc.total);
    }

    return sc;
}

static int score(const WORD* frame, int pitch_px, int x, int y, const PORTRAIT* r, int rule,
    int mirror, int needed)
{
    if (r->masked)
        return count_spans(frame, pitch_px, x, y, r, rule, mirror, needed);

    if (is_large(r))
        return count_sample(frame, pitch_px, x, y, r, rule, needed);

    int samples = r->opaque_sample;

    if (samples >= 64 &&
        count_sample(frame, pitch_px, x, y, r, rule, (int)(LOMHD_SPARSE_FRACTION * samples)) < 0)
        return -1;

    return count_matches(frame, pitch_px, x, y, r, rule, needed);
}

/* Two placements are alternatives for one spot when they share at least half of the smaller one
 * AND are of a similar size (neither more than twice the other's area). Upgrade levels of a
 * building are both; a portrait drawn on a full screen, or two pictures that touch, are not.
 *
 * A sprite is an alternative only at the very same top-left. On the map, sprites overlap all the
 * time -- a forest is trees in front of trees -- and each is really there; what is not is a
 * look-alike matched where another stands, and those line up exactly: three library levels
 * matched at one spot in a capture (2026-09-23). */
static BOOL same_spot(const PLACEMENT* a, const PORTRAIT* ra, int x, int y, const PORTRAIT* rb)
{
    if (ra->masked != rb->masked)
        return FALSE;                   /* a sprite on a picture is on it, not instead of it */

    if (ra->masked)
        return a->x == x && a->y == y;

    int right = a->x + ra->w < x + rb->w ? a->x + ra->w : x + rb->w;
    int bottom = a->y + ra->h < y + rb->h ? a->y + ra->h : y + rb->h;
    int w = right - (a->x > x ? a->x : x), h = bottom - (a->y > y ? a->y : y);
    int area_a = ra->w * ra->h, area_b = rb->w * rb->h;
    int smaller = area_a < area_b ? area_a : area_b, larger = area_a < area_b ? area_b : area_a;

    if (w <= 0 || h <= 0 || larger > 2 * smaller)
        return FALSE;

    return 2 * w * h >= smaller;
}

/* Whether `win`, one of setup's strip windows, is exactly the top rows of `frame`: same width and
 * palette, and its runs and their pixels are the frame's first ones. Decided by content where the
 * two meet, not by pack position -- two frames of one sprite can share their top rows, and then
 * both frames hold the window. Only asked of records at one top-left, which is rare. */
static BOOL window_of(const PORTRAIT* win, const PORTRAIT* frame)
{
    if (!win->window || frame->window || !frame->masked || frame->w != win->w || frame->h <= win->h ||
        frame->nspans <= win->nspans || memcmp(frame->pal, win->pal, sizeof(*win->pal)) != 0)
        return FALSE;

    if (memcmp(frame->spans, win->spans, sizeof(WORD) * 3 * win->nspans) != 0 ||
        frame->spans[3 * win->nspans] < win->h)
        return FALSE;

    return memcmp(frame->opix, win->opix, win->opaque) == 0;
}

typedef struct
{
    const LOMHD_PACK* pack;
    const WORD* frame;
    int width, height, pitch_px, max, found;
    PLACEMENT* out;
    LOMHD_STATS stats;
    int band_verifications;             /* this band's sprite comparisons so far */
    int failed[8][4], nfailed;          /* wholes that did not pass where their window was found */
    int matched_of[LOMHD_MAX_PLACEMENTS], total_of[LOMHD_MAX_PLACEMENTS];
} SEARCH;

/* Every probe of this width whose hash is `h`, for the window starting at (x, y). */
static void try_probes(SEARCH* s, unsigned long long h, int probe_width, int x, int y)
{
    const LOMHD_PACK* pack = s->pack;
    PLACEMENT* out = s->out;

    DWORD check = (DWORD)(h >> 32);

    for (DWORD i = (DWORD)h & pack->table_mask; pack->table[i].portrait >= 0; i = (i + 1) & pack->table_mask)
    {
        const PROBE* probe = &pack->table[i];

        if (probe->check != check || probe->width != probe_width)
            continue;

        const PORTRAIT* r = &pack->portraits[probe->portrait];
        int left = x - probe->col, top = y - probe->row;

        if (left < 0 || top < 0 || left + r->w > s->width || top + r->h > s->height)
            continue;

        /* Every candidate at the same spot must be beaten, not just the first found; the same
         * image seen again (another probe row, the other RGB565 rule) is just another candidate.
         * Needing more than the best rival lets the count stop early on repeats. Scores are
         * compared as fractions by cross-multiplying, in integers. */
        /* A whole figure and a strip window cut from it (setup's strip__ records) share a top-left
         * when the window is the figure's top rows. Where the whole is drawn and
         * passes it is the better answer however the window scores -- the window is at 100%
         * whenever the covered rows are all below it -- so it need not beat the window, and a
         * window never replaces its whole. Any other pair at one spot is decided by score. */
        SCORING sc = scoring(r);
        int needed = sc.needed;
        BOOL inside = FALSE, over_window = FALSE;

        for (int k = 0; k < s->found; k++)
        {
            const PORTRAIT* rk = &pack->portraits[out[k].portrait];

            if (!same_spot(&out[k], rk, left, top, r))
                continue;

            if (window_of(r, rk))
            {
                inside = TRUE;
                break;
            }

            if (window_of(rk, r))
            {
                over_window = TRUE;
                continue;
            }

            int beat = (int)((long long)s->matched_of[k] * sc.total / s->total_of[k]) + 1;

            if (beat > needed)
                needed = beat;
        }

        if (inside || needed > sc.total)
            continue;

        /* In the strip the whole usually fails (it passes only when the window holds 70% of it,
         * and then drawing it is the same pixels), and each of its probe hits there would count
         * it again: once is enough per spot and orientation. */
        BOOL tried = FALSE;

        for (int k = 0; over_window && k < s->nfailed && k < 8; k++)
            tried |= s->failed[k][0] == probe->portrait && s->failed[k][1] == left && s->failed[k][2] == top &&
                s->failed[k][3] == probe->mirror;

        if (tried)
            continue;

        /* A frame no capture comes near -- a battle nobody has recorded -- must not stall the game:
         * past its band's budget, that band's sprites are skipped. Pictures are always searched. */
        if (r->masked && s->band_verifications >= LOMHD_BAND_VERIFICATIONS)
        {
            s->stats.over_budget = TRUE;
            continue;
        }

        s->stats.verifications += r->masked;
        s->band_verifications += r->masked;

        int matched = score(s->frame, s->pitch_px, left, top, r, probe->rule, probe->mirror, needed);

        if (matched < 0)
        {
            if (over_window)
            {
                int* f = s->failed[s->nfailed++ % 8];
                f[0] = probe->portrait;
                f[1] = left;
                f[2] = top;
                f[3] = probe->mirror;
            }

            continue;
        }

        /* It beats everything at its spot: drop those, then add it. */
        int kept = 0;

        for (int k = 0; k < s->found; k++)
        {
            if (same_spot(&out[k], &pack->portraits[out[k].portrait], left, top, r))
                continue;

            out[kept] = out[k];
            s->matched_of[kept] = s->matched_of[k];
            s->total_of[kept] = s->total_of[k];
            kept++;
        }

        s->found = kept;

        if (s->found == s->max)
            continue;

        out[s->found] = (PLACEMENT){ probe->portrait, probe->rule, left, top, probe->mirror };
        s->matched_of[s->found] = matched;
        s->total_of[s->found] = sc.total;
        s->found++;
    }
}

int lomhd_find(const LOMHD_PACK* pack, const WORD* frame, int width, int height, int pitch_px,
    PLACEMENT* out, int max, LOMHD_STATS* stats)
{
    const int W = LOMHD_PROBE_W, SW = LOMHD_SPRITE_PROBE_W;
    SEARCH s = { pack, frame, width, height, pitch_px, max, 0, out };

    if (stats)
        memset(stats, 0, sizeof(*stats));

    if (!pack->count || width < SW || max > LOMHD_MAX_PLACEMENTS)
        return 0;

    /* Two rolling hashes per row: pictures' 32-pixel probes and sprites' 8-pixel ones. The second
     * costs nothing in a pack without sprites. */
    for (int y = 0; y < height; y++)
    {
        const WORD* row = frame + y * pitch_px;

        if (y % LOMHD_BAND_ROWS == 0)
            s.band_verifications = 0;
        unsigned long long h = width >= W ? run_hash(row, W) : 0, hs = run_hash(row, SW);

        for (int x = 0; x + SW <= width; x++)
        {
            if (x + W <= width)
            {
                if (x > 0)
                    h = (h - (row[x - 1] + 1ULL) * pack->pow) * HASH_BASE + row[x + W - 1] + 1;

                try_probes(&s, h, W, x, y);
            }

            if (pack->sprites)
            {
                if (x > 0)
                    hs = (hs - (row[x - 1] + 1ULL) * pack->sprite_pow) * HASH_BASE + row[x + SW - 1] + 1;

                try_probes(&s, hs, SW, x, y);
            }
        }
    }

    if (stats)
        *stats = s.stats;

    return s.found;
}

int lomhd_group_ahead(const LOMHD_PACK* pack, int p, int max, int* out)
{
    const PORTRAIT* r = &pack->portraits[p];
    int n = 0;

    if (!r->group)
        return 0;

    for (int k = 1; k <= max && k < r->group_count; k++)
        out[n++] = r->group_first + (p - r->group_first + k) % r->group_count;

    return n;
}

void lomhd_erode_mask(BYTE* m, int w, int h)
{
    /* Mark first (128), clear after: a pixel cleared in this pass must not clear its neighbours. */
    for (int y = 0; y < h; y++)
    {
        for (int x = 0; x < w; x++)
        {
            if (m[y * w + x] != 255)
                continue;

            for (int dy = -1; dy <= 1 && m[y * w + x] == 255; dy++)
            {
                for (int dx = -1; dx <= 1; dx++)
                {
                    int nx = x + dx, ny = y + dy;

                    if (nx >= 0 && nx < w && ny >= 0 && ny < h && m[ny * w + nx] == 0)
                    {
                        m[y * w + x] = 128;
                        break;
                    }
                }
            }
        }
    }

    for (int i = 0; i < w * h; i++)
    {
        if (m[i] == 128)
            m[i] = 0;
    }
}

BOOL lomhd_mask_erodes(const PORTRAIT* r)
{
    return !r->masked;
}
