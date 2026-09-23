#ifndef LOMHD_MATCH_H
#define LOMHD_MATCH_H

#include <windows.h>

/* The part of the HD overlay that finds known images in a frame. Pure: no cnc-ddraw state, no GL,
 * no files, so tests/lomhd_match_test.c can run the exact shipped code against captured frames. */

#define LOMHD_MAX_PLACEMENTS 8
#define LOMHD_PROBES 3                  /* rows per image that can each start a match */
#define LOMHD_PROBE_W 32                /* pixels hashed per probe: one width scans every image */
#define LOMHD_TABLE 16384               /* power of two; count x 2 rules x probes must stay under half */
#define LOMHD_MAX_HD_SIDE 512           /* an upscale larger than this is refused at load */
#define LOMHD_MATCH_FRACTION 0.85       /* a cursor or tooltip may cover part of an image */

typedef struct
{
    char name[40];
    int w, h, hw, hh;
    WORD* templ[2];                     /* original pixels as RGB565, truncating and rounding */
    BYTE* hd_rgb;                       /* hw*hh*3, owned: expanded (v1) or inflated (v2) at load */
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
    int version;                        /* 1: palette upscales; 2: zlib RGB upscales, any width */
    PROBE* table;
    int probe_width;                    /* LOMHD_PROBE_W, kept for the load log */
    unsigned long long pow;
} LOMHD_PACK;

/* Parse a pack held in memory. Returns FALSE, and sets *bad_offset, on anything unexpected: a
 * half-parsed pack would draw the wrong image somewhere, which is worse than drawing none. */
BOOL lomhd_pack_parse(const BYTE* data, DWORD size, LOMHD_PACK* pack, DWORD* bad_offset);

/* Release everything lomhd_pack_parse allocated. Safe on a zeroed or already-freed pack. */
void lomhd_pack_free(LOMHD_PACK* pack);

/* Every known image in the frame, up to max. Frame is RGB565, pitch in pixels. Where candidates
 * overlap -- an upgraded building shares most of its pixels with the level below -- only the one
 * that matches best is returned. */
int lomhd_find(const LOMHD_PACK* pack, const WORD* frame, int width, int height, int pitch_px,
    PLACEMENT* out, int max);

#endif
