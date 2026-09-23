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

#endif
