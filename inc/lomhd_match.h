#ifndef LOMHD_MATCH_H
#define LOMHD_MATCH_H

#include <windows.h>

/* The part of the HD overlay that finds known images in a frame. Pure: no cnc-ddraw state, no GL,
 * no files -- the pack is read through a callback -- so tests can run the exact shipped code
 * against captured frames. */

#define LOMHD_MAX_PLACEMENTS 8
#define LOMHD_PROBES 3                  /* rows per image that can each start a match */
#define LOMHD_PROBE_W 32                /* pixels hashed per probe: one width scans every image */
#define LOMHD_TABLE 65536               /* power of two; count x 2 rules x probes must stay under half */
#define LOMHD_MAX_HD_SIDE 1280          /* an upscale larger than this is refused at load */
#define LOMHD_MATCH_FRACTION 0.85       /* small images: a cursor or tooltip may cover part */
#define LOMHD_LARGE_PIXELS 65536        /* above this an image is "large": see LOMHD_LARGE_FRACTION */
#define LOMHD_LARGE_FRACTION 0.30       /* of a large image's 1-in-16 sample (see lomhd_match.c) */

/* Reads len bytes at offset of the pack file into out; FALSE on any failure. */
typedef BOOL (*LOMHD_READ)(void* ctx, DWORD offset, DWORD len, BYTE* out);

typedef struct
{
    char name[40];
    int w, h, hw, hh;
    WORD lut[2][256];                   /* palette index -> RGB565, truncating and rounding */
    BYTE* idx;                          /* w*h palette indices; resident for small images only */
    BYTE* sample;                       /* every 4th pixel of every 4th row, sw*sh; all images */
    int sw, sh;
    DWORD idx_off, idx_len;             /* zlib(w*h indices) in the pack file */
    DWORD hd_off, hd_len;               /* zlib(hw*hh*3 RGB) in the pack file */
} PORTRAIT;

typedef struct
{
    unsigned long long hash;
    int portrait;                       /* -1 = empty */
    int rule;
    int row, col;                       /* where in the image this probe's slice starts */
} PROBE;

typedef struct
{
    int portrait, rule, x, y;
} PLACEMENT;

typedef struct
{
    PORTRAIT* portraits;
    int count;
    int allocated;                      /* portraits[] entries, for lomhd_pack_free */
    PROBE* table;
    int probe_width;                    /* LOMHD_PROBE_W, kept for the load log */
    unsigned long long pow;
} LOMHD_PACK;

/* Read a pack's index and every image's indices (which it inflates to build probes and samples,
 * so a damaged index stream is caught here). Upscales stay in the file. Returns FALSE, and sets
 * *bad_offset, on anything unexpected: a half-parsed pack would draw the wrong image somewhere. */
BOOL lomhd_pack_open(LOMHD_READ read, void* ctx, DWORD file_size, LOMHD_PACK* pack, DWORD* bad_offset);

/* Release everything lomhd_pack_open allocated. Safe on a zeroed or already-freed pack. */
void lomhd_pack_free(LOMHD_PACK* pack);

/* Inflate one image's full indices (w*h) or upscale (hw*hh*3 RGB) into a malloc'd buffer the
 * caller frees. Exactly that size or nothing: output past it is refused while inflating. */
BYTE* lomhd_load_indices(const LOMHD_PACK* pack, int portrait, LOMHD_READ read, void* ctx);
BYTE* lomhd_load_upscale(const LOMHD_PACK* pack, int portrait, LOMHD_READ read, void* ctx);

/* Every known image in the frame, up to max. Frame is RGB565, pitch in pixels. Candidates of a
 * similar size at the same spot -- an upgraded building and the level below it -- are rivals, and
 * only the best is returned; images of very different sizes (a portrait on a screen) are both. */
int lomhd_find(const LOMHD_PACK* pack, const WORD* frame, int width, int height, int pitch_px,
    PLACEMENT* out, int max);

#endif
