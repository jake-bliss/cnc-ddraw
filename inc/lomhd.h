#ifndef LOMHD_H
#define LOMHD_H

#include <windows.h>

/* Lords of Magic HD overlay. Everything this fork adds lives behind these calls, so the upstream
 * files each carry one line and the fork stays easy to rebase onto a newer cnc-ddraw. */

/* Called by the renderer with g_ddraw.cs held, once per rendered frame. */
void lomhd_on_frame(const char* renderer);

/* Called by the OpenGL renderer after it has drawn the scaled frame, before SwapBuffers. */
void lomhd_draw(void);

/* TRUE when a portrait pack sits beside the game. The only upstream behaviour it changes: with
 * renderer=auto, cnc-ddraw picks Direct3D 9 on real Windows, where the overlay does not draw, so the
 * pack's presence tips auto to OpenGL. An explicit renderer setting is never overridden. Called
 * once at startup, before the render thread exists. */
BOOL lomhd_wants_opengl(void);

/* HD terrain for the hybrid exe build; see lomhd_terrain.c. Inert unless that build is running.
 * The Blt hooks return TRUE when they handled the call, with its result in *ret. */
struct IDirectDrawSurfaceImpl;
BOOL lomhd_terrain_active(void);
BOOL lomhd_terrain_bltfast(struct IDirectDrawSurfaceImpl* This, DWORD dwX, DWORD dwY,
    struct IDirectDrawSurfaceImpl* src, LPRECT lpSrcRect, DWORD dwFlags, HRESULT* ret);
BOOL lomhd_terrain_blt(struct IDirectDrawSurfaceImpl* This, LPRECT lpDestRect,
    struct IDirectDrawSurfaceImpl* src, LPRECT lpSrcRect, DWORD dwFlags, void* lpDDBltFx, HRESULT* ret);
void lomhd_terrain_lock(struct IDirectDrawSurfaceImpl* This, void* return_address, void* frame);
void lomhd_terrain_flip(void);
void lomhd_terrain_frame(void);
/* For the debug log: [0] map copies and [1] mask builds since the last call, [2] pixels drawn by the
 * latest build, [3] start-screen locks since the last call. Then every distinct game caller of
 * Lock on the map surface seen so far (the start-screen check keys on one of them). */
void lomhd_terrain_stats(long out[4]);
int lomhd_terrain_lock_callers(DWORD* out, int max);
BOOL lomhd_terrain_snapshot(const BYTE** mask, const WORD** hd, int* w, int* h, LONG* gen);

/* Surface traffic trace, for designing the HD terrain composite. Off unless lomhd_trace sits beside
 * the game; see lomhd_trace.c. Safe to call from any thread. */
void lomhd_trace_surface(const void* s, DWORD width, DWORD height, DWORD bpp, DWORD caps, LONG pitch,
    BOOL caller_memory);
void lomhd_trace_op(char op, const void* dst, const void* src, const RECT* r, long x, long y, DWORD flags);

#endif
