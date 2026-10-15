/* The pure half of the runtime redirect (src/lomhd_reimpl_core.c).
 *
 *     cc -Iinc -Itests/native -std=c99 -Wall -o /tmp/lr tests/lomhd_reimpl_test.c \
 *         src/lomhd_reimpl_core.c && /tmp/lr
 *
 * If LOMSE_EXE points at lomse.exe it also checks every table row against the real bytes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lomhd_reimpl.h"

static int g_fail;

#define CHECK(cond, ...) do { if (!(cond)) { g_fail++; printf("FAIL %s:%d ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static void fake_header(uint8_t *img, uint32_t stamp, uint32_t size)
{
    memset(img, 0, 0x1000);
    img[0] = 'M'; img[1] = 'Z';
    put32(img + 0x3c, 0x80);
    memcpy(img + 0x80, "PE\0\0", 4);
    img[0x84] = 0x4c; img[0x85] = 0x01;
    put32(img + 0x88, stamp);
    img[0x98] = 0x0b; img[0x99] = 0x01;
    put32(img + 0x98 + 56, size);
}

static void test_jmp(void)
{
    uint8_t j[5];

    lomhd_reimpl_make_jmp(j, 0x404ab0, 0x10001000);
    CHECK(j[0] == 0xe9, "opcode");
    uint32_t rel = j[1] | j[2] << 8 | j[3] << 16 | (uint32_t)j[4] << 24;
    CHECK(0x404ab0u + 5 + rel == 0x10001000u, "forward target");
    lomhd_reimpl_make_jmp(j, 0x10001000, 0x404ab0);
    rel = j[1] | j[2] << 8 | j[3] << 16 | (uint32_t)j[4] << 24;
    CHECK(0x10001000u + 5 + rel == 0x404ab0u, "backward target (wraps)");
}

static void test_hash(void)
{
    CHECK(lomhd_reimpl_fnv((const uint8_t *)"", 0) == 0x811c9dc5u, "empty");
    CHECK(lomhd_reimpl_fnv((const uint8_t *)"a", 1) == 0xe40c292cu, "fnv1a('a')");
    uint8_t b[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint32_t h = lomhd_reimpl_fnv(b, 8);
    CHECK(lomhd_reimpl_code_ok(b, 8, h), "same bytes");
    b[7] ^= 1;
    CHECK(!lomhd_reimpl_code_ok(b, 8, h), "one bit changed must fail");
}

static void test_header(void)
{
    static uint8_t img[0x1000];

    fake_header(img, LOMHD_EXE_TIMESTAMP, LOMHD_EXE_IMAGE_SIZE);
    CHECK(lomhd_reimpl_header_ok(img, sizeof(img)), "good header");
    fake_header(img, LOMHD_EXE_TIMESTAMP + 1, LOMHD_EXE_IMAGE_SIZE);
    CHECK(!lomhd_reimpl_header_ok(img, sizeof(img)), "wrong timestamp");
    fake_header(img, LOMHD_EXE_TIMESTAMP, LOMHD_EXE_IMAGE_SIZE + 0x1000);
    CHECK(!lomhd_reimpl_header_ok(img, sizeof(img)), "wrong size of image");
    fake_header(img, LOMHD_EXE_TIMESTAMP, LOMHD_EXE_IMAGE_SIZE);
    img[0] = 'X';
    CHECK(!lomhd_reimpl_header_ok(img, sizeof(img)), "no MZ");
    fake_header(img, LOMHD_EXE_TIMESTAMP, LOMHD_EXE_IMAGE_SIZE);
    put32(img + 0x3c, 0xfffffff0u);
    CHECK(!lomhd_reimpl_header_ok(img, sizeof(img)), "e_lfanew out of range");
    CHECK(!lomhd_reimpl_header_ok(img, 0x20), "truncated");
}

static void test_real_exe(const char *path)
{
    FILE *f = fopen(path, "rb");

    if (!f) {
        printf("skip real exe: cannot open %s\n", path);
        return;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    uint8_t *d = malloc(n);
    if (fread(d, 1, n, f) != (size_t)n) { printf("short read\n"); g_fail++; return; }
    fclose(f);
    CHECK(lomhd_reimpl_header_ok(d, 0x1000), "real exe header");
    /* .text: VA 0x401000 at file offset 0x400 */
#define ROW(a, l, h, nm, fn, c) CHECK(lomhd_reimpl_code_ok(d + (a) - 0x401000 + 0x400, l, h), "row %s", nm);
    LOMHD_REIMPL_LIST(ROW)
#undef ROW
    free(d);
    printf("real exe rows checked\n");
}

int main(void)
{
    test_jmp();
    test_hash();
    test_header();
    if (getenv("LOMSE_EXE"))
        test_real_exe(getenv("LOMSE_EXE"));
    printf(g_fail ? "FAILED %d\n" : "all ok\n", g_fail);
    return g_fail != 0;
}
