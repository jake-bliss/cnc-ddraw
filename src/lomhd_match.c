#include <windows.h>
#include <string.h>
#include "lomhd_match.h"

/* Finding portraits in a frame. Pure -- see lomhd_match.h. */

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

/* Rolling polynomial hash over one row of pixels. `+ 1` so a run of black (0x0000) still moves the
 * hash; without it every all-black window would hash to zero and collide. */
static unsigned long long row_hash(const WORD* px, int n)
{
    unsigned long long h = 0;

    for (int i = 0; i < n; i++)
        h = h * HASH_BASE + px[i] + 1;

    return h;
}

static void table_insert(PROBE* table, unsigned long long hash, int portrait, int rule, int row)
{
    for (int i = (int)(hash & (LOMHD_TABLE - 1)); ; i = (i + 1) & (LOMHD_TABLE - 1))
    {
        if (table[i].portrait < 0)
        {
            table[i].hash = hash;
            table[i].portrait = portrait;
            table[i].rule = rule;
            table[i].row = row;
            return;
        }
    }
}

BOOL lomhd_pack_parse(const BYTE* data, DWORD size, LOMHD_PACK* pack, DWORD* bad_offset)
{
    DWORD pos = 12;
    memset(pack, 0, sizeof(*pack));
    *bad_offset = 0;

    if (size < 12 || memcmp(data, "LOMHDPK1", 8) != 0)
        return FALSE;

    int count = *(const DWORD*)(data + 8);

    /* 748 x 2 rules x LOMHD_PROBES entries must leave the table well under half full, or every
     * miss walks a long probe chain on every pixel of every frame. */
    if (count <= 0 || count * 2 * LOMHD_PROBES > LOMHD_TABLE / 2)
    {
        *bad_offset = 8;
        return FALSE;
    }

    pack->portraits = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(PORTRAIT) * count);
    pack->table = HeapAlloc(GetProcessHeap(), 0, sizeof(PROBE) * LOMHD_TABLE);

    if (!pack->portraits || !pack->table)
        return FALSE;

    for (int i = 0; i < LOMHD_TABLE; i++)
        pack->table[i].portrait = -1;

    for (int p = 0; p < count; p++)
    {
        PORTRAIT* r = &pack->portraits[p];

        if (pos + 1 > size || pos + 1 + data[pos] > size)
            goto corrupt;

        int n = data[pos];
        memcpy(r->name, data + pos + 1, n < (int)sizeof(r->name) - 1 ? n : (int)sizeof(r->name) - 1);
        pos += 1 + n;

        for (int image = 0; image < 2; image++)
        {
            if (pos + 4 + 768 > size)
                goto corrupt;

            int w = *(const WORD*)(data + pos), h = *(const WORD*)(data + pos + 2);
            const BYTE* pal = data + pos + 4;
            const BYTE* idx = pal + 768;

            if (w <= 0 || h <= 0 || pos + 4 + 768 + (DWORD)(w * h) > size)
                goto corrupt;

            pos += 4 + 768 + w * h;

            if (image == 0)
            {
                r->w = w;
                r->h = h;

                for (int rule = 0; rule < 2; rule++)
                {
                    r->templ[rule] = HeapAlloc(GetProcessHeap(), 0, sizeof(WORD) * w * h);

                    if (!r->templ[rule])
                        return FALSE;

                    for (int i = 0; i < w * h; i++)
                        r->templ[rule][i] = rule ? rgb565_round(pal + idx[i] * 3)
                                                 : rgb565_truncate(pal + idx[i] * 3);
                }
            }
            else
            {
                r->hw = w;
                r->hh = h;
                r->hd_pal = pal;
                r->hd_idx = idx;
            }
        }

        /* The rolling hash scans one fixed width. Every shipped portrait is 70x67; a pack that
         * breaks that is refused rather than half-matched. */
        if (p == 0)
            pack->probe_width = r->w;

        if (r->w != pack->probe_width || r->h < LOMHD_PROBES + 1)
            goto corrupt;
    }

    if (pos != size)
        goto corrupt;

    pack->pow = 1;

    for (int i = 1; i < pack->probe_width; i++)
        pack->pow *= HASH_BASE;

    for (int p = 0; p < count; p++)
    {
        PORTRAIT* r = &pack->portraits[p];

        for (int rule = 0; rule < 2; rule++)
        {
            for (int k = 1; k <= LOMHD_PROBES; k++)
            {
                /* Several probe rows, so a cursor sitting on one does not hide the portrait. */
                int row = r->h * k / (LOMHD_PROBES + 1);
                table_insert(pack->table, row_hash(r->templ[rule] + row * r->w, r->w), p, rule, row);
            }
        }
    }

    pack->count = count;
    return TRUE;

corrupt:
    *bad_offset = pos;
    return FALSE;
}

static int count_matches(const WORD* frame, int pitch_px, int x, int y, const PORTRAIT* r, int rule)
{
    const WORD* t = r->templ[rule];
    int same = 0;

    for (int j = 0; j < r->h; j++)
    {
        const WORD* row = frame + (y + j) * pitch_px + x;

        for (int i = 0; i < r->w; i++)
            same += row[i] == t[j * r->w + i];
    }

    return same;
}

int lomhd_find(const LOMHD_PACK* pack, const WORD* frame, int width, int height, int pitch_px,
    PLACEMENT* out, int max)
{
    int w = pack->probe_width;
    int found = 0;

    if (!pack->count || width < w)
        return 0;

    for (int y = 0; y < height && found < max; y++)
    {
        const WORD* row = frame + y * pitch_px;
        unsigned long long h = row_hash(row, w);

        for (int x = 0; x + w <= width && found < max; x++)
        {
            if (x > 0)
                h = (h - (row[x - 1] + 1ULL) * pack->pow) * HASH_BASE + row[x + w - 1] + 1;

            for (int i = (int)(h & (LOMHD_TABLE - 1)); pack->table[i].portrait >= 0 && found < max;
                 i = (i + 1) & (LOMHD_TABLE - 1))
            {
                const PROBE* probe = &pack->table[i];

                if (probe->hash != h)
                    continue;

                const PORTRAIT* r = &pack->portraits[probe->portrait];
                int top = y - probe->row;

                if (top < 0 || top + r->h > height)
                    continue;

                /* The same portrait is reached from each probe row and both rules. */
                BOOL duplicate = FALSE;

                for (int k = 0; k < found; k++)
                    if (out[k].x == x && out[k].y == top)
                        duplicate = TRUE;

                if (duplicate)
                    continue;

                if (count_matches(frame, pitch_px, x, top, r, probe->rule) <
                    (int)(LOMHD_MATCH_FRACTION * r->w * r->h))
                    continue;

                out[found].portrait = probe->portrait;
                out[found].rule = probe->rule;
                out[found].x = x;
                out[found].y = top;
                found++;
            }
        }
    }

    return found;
}
