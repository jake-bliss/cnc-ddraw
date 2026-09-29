#ifndef LOMHD_MATCH_H
#define LOMHD_MATCH_H

#include <windows.h>

/* The part of the HD overlay that finds known images in a frame. Pure: no cnc-ddraw state, no GL,
 * no files -- the pack is read through a callback -- so tests can run the exact shipped code
 * against captured frames. */

#define LOMHD_PACK_MAGIC "LOMHDPK5"     /* the one place the format is named: reader and loader */
#define LOMHD_MAX_PLACEMENTS 64        /* a map screen holds dozens of trees and buildings */
#define LOMHD_PROBES 3                  /* rows per picture that can each start a match */
#define LOMHD_SPRITE_PROBE_ROWS 4       /* rows per sprite, each probed in up to 3 column bands:
                                         * a tree in front of a tree covers one side of it */
#define LOMHD_PROBE_W 32                /* pixels hashed per probe of a picture */
#define LOMHD_SPRITE_PROBE_W 8          /* ... of a sprite. Map armies are drawn from the half-size
                                         * unit sprites, most narrower than 16 (2026-09-23) */
#define LOMHD_SPRITE_MIN_COLOURS 4      /* distinct colours a sprite probe must hold: a run of one
                                         * colour (black) hit 80,000 places a frame, and every hit
                                         * was a full comparison -- 27 s a scan (2026-09-23) */
#define LOMHD_SPRITE_FRACTION 0.70      /* of a sprite's opaque pixels: trees overlap each other */
#define LOMHD_SHADOW_INDEX 1            /* IMP palette index drawn as the background darkened */
#define LOMHD_MAX_IMAGES 131072         /* records a pack may hold */
#define LOMHD_MAX_PROBES (1 << 20)      /* probes a pack may insert; the table is sized to hold them
                                         * at most half full, so 16 MB at most, 32 MB with slack */
#define LOMHD_BAND_ROWS 64              /* the frame is searched in bands of this many rows ... */
#define LOMHD_BAND_VERIFICATIONS 256    /* ... each making at most this many sprite comparisons;
                                         * beyond them the band's sprites are skipped. A budget for
                                         * the whole frame always ran out at the same row, so the
                                         * same lower sprites went unseen scan after scan (Claude
                                         * review, 2026-09-23). Captured frames need < 100 in all */
#define LOMHD_MAX_HD_SIDE 1280          /* an upscale larger than this is refused at load */
#define LOMHD_MATCH_FRACTION 0.85       /* small images: a cursor or tooltip may cover part */
#define LOMHD_LARGE_PIXELS 65536        /* above this an image is "large": see LOMHD_LARGE_FRACTION */
#define LOMHD_LARGE_FRACTION 0.30       /* of a large image's 1-in-16 sample (see lomhd_match.c) */

#define LOMHD_FLAG_MASKED 1             /* a sprite: see lomhd_pack_open */
#define LOMHD_FLAG_MIRROR 2             /* a sprite the game also draws flipped left to right */

/* Reads len bytes at offset of the pack file into out; FALSE on any failure. */
typedef BOOL (*LOMHD_READ)(void* ctx, DWORD offset, DWORD len, BYTE* out);

/* A palette as the matcher uses it. Every frame of one sprite shares its palette and colour key, so
 * records with the same palette as the record before them share one of these: 1.3 KB apiece was
 * 60 MB over the animated frames. */
typedef struct
{
    WORD lut[2][256];                   /* palette index -> RGB565, truncating and rounding */
    BYTE skip[256];                     /* masked only: 1 for the colour key and the shadow index */
} LOMHD_PALETTE;

typedef struct
{
    char name[40];
    int w, h, hw, hh;
    BOOL masked;                        /* a sprite: `skip` pixels are not part of it, and its
                                         * upscale is RGBA rather than RGB */
    BOOL mirror;                        /* a sprite also searched for flipped left to right */
    int group;                          /* the animated sprite this frame belongs to; 0 = none */
    int group_first, group_count;       /* the group's records, which are consecutive */
    int palette;                        /* index into LOMHD_PACK.palettes ... */
    const LOMHD_PALETTE* pal;           /* ... and the entry itself */
    int opaque, opaque_sample;          /* pixels that count, in full and in the sample */
    BYTE* idx;                          /* w*h palette indices; resident for small pictures only */
    BYTE* sample;                       /* every 4th pixel of every 4th row, sw*sh; pictures only */
    int sw, sh;
    WORD* spans;                        /* sprites: (row, col, length) of every opaque run ... */
    BYTE* opix;                         /* ... and those runs' indices, back to back */
    int nspans;
    int whole;                          /* sprites: this record is the top rows of that record (a
                                         * strip window setup cut from it), or -1 */
    DWORD idx_off, idx_len;             /* zlib(w*h indices) in the pack file */
    DWORD hd_off, hd_len;               /* zlib(hw*hh*3 RGB, or *4 RGBA if masked) in the pack */
} PORTRAIT;

