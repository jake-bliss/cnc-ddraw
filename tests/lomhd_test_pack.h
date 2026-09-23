/* Building format-4 packs in memory for the tests, and reading them back through the same callback
 * the overlay uses for its file. zlib is written by hand -- "stored" deflate blocks, no compression
 * -- so the tests do not depend on a compressor they would then be trusting. */
#ifndef LOMHD_TEST_PACK_H
#define LOMHD_TEST_PACK_H

#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "lomhd_match.h"

#define TP_MAX 6000

static BYTE tp_buf[1 << 24];
static DWORD tp_len;

typedef struct { const BYTE* data; DWORD size; } TP_MEM;

static BOOL tp_read(void* ctx, DWORD off, DWORD len, BYTE* out)
{
    TP_MEM* m = ctx;
    if ((unsigned long long)off + len > m->size) return FALSE;
    memcpy(out, m->data + off, len);
    return TRUE;
}

static unsigned long tp_adler32(const BYTE* p, DWORD n)
{
    unsigned long a = 1, b = 0;
    for (DWORD i = 0; i < n; i++) { a = (a + p[i]) % 65521; b = (b + a) % 65521; }
    return (b << 16) | a;
}

/* zlib of n bytes as stored blocks; returns its length. */
static DWORD tp_zlib(BYTE* out, const BYTE* in, DWORD n)
{
    DWORD o = 0;
    out[o++] = 0x78; out[o++] = 0x01;
    for (DWORD at = 0; ; )
    {
        DWORD chunk = n - at > 65535 ? 65535 : n - at;
        BOOL last = at + chunk == n;
        out[o++] = (BYTE)last;
        out[o++] = (BYTE)chunk; out[o++] = (BYTE)(chunk >> 8);
        out[o++] = (BYTE)~chunk; out[o++] = (BYTE)(~chunk >> 8);
        if (in) memcpy(out + o, in + at, chunk); else memset(out + o, 0, chunk);
        o += chunk; at += chunk;
        if (last) break;
    }
    unsigned long a = in ? tp_adler32(in, n) : 0;
    if (!in) { unsigned long x = 1, y = 0; for (DWORD i = 0; i < n; i++) y = (y + x) % 65521; a = (y << 16) | x; }
    out[o++] = (BYTE)(a >> 24); out[o++] = (BYTE)(a >> 16); out[o++] = (BYTE)(a >> 8); out[o++] = (BYTE)a;
    return o;
}

typedef struct
{
    char name[40];
    int w, h, hw, hh;
    const BYTE* idx;                    /* w*h */
    const BYTE* pal;                    /* 768, or NULL for grey (i, i, i) */
    BYTE flags, key;                    /* flags bit 0: masked (a sprite), key its colour key */
    DWORD idx_bytes, hd_bytes;          /* what the streams really inflate to; 0 = the right size */
} TP_RECORD;

static TP_RECORD tp_records[TP_MAX];
static int tp_count;

static void tp_begin(void) { tp_count = 0; }

static TP_RECORD* tp_add(const char* name, int w, int h, const BYTE* idx)
{
    TP_RECORD* r = &tp_records[tp_count++];
    memset(r, 0, sizeof(*r));
    strncpy(r->name, name, sizeof(r->name) - 1);
    r->w = w; r->h = h; r->hw = 2 * w; r->hh = 2 * h; r->idx = idx;
    return r;
}

/* A sprite: indices `key` and LOMHD_SHADOW_INDEX are not part of it; its upscale is RGBA. */
static TP_RECORD* tp_add_sprite(const char* name, int w, int h, const BYTE* idx, BYTE key)
{
    TP_RECORD* r = tp_add(name, w, h, idx);
    r->flags = 1;
    r->key = key;
    return r;
}

static void tp_put(const void* p, DWORD n) { memcpy(tp_buf + tp_len, p, n); tp_len += n; }
static void tp_u16(int v) { WORD x = (WORD)v; tp_put(&x, 2); }
static void tp_u32(DWORD v) { tp_put(&v, 4); }

/* Lay the pack out: header, index, then each record's two streams. Returns its size. */
static DWORD tp_finish(void)
{
    static BYTE z[1 << 23], scratch[1 << 23];
    static DWORD zlen[TP_MAX][2];
    tp_len = 0;
    tp_put(LOMHD_PACK_MAGIC, 8);
    tp_u32((DWORD)tp_count);

    /* Streams first into z (to learn their lengths), then the index, then the streams. */
    DWORD zpos = 0;
    for (int i = 0; i < tp_count; i++)
    {
        TP_RECORD* r = &tp_records[i];
        DWORD n = r->idx_bytes ? r->idx_bytes : (DWORD)(r->w * r->h);
        if (n <= (DWORD)(r->w * r->h)) memcpy(scratch, r->idx, n);
        else { memcpy(scratch, r->idx, r->w * r->h); memset(scratch + r->w * r->h, 0, n - r->w * r->h); }
        zlen[i][0] = tp_zlib(z + zpos, scratch, n); zpos += zlen[i][0];
        DWORD m = r->hd_bytes ? r->hd_bytes : (DWORD)(r->hw * r->hh * ((r->flags & 1) ? 4 : 3));
        zlen[i][1] = tp_zlib(z + zpos, NULL, m); zpos += zlen[i][1];
    }
    for (int i = 0; i < tp_count; i++)
    {
        TP_RECORD* r = &tp_records[i];
        BYTE n = (BYTE)strlen(r->name), pal[768];
        for (int k = 0; k < 768; k++) pal[k] = r->pal ? r->pal[k] : (BYTE)(k / 3);
        tp_put(&n, 1); tp_put(r->name, n);
        tp_u16(r->w); tp_u16(r->h); tp_u16(r->hw); tp_u16(r->hh);
        tp_put(&r->flags, 1); tp_put(&r->key, 1);
        tp_put(pal, 768);
        tp_u32(zlen[i][0]); tp_u32(zlen[i][1]);
    }
    tp_put(z, zpos);
    return tp_len;
}

static BOOL tp_open(DWORD size, LOMHD_PACK* pack, TP_MEM* mem)
{
    DWORD bad;
    mem->data = tp_buf; mem->size = size;
    return lomhd_pack_open(tp_read, mem, size, pack, &bad);
}

#endif
