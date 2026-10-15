#include <string.h>
#include "lomhd_reimpl.h"

uint32_t lomhd_reimpl_fnv(const uint8_t *p, size_t n)
{
    uint32_t h = 0x811c9dc5u;

    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 0x01000193u;

    return h;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int lomhd_reimpl_header_ok(const uint8_t *image, size_t size)
{
    if (size < 0x40 || image[0] != 'M' || image[1] != 'Z')
        return 0;

    uint32_t pe = rd32(image + 0x3c);

    if (pe > size || size - pe < 24 + 60 || memcmp(image + pe, "PE\0\0", 4) != 0)
        return 0;

    const uint8_t *coff = image + pe + 4;
    const uint8_t *opt = image + pe + 24;

    return (coff[0] | coff[1] << 8) == 0x14c && rd32(coff + 4) == LOMHD_EXE_TIMESTAMP &&
        (opt[0] | opt[1] << 8) == 0x10b && rd32(opt + 56) == LOMHD_EXE_IMAGE_SIZE;
}

int lomhd_reimpl_code_ok(const uint8_t *code, size_t len, uint32_t fnv)
{
    return lomhd_reimpl_fnv(code, len) == fnv;
}

void lomhd_reimpl_make_jmp(uint8_t out[5], uint32_t from, uint32_t to)
{
    uint32_t rel = to - (from + 5);

    out[0] = 0xe9;
    out[1] = (uint8_t)rel;
    out[2] = (uint8_t)(rel >> 8);
    out[3] = (uint8_t)(rel >> 16);
    out[4] = (uint8_t)(rel >> 24);
}
