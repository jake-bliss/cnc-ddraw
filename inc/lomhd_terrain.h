#ifndef LOMHD_TERRAIN_H
#define LOMHD_TERRAIN_H

#include <windows.h>

/* HD terrain for the "hybrid" exe build (lords-of-magic-modding tools/exe_patches/
 * terrain-hybrid-2x.toml): the game runs at 640x480 as stock, but its map render surface holds a
 * 2x-resolution image of the stock 1x surface space. The game still passes 1x rects for that
 * surface, so the wrapper doubles them, writes a 2:1 downsample wherever the map is copied to a 1x
 * surface, and remembers the 2x pixels behind every downsampled one. At present time the 2x pixels
 * are drawn wherever the frame still shows the downsample -- anything the game drew on top (units,
 * the cursor, the interface) differs, so it stays as the game drew it.
 *
 * This part is pure (no DirectDraw, no GL, no threads), so it builds natively for the tests. */

typedef struct
{
    int w, h;                   /* the 1x frame */
    WORD* exp_b;                /* back buffer: the value the downsample wrote, per 1x pixel */
    WORD* hd_b;                 /* back buffer: the 2x block behind it, 2w x 2h */
    BYTE* valid_b;
    WORD* exp_p;                /* the same, carried to the primary by the present copies */
    WORD* hd_p;
    BYTE* valid_p;
} LOMHD_TERRAIN;

BOOL lt_init(LOMHD_TERRAIN* t, int w, int h);
void lt_free(LOMHD_TERRAIN* t);

/* DirectDraw BltFast clipping, as dds_BltFast does it: src rect (NULL = whole source) clipped to the
 * source, a negative destination point moves the source, the result clipped to the destination.
 * FALSE when nothing is left. */
BOOL lt_clip_fast(const RECT* src_in, int src_w, int src_h, long dx, long dy, int dst_w, int dst_h,
    RECT* src_out, POINT* dst_out);

/* Copies a 1x rect of the 2x map to a 1x surface, taking the top-left pixel of each 2x2 block.
 * src_1x and (dx, dy) are the game's arguments, unclipped. record: the destination is the back
 * buffer, so remember what was written; otherwise, if the destination is the back buffer, forget
 * that rect. Returns the pixels written. */
int lt_map_to_1x(LOMHD_TERRAIN* t, const WORD* map, int map_pitch_px, int map_w, int map_h,
    const RECT* src_1x, long dx, long dy, WORD* dst, int dst_pitch_px, int dst_w, int dst_h,
    BOOL dst_is_back, BOOL record);

/* The back buffer was copied to the primary: src rect at (dx, dy), both already clipped. Carries
 * the record along when the copy is in place (as the game's always are); forgets the destination
 * otherwise. */
void lt_back_to_primary(LOMHD_TERRAIN* t, const RECT* src, POINT dst);

void lt_forget_back(LOMHD_TERRAIN* t, const RECT* r);        /* NULL = all */
void lt_forget_primary(LOMHD_TERRAIN* t, const RECT* r);     /* NULL = all */

/* Builds what to draw from the presented frame: mask (w x h, 255 = draw the 2x block) and hd
 * (2w x 2h, the 2x pixels, RGB as stored). A pixel is drawn where the frame still equals the
 * recorded downsample and so do its four neighbours: an isolated coincidence (a menu pixel that
 * happens to equal the terrain once under it) is never drawn. If fewer than min_permille of the
 * recorded pixels still match, the frame has moved on (a full-screen menu) and the whole record is
 * dropped. Returns the pixels to draw. */
int lt_build(LOMHD_TERRAIN* t, const WORD* frame, int frame_pitch_px, BYTE* mask, WORD* hd,
    int min_permille);

#endif
