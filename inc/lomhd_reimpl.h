#ifndef LOMHD_REIMPL_H
#define LOMHD_REIMPL_H

/* Runtime redirect of rewritten lomse.exe functions to src/reimpl; see src/lomhd_reimpl.c.
 * The pure half (hash, byte check, jmp encoding, header check) is in lomhd_reimpl_core.c so it
 * can be tested natively. */

#include <stddef.h>
#include <stdint.h>

/* X(original address, length in bytes, FNV-1a of those bytes, name, reimpl symbol, counter index)
 * One row per redirected function. Hash and length are of the exact original code, so a different
 * exe build (or a hybrid edit inside the function) is skipped, never patched. */
#define LOMHD_REIMPL_LIST(X) \
    X(0x404ab0, 36,  0x3c3aaf76u, "script_raise_error",  lom_script_raise_error,  0) \
    X(0x4026a0, 149, 0xf2f38f1fu, "script_value_decode", lom_script_value_decode, 1) \
    X(0x4ac0b0, 363, 0x613c2043u, "op_abracadabra",      lom_op_abracadabra,      2)

#define LOMHD_REIMPL_COUNT 3

/* lomse.exe GS5R3 PE header identity: COFF TimeDateStamp and OptionalHeader SizeOfImage. */
#define LOMHD_EXE_TIMESTAMP 913735361u
#define LOMHD_EXE_IMAGE_SIZE 0x1d8000u
#define LOMHD_EXE_BASE 0x400000u

uint32_t lomhd_reimpl_fnv(const uint8_t *p, size_t n);

/* 1 when `image` (the first `size` bytes of the exe image at its base) is an i386 PE with the
 * expected timestamp and size of image. */
int lomhd_reimpl_header_ok(const uint8_t *image, size_t size);

/* 1 when the `len` bytes at `code` hash to `fnv`. */
int lomhd_reimpl_code_ok(const uint8_t *code, size_t len, uint32_t fnv);

/* Writes `jmp rel32` at `from` targeting `to` into out[5]. */
void lomhd_reimpl_make_jmp(uint8_t out[5], uint32_t from, uint32_t to);

/* Calls made into each rewritten function since startup (index = table counter index). */
extern volatile int32_t lom_reimpl_calls[LOMHD_REIMPL_COUNT];

/* DLL side. Install is a no-op unless a lomhd_reimpl\ folder sits beside the exe. */
void lomhd_reimpl_install(void);
void lomhd_reimpl_report(void);
void lomhd_reimpl_tick(void);
int lomhd_reimpl_active(void);

#endif
