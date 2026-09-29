/* Just enough of windows.h to build the pure lomhd parts natively (clang/gcc on macOS or Linux),
 * for tests that need no Windows. Only what lomhd_terrain_core.c and lomhd_crash_core.c use. */
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
/* Single-threaded tests: the interlocked operations need no atomicity here. */
static inline LONG InterlockedIncrement(volatile LONG* p)
{
    return ++*p;
}
static inline LONG InterlockedExchange(volatile LONG* p, LONG v)
{
    LONG old = *p;
    *p = v;
    return old;
}
static inline LONG InterlockedCompareExchange(volatile LONG* p, LONG v, LONG cmp)
{
    LONG old = *p;
    if (old == cmp)
        *p = v;
    return old;
}
static inline BOOL SetRect(RECT* r, LONG l, LONG t, LONG rr, LONG b)
{
    r->left = l; r->top = t; r->right = rr; r->bottom = b;
    return TRUE;
}
#endif