typedef struct
{
    DWORD check;                        /* the hash's high 32 bits; its low bits chose the slot */
    int portrait;                       /* -1 = empty */
    WORD row, col;                      /* where in the (flipped, if mirror) image the slice starts */
    BYTE width;                         /* LOMHD_PROBE_W or LOMHD_SPRITE_PROBE_W */
    BYTE rule;
    BYTE mirror;
    BYTE unused;
} PROBE;

typedef struct
{
    int portrait, rule, x, y, mirror;
} PLACEMENT;

typedef struct
{
    PORTRAIT* portraits;
    int count;
    int allocated;                      /* portraits[] entries, for lomhd_pack_free */
    LOMHD_PALETTE* palettes;
    int palette_count;
    PROBE* table;
    DWORD table_mask;                   /* table size - 1; the size is a power of two */
    int probes;                         /* entries in use, for the load log */
    int probe_width;                    /* LOMHD_PROBE_W, kept for the load log */
    int sprites;                        /* masked images, for the load log */
    unsigned long long pow, sprite_pow; /* HASH_BASE^(width-1) for each probe width */
    size_t resident;                    /* bytes this pack holds in memory, for the load log */
} LOMHD_PACK;

/* What a search did, for logs and tests. */
typedef struct
{
    int verifications;                  /* sprite comparisons made */
    BOOL over_budget;                   /* a band spent LOMHD_BAND_VERIFICATIONS: sprites skipped */
} LOMHD_STATS;

/* What the first 8 bytes of a file say: this build's pack, another LOMHDPK version (made by an
 * older or newer setup -- the player should run it again), or not a pack at all. The loader asks this
 * before opening; the version once lived there as its own literal and fell behind (2026-09-23). */
enum { LOMHD_PACK_UNKNOWN, LOMHD_PACK_CURRENT, LOMHD_PACK_OTHER_VERSION };
int lomhd_pack_version(const BYTE* head, DWORD got);

/* Read a pack's index and every image's indices (which it inflates to build probes and samples,
 * so a damaged index stream is caught here). Upscales stay in the file. Returns FALSE, and sets
 * *bad_offset, on anything unexpected: a half-parsed pack would draw the wrong image somewhere. */
BOOL lomhd_pack_open(LOMHD_READ read, void* ctx, DWORD file_size, LOMHD_PACK* pack, DWORD* bad_offset);

/* Release everything lomhd_pack_open allocated. Safe on a zeroed or already-freed pack. */
void lomhd_pack_free(LOMHD_PACK* pack);

/* Clear every set pixel of a w*h mask (255 = draw) that has an unset neighbour, of the eight: a
 * picture's upscale is wrong there, carrying whatever the original held next to it -- the key green
 * above the interface bar, the button baked under an icon (2026-09-24, seen live as a fringe and
 * as squares). Outside the image counts as set, so a picture drawn whole keeps its border. */
void lomhd_erode_mask(BYTE* m, int w, int h);

/* Whether a record's mask is eroded: pictures only. A sprite's edge is its upscale's own alpha. */
BOOL lomhd_mask_erodes(const PORTRAIT* r);

/* The frames to load ahead of frame p: the next `max` of its animated sprite's group, wrapping to
 * its first frames (a looping animation's first frames follow its last) and never p itself. Writes
 * their indices to out and returns how many; 0 for a record in no group. */
int lomhd_group_ahead(const LOMHD_PACK* pack, int p, int max, int* out);

/* Inflate one image's full indices (w*h) or upscale (hw*hh*3 RGB, *4 RGBA if masked) into a
 * malloc'd buffer the caller frees. Exactly that size or nothing: output past it is refused while
 * inflating. */
BYTE* lomhd_load_indices(const LOMHD_PACK* pack, int portrait, LOMHD_READ read, void* ctx);
BYTE* lomhd_load_upscale(const LOMHD_PACK* pack, int portrait, LOMHD_READ read, void* ctx);

/* Every known image in the frame, up to max. Frame is RGB565, pitch in pixels. Candidates of a
 * similar size at the same spot -- an upgraded building and the level below it -- are rivals, and
 * only the best is returned; images of very different sizes (a portrait on a screen) are both.
 * Sprites are rivals only at the very same top-left: overlapping trees are all there. `stats` may
 * be NULL. */
int lomhd_find(const LOMHD_PACK* pack, const WORD* frame, int width, int height, int pitch_px,
    PLACEMENT* out, int max, LOMHD_STATS* stats);

#endif
