/* The pure half of the crash reporter (src/lomhd_crash_core.c): text, module+offset annotation, the
 * log ring, report names, the report layout and SHA-256.
 *
 *     cc -Itests/native -Iinc -std=c99 -Wall -o /tmp/lc tests/lomhd_crash_test.c \
 *         src/lomhd_crash_core.c && /tmp/lc
 *
 * tests/native/windows.h stands in for the real one, so this needs no Windows and no Wine. The
 * Windows half (the filter, the watchdog) is exercised by tests/lomhd_crash_victim.c under Wine. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "lomhd_crash.h"

static int g_fail;

#define CHECK(cond, ...) do { if (!(cond)) { g_fail++; printf("FAIL %s:%d ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static BOOL has(const char* text, const char* want)
{
    return strstr(text, want) != NULL;
}

static void test_text(void)
{
    char buf[16];
    LC_TEXT t;
    lc_text_init(&t, buf, sizeof(buf));
    lc_hex(&t, 0x1A2B, 8);
    lc_puts(&t, " ");
    lc_dec(&t, 0);
    CHECK(strcmp(buf, "00001A2B 0") == 0, "hex and zero: [%s]", buf);

    lc_text_init(&t, buf, sizeof(buf));
    lc_dec(&t, 4294967295u);
    CHECK(strcmp(buf, "4294967295") == 0, "largest dword: [%s]", buf);

    /* Overflow: truncated, terminated, and counted -- never written past the end. */
    char small[8];
    memset(small, 'x', sizeof(small));
    lc_text_init(&t, small, 6);
    lc_puts(&t, "abcdefghij");
    CHECK(strcmp(small, "abcde") == 0 && t.dropped == 5 && small[6] == 'x', "overflow: [%s] dropped %u",
        small, t.dropped);

    lc_text_init(&t, buf, sizeof(buf));
    lc_hex(&t, 0xDEADBEEF, 2);
    CHECK(strcmp(buf, "EF") == 0, "short hex keeps the low digits: [%s]", buf);
}

static LC_MODULE mods[4];

static void set_mod(int i, DWORD base, DWORD size, const char* name)
{
    mods[i].base = base;
    mods[i].size = size;
    strncpy(mods[i].name, name, LC_MODULE_NAME - 1);
}

static void test_modules(void)
{
    /* Out of order on purpose: the snapshot comes from Toolhelp in load order. */
    set_mod(0, 0x15000000, 0x40000, "Storm.dll");
    set_mod(1, 0x00400000, 0x200000, "lomse.exe");
    set_mod(2, 0x6A000000, 0x100000, "ddraw.dll");
    set_mod(3, 0xFFFF0000, 0x10000, "top.dll");
    lc_sort_modules(mods, 4);
    CHECK(mods[0].base == 0x400000 && mods[1].base == 0x15000000 && mods[3].base == 0xFFFF0000,
        "sorted by base");

    char buf[64];
    LC_TEXT t;
    lc_text_init(&t, buf, sizeof(buf));
    lc_addr(&t, mods, 4, 0x15001A2B);
    CHECK(strcmp(buf, "Storm.dll+0x00001A2B") == 0, "inside storm: [%s]", buf);

    lc_text_init(&t, buf, sizeof(buf));
    lc_addr(&t, mods, 4, 0x15040000);
    CHECK(strcmp(buf, "0x15040000") == 0, "one past the end is outside: [%s]", buf);

    lc_text_init(&t, buf, sizeof(buf));
    lc_addr(&t, mods, 4, 0x00400000);
    CHECK(strcmp(buf, "lomse.exe+0x00000000") == 0, "the base itself is inside: [%s]", buf);

    /* A module ending at 4 GB: base + size wraps to 0, which a naive bound check gets wrong. */
    CHECK(lc_find_module(mods, 4, 0xFFFFFFFF) == &mods[3], "module at the top of memory");
    CHECK(lc_find_module(mods, 4, 0x10) == NULL, "near-NULL is in no module");

    CHECK(strcmp(lc_module_kind("LOMSE.EXE"), "lomse") == 0, "kind is case-insensitive");
    CHECK(strcmp(lc_module_kind("lomse_hybrid.exe"), "lomse") == 0, "a patched copy is still lomse");
    CHECK(strcmp(lc_module_kind("storm.dll"), "storm") == 0, "storm");
    CHECK(strcmp(lc_module_kind("stormx.dll"), "other") == 0, "not storm: stormx.dll");
    CHECK(strcmp(lc_module_kind("ddraw.dll"), "ddraw") == 0, "ddraw");
    CHECK(strcmp(lc_module_kind("kernel32.dll"), "other") == 0, "other");
    CHECK(strcmp(lc_module_kind(NULL), "none") == 0, "none");
}

