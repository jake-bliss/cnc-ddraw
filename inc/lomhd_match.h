#ifndef LOMHD_MATCH_H
#define LOMHD_MATCH_H

#include <windows.h>

/* The part of the HD overlay that finds portraits in a frame. Pure: no cnc-ddraw state, no GL, no
 * files, so tests/lomhd_match_test.c can run the exact shipped code against captured frames. */

#define LOMHD_MAX_PLACEMENTS 8
#define LOMHD_PROBES 3                  /* rows per portrait that can each start a match */
#define LOMHD_TABLE 16384                /* power of two, comfortably over 748 x 2 rules x 3 */
#define LOMHD_MATCH_FRACTION 0.85        /* a cursor or tooltip may cover part of a portrait */

typedef struct
{
    char name[40];
    int w, h, hw, hh;
    WORD* templ[2];                     /* original pixels as RGB565, truncating and rounding */
    const BYTE* hd_pal;                 /* 256 x rgb, points into the pack */
    const BYTE* hd_idx;                 /* hw*hh indices, points into the pack */
} PORTRAIT;

typedef struct
{
    unsigned long long hash;
    int portrait;                       /* -1 = empty */
    int rule;
    int row;
} PROBE;

typedef struct
{
    int portrait, rule, x, y;
} PLACEMENT;


typedef struct
{
    PORTRAIT* portraits;
    int count;
    PROBE* table;
    int probe_width;
    unsigned long long pow;
} LOMHD_PACK;

/* Parse a pack held in memory. Returns FALSE, and sets *bad_offset, on anything unexpected: a
 * half-parsed pack would draw the wrong portrait somewhere, which is worse than drawing none. */
BOOL lomhd_pack_parse(const BYTE* data, DWORD size, LOMHD_PACK* pack, DWORD* bad_offset);

/* Every portrait in the frame, up to max. Frame is RGB565, pitch in pixels. */
int lomhd_find(const LOMHD_PACK* pack, const WORD* frame, int width, int height, int pitch_px,
    PLACEMENT* out, int max);

#endif
