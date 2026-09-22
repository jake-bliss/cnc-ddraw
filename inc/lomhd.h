#ifndef LOMHD_H
#define LOMHD_H

/* Lords of Magic HD overlay. Everything this fork adds lives behind these calls, so the upstream
 * files each carry one line and the fork stays easy to rebase onto a newer cnc-ddraw. */

/* Called by the renderer with g_ddraw.cs held, once per rendered frame. */
void lomhd_on_frame(const char* renderer);

#endif