static void test_ring(void)
{
    static LC_RING ring;
    char buf[8192];
    LC_TEXT t;

    lc_text_init(&t, buf, sizeof(buf));
    CHECK(lc_ring_emit(&ring, &t, 30) == 0 && buf[0] == 0, "empty ring emits nothing");

    lc_ring_push(&ring, "first\r\n");
    lc_ring_push(&ring, "second");
    lc_text_init(&t, buf, sizeof(buf));
    CHECK(lc_ring_emit(&ring, &t, 30) == 2, "two lines");
    CHECK(strcmp(buf, "  first\r\n  second\r\n") == 0, "oldest first, CRLF trimmed: [%s]", buf);

    /* Wrap: 40 pushes into 32 slots keep the last 32; asking for 30 gives lines 10..39. */
    memset(&ring, 0, sizeof(ring));
    for (int i = 0; i < 40; i++)
    {
        char line[32];
        snprintf(line, sizeof(line), "line %02d", i);
        lc_ring_push(&ring, line);
    }

    lc_text_init(&t, buf, sizeof(buf));
    CHECK(lc_ring_emit(&ring, &t, 30) == 30, "thirty of forty");
    CHECK(strncmp(buf, "  line 10\r\n", 11) == 0, "starts at line 10: [%.12s]", buf);
    CHECK(has(buf, "  line 39\r\n") && !has(buf, "line 09"), "ends at 39, drops 09");

    lc_text_init(&t, buf, sizeof(buf));
    CHECK(lc_ring_emit(&ring, &t, 100) == LC_RING_LINES, "never more than the ring holds");
    CHECK(strncmp(buf, "  line 08\r\n", 11) == 0, "oldest kept is line 08: [%.12s]", buf);

    /* A line wider than a slot is truncated, not spilled into the next slot. */
    char wide[400];
    memset(wide, 'w', sizeof(wide) - 1);
    wide[sizeof(wide) - 1] = 0;
    memset(&ring, 0, sizeof(ring));
    lc_ring_push(&ring, wide);
    lc_ring_push(&ring, "after");
    CHECK(strlen(ring.line[0]) == LC_RING_WIDTH - 1 && strcmp(ring.line[1], "after") == 0,
        "wide line truncated to %d", LC_RING_WIDTH - 1);

    /* A torn slot with no terminator is still bounded when emitted. */
    memset(ring.line[1], 'z', LC_RING_WIDTH);
    lc_text_init(&t, buf, sizeof(buf));
    lc_ring_emit(&ring, &t, 1);
    CHECK(strlen(buf) == 2 + LC_RING_WIDTH + 2, "torn line bounded: %zu", strlen(buf));
}

static void test_names(void)
{
    char name[64];
    WORD when[6] = { 2026, 9, 7, 4, 5, 9 };
    lc_report_name(name, sizeof(name), "crash", when, 1, ".txt");
    CHECK(strcmp(name, "lomhd_crash_20260907_040509.txt") == 0, "crash name: %s", name);
    lc_report_name(name, sizeof(name), "crash", when, 2, ".txt");
    CHECK(strcmp(name, "lomhd_crash_20260907_040509_2.txt") == 0, "the first collision gets _2: %s", name);
    lc_report_name(name, sizeof(name), "crash", when, 3, ".dmp");
    CHECK(strcmp(name, "lomhd_crash_20260907_040509_3.dmp") == 0, "second in the same second: %s", name);
    WORD late[6] = { 2026, 12, 31, 23, 59, 58 };
    lc_report_name(name, sizeof(name), "hang", late, 1, ".txt");
    CHECK(strcmp(name, "lomhd_hang_20261231_235958.txt") == 0, "hang name: %s", name);
}

