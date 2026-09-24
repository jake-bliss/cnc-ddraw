/* Just enough of windows.h to build the pure lomhd parts natively (clang/gcc on macOS or Linux),
 * for tests that need no Windows. Only what lomhd_terrain_core.c uses. */
#ifndef LOMHD_NATIVE_WINDOWS_H
#define LOMHD_NATIVE_WINDOWS_H
#include <stdint.h>
typedef uint8_t BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef int32_t LONG;
typedef int BOOL;
#define TRUE 1
#define FALSE 0
typedef struct { LONG left, top, right, bottom; } RECT;
typedef struct { LONG x, y; } POINT;
static inline BOOL SetRect(RECT* r, LONG l, LONG t, LONG rr, LONG b)
{
    r->left = l; r->top = t; r->right = rr; r->bottom = b;
    return TRUE;
}
#endif