static void test_report(void)
{
    static LC_RING ring;
    static char buf[32768];
    lc_ring_push(&ring, "12:00:01 pack: 120 images loaded, format 5, probe width 32");

    LC_SEEN seen[1] = { { 0xC0000005, 0x15000010, 0, 0x0, 7, 900 } };
    LC_REPORT r = { 0 };
    r.kind = "crash";
    r.when[0] = 2026; r.when[1] = 9; r.when[2] = 27; r.when[3] = 14; r.when[4] = 30; r.when[5] = 5;
    r.uptime_ms = 1000;
    r.has_exception = TRUE;
    r.code = 0xC0000005;
    r.address = 0x15001A2B;
    r.nparams = 2;
    r.params[0] = 1;
    r.params[1] = 0x10;
    r.thread_id = 7;
    r.window_thread = TRUE;
    r.has_regs = TRUE;
    r.regs.eip = 0x15001A2B;
    r.regs.eax = 0x00401234;
    r.regs.esp = 0x0019FF00;
    r.stack_addr = 0x0019FF00;
    r.stack[0] = 0x6A000100;       /* ddraw */
    r.stack[1] = 0x12345678;       /* nowhere */
    r.stack_count = 2;
    r.code_bytes[0] = 0x89;
    r.code_bytes[1] = 0x08;
    r.code_count = 2;
    r.mods = mods;
    r.mod_count = 4;
    r.exe_path = "C:\\Games\\LoM\\lomse.exe";
    r.exe_size = 1234567;
    r.exe_sha256 = NULL;
    r.ddraw_version = "cnc-ddraw 7.1.0.0 lom-hd-overlay@abc1234";
    r.host = "Wine 10.0";
    r.pack = "lomhd_portraits.pack: loaded";
    r.terrain = "off";
    r.seen = seen;
    r.seen_count = 1;
    r.log = &ring;
    r.log_lines = 30;

    LC_TEXT t;
    lc_text_init(&t, buf, sizeof(buf));
    lc_format_report(&r, &t);

    CHECK(t.dropped == 0, "the report fits its buffer");
    CHECK(has(buf, "Exception:  C0000005 ACCESS_VIOLATION"), "code and name");
    CHECK(has(buf, "Address:    Storm.dll+0x00001A2B (storm)"), "fault as module+offset with its kind");
    CHECK(has(buf, "Access:     write to 0x00000010  (a near-NULL pointer)"), "write access and target");
    CHECK(has(buf, "EIP = 15001A2B  Storm.dll+0x00001A2B"), "EIP annotated");
    CHECK(has(buf, "EAX = 00401234  lomse.exe+0x00001234"), "register pointing into the exe annotated");
    CHECK(has(buf, "ESP = 0019FF00\r\n"), "ESP never annotated");
    CHECK(has(buf, "0019FF00: 6A000100  ddraw.dll+0x00000100"), "stack value inside ddraw annotated");
    CHECK(has(buf, "0019FF04: 12345678\r\n"), "stack value outside every module left bare");
    CHECK(has(buf, "Code at EIP: 89 08"), "code bytes");
    CHECK(has(buf, "sha256 not computed yet"), "missing hash says so");
    CHECK(has(buf, "Time:       2026-09-27 14:30:05"), "time");
    CHECK(has(buf, "00400000-005FFFFF  lomse.exe"), "module list with ranges");
    CHECK(has(buf, "C0000005 ACCESS_VIOLATION at Storm.dll+0x00000010 reading 00000000, thread 7, 100 ms"),
        "first-chance breadcrumb");
    CHECK(has(buf, "  12:00:01 pack: 120 images loaded"), "log tail");
    CHECK(has(buf, "(owns the game window)"), "window thread");

    /* A hang report has no exception block and says why it was written. */
    r.kind = "hang";
    r.has_exception = FALSE;
    r.hang_ms = 21500;
    lc_text_init(&t, buf, sizeof(buf));
    lc_format_report(&r, &t);
    CHECK(has(buf, "hang report") && !has(buf, "Exception:"), "hang has no exception block");
    CHECK(has(buf, "No progress for 21 s"), "hang duration");

    /* A report for an exception with no parameters and no registers still formats. */
    LC_REPORT bare = { 0 };
    bare.kind = "crash";
    bare.has_exception = TRUE;
    bare.code = 0xC00000FD;
    lc_text_init(&t, buf, sizeof(buf));
    lc_format_report(&bare, &t);
    CHECK(has(buf, "C00000FD STACK_OVERFLOW") && has(buf, "Address:    0x00000000 (no module)") &&
        !has(buf, "Access:"), "bare report");
}

static void sha_of(const char* s, unsigned repeat, char out[65])
{
    LC_SHA256 h;
    lc_sha256_init(&h);

    for (unsigned i = 0; i < repeat; i++)
        lc_sha256_update(&h, (const BYTE*)s, (unsigned)strlen(s));

    lc_sha256_hex(&h, out);
}

static void test_sha256(void)
{
    char hex[65];
    sha_of("", 1, hex);
    CHECK(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0, "empty: %s", hex);
    sha_of("abc", 1, hex);
    CHECK(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0, "abc: %s", hex);
    /* 56 bytes: the length no longer fits the first block, so padding takes a second one. */
    sha_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 1, hex);
    CHECK(strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0, "448 bits: %s", hex);
    /* A million bytes fed in pieces, as the exe is hashed in 64 KB reads. */
    sha_of("aaaaaaaaaa", 100000, hex);
    CHECK(strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0, "million a: %s", hex);
}

int main(void)
{
    test_text();
    test_modules();
    test_ring();
    test_names();
    test_report();
    test_sha256();

    printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail != 0;
}
