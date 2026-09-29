/* The pure half of the crash reporter (src/lomhd_crash_core.c): text, module+offset annotation, the
 * log ring, report names, the report layout, SHA-256, and the filter-chain bookkeeping.
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
    CHECK(has(buf, "Modules (read from memory at report time)"), "module list heading");
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

/* Stand-ins: our eight entry points, and other filters, as distinct addresses. */
static char stub_mem[LC_MAX_CHAIN], other_mem[8];
static void* stubs[LC_MAX_CHAIN];
#define S(i) ((void*)&stub_mem[i])
#define F(i) ((void*)&other_mem[i])

static void test_chain(void)
{
    for (int i = 0; i < LC_MAX_CHAIN; i++)
        stubs[i] = S(i);

    LC_CHAIN c;
    int install, dropped;

    /* Installed over the CRT's filter F0: ours at level 0 hands on to F0. */
    lc_chain_init(&c, F(0));
    CHECK(c.count == 1 && lc_chain_next(&c, stubs, 0) == F(0), "init");
    CHECK(lc_chain_level(stubs, S(3)) == 3 && lc_chain_level(stubs, F(0)) == -1 &&
        lc_chain_level(stubs, NULL) == -1, "level lookup");

    /* The game sets F1 through the patched import: it gets F0 back, as natively, and ours stays. */
    void* old = lc_chain_set(&c, stubs, F(1), &install);
    CHECK(old == F(0) && install == -1 && c.count == 1 && c.f[0] == F(1), "patched set replaces the newest");

    /* Something bypasses the patch with F2 (its predecessor: ours at level 0). Observed: pushed,
     * and ours goes back on top at level 1, which hands on to F2. */
    CHECK(lc_chain_observe(&c, stubs, F(2), &dropped) == 1 && c.count == 2 && !dropped, "newcomer pushed");
    CHECK(lc_chain_next(&c, stubs, 1) == F(2), "top level hands on to the newcomer");
    /* F2 chains to its predecessor, ours at level 0: that continues with F1, not F2 again. */
    CHECK(lc_chain_next(&c, stubs, 0) == F(1), "level 0 hands on to the game's filter");

    /* Nothing changed since: ours at the top level is what the reporter finds. */
    CHECK(lc_chain_observe(&c, stubs, S(1), &dropped) == 1 && c.count == 2, "unchanged");

    /* F2 uninstalls: it restores what it saved, ours at level 0. F2 must leave the chain. */
    CHECK(lc_chain_observe(&c, stubs, S(0), &dropped) == 0 && c.count == 1, "restore to our lower level drops F2");
    CHECK(lc_chain_next(&c, stubs, 0) == F(1), "and the game's filter is next again");

    /* Bypass again with F2, then someone restores F1 itself (already in the chain): no duplicate,
     * everything above F1 is gone. */
    lc_chain_observe(&c, stubs, F(2), &dropped);
    CHECK(lc_chain_observe(&c, stubs, F(1), &dropped) == 0 && c.count == 1 && c.f[0] == F(1),
        "a filter already in the chain is never pushed twice");

    /* With two levels, a patched call replaces the newest entry (natively, it goes on top of it and
     * chains to it itself), not the oldest. */
    lc_chain_observe(&c, stubs, F(2), &dropped);
    old = lc_chain_set(&c, stubs, F(3), &install);
    CHECK(old == F(2) && c.count == 2 && c.f[1] == F(3) && c.f[0] == F(1), "patched set with two levels");
    lc_chain_observe(&c, stubs, S(0), &dropped);

    /* A patched call handing back ours at level 0 while the chain has two levels: a restore. */
    lc_chain_observe(&c, stubs, F(2), &dropped);
    old = lc_chain_set(&c, stubs, S(0), &install);
    CHECK(old == F(2) && install == 0 && c.count == 1, "patched hand-back of ours restores that level");

    /* NULL installed around the patch: natively "no filter", so nothing below it runs. */
    lc_chain_init(&c, F(0));
    CHECK(lc_chain_observe(&c, stubs, NULL, &dropped) == 1 && c.count == 2 && lc_chain_next(&c, stubs, 1) == NULL,
        "NULL on top means nothing to hand on to");

    /* Full: a ninth is ignored rather than overwriting (which would loop through our levels). */
    lc_chain_init(&c, F(0));
    for (int i = 1; i < LC_MAX_CHAIN; i++)
        lc_chain_observe(&c, stubs, (void*)&other_mem[i], &dropped);
    int top = lc_chain_observe(&c, stubs, (void*)((char*)F(7) + 1), &dropped);
    CHECK(c.count == LC_MAX_CHAIN && top == LC_MAX_CHAIN - 1 && dropped && c.f[LC_MAX_CHAIN - 1] == F(7),
        "full chain ignores a newcomer, keeping its newest entry");

    /* A level the chain no longer has (a stale entry point) hands on to the newest entry. */
    lc_chain_init(&c, F(0));
    CHECK(lc_chain_next(&c, stubs, 5) == F(0) && lc_chain_next(&c, stubs, -1) == NULL, "stale level");

    /* One of ours never ends up called back as a foreign filter. */
    c.f[0] = S(2);
    CHECK(lc_chain_next(&c, stubs, 0) == NULL, "ours is never next");

    DWORD visited = 0;
    CHECK(lc_chain_enter(&visited, 1) && lc_chain_enter(&visited, 0) && !lc_chain_enter(&visited, 1),
        "a level entered twice on one thread is refused");

    CHECK(lc_same_fault(0xC0000005, 0x401000, 7, 0xC0000005, 0x401000, 7), "same fault");
    CHECK(!lc_same_fault(0xC0000005, 0x401000, 8, 0xC0000005, 0x401000, 7), "other thread is new");
    CHECK(!lc_same_fault(0xC0000005, 0x401004, 7, 0xC0000005, 0x401000, 7), "other address is new");
    CHECK(!lc_same_fault(0, 0, 0, 0, 0, 0), "nothing reported yet");

    int w = 0, fg = 0, ghost = 0;
    CHECK(lc_in_front(&w, &w, NULL, FALSE), "foreground");
    CHECK(lc_in_front(&w, &ghost, &w, FALSE), "our ghost is in front: still in front");
    CHECK(!lc_in_front(&w, &fg, NULL, FALSE), "another window");
    CHECK(!lc_in_front(&w, &ghost, &fg, FALSE), "another window's ghost");
    CHECK(!lc_in_front(&w, &w, NULL, TRUE), "minimized");
    CHECK(!lc_in_front(NULL, NULL, NULL, FALSE), "no window");
}

static void test_hang_outcome(void)
{
    char buf[256];
    LC_TEXT t;

    lc_text_init(&t, buf, sizeof(buf));
    lc_hang_outcome(&t, 45999, FALSE);
    CHECK(strcmp(buf, "Outcome:    the window thread resumed after 45 s.\r\n") == 0, "hang outcome, Windows");

    lc_text_init(&t, buf, sizeof(buf));
    lc_hang_outcome(&t, 21000, TRUE);
    CHECK(strstr(buf, "resumed after 21 s -- on Wine") && strstr(buf, "background, not a hang.\r\n"),
        "hang outcome, Wine");

    /* The cap: 3 reports, and on Wine 6 of them given back -- so at most 9, however often it recovers. */
    int count = 0, refunds = 0, written = 0;

    for (int i = 0; i < 50; i++)
    {
        if (count >= 3)
            continue;

        count++;
        written++;

        if (lc_hang_refund(TRUE, count, refunds, 6))
        {
            count--;
            refunds++;
        }
    }

    CHECK(written == 9 && refunds == 6, "Wine: 50 recovered hangs write 9 reports");
    CHECK(!lc_hang_refund(FALSE, 1, 0, 6), "Windows: never refunded");
    CHECK(!lc_hang_refund(TRUE, 0, 0, 6), "nothing counted, nothing to refund");

    static char big[8192];
    LC_REPORT r;
    memset(&r, 0, sizeof(r));
    r.kind = "hang";
    lc_text_init(&t, big, sizeof(big));
    lc_format_report(&r, &t);
    CHECK(!strstr(big, "On Wine"), "hang lead, Windows: no Wine note");
    r.wine = TRUE;
    lc_text_init(&t, big, sizeof(big));
    lc_format_report(&r, &t);
    CHECK(strstr(big, "while in front. The game was not stopped") && strstr(big, "On Wine a game window"),
        "hang lead, Wine: the note follows the lead");
}

static void test_mailbox(void)
{
    LC_MAILBOX m = { { 0 }, { 0 }, 0 };

    /* The ordinary round trip. */
    int a = lc_mail_claim(&m);
    LONG g1 = lc_mail_post(&m, a);
    CHECK(a >= 0 && g1 == 1 && lc_mail_take(&m) == a, "post and take");
    CHECK(!lc_mail_collect(&m, a), "not collectable while the helper works");
    lc_mail_finish(&m, a);
    CHECK(lc_mail_collect(&m, a) && m.state[a] == LC_FREE, "collected, and the slot is free again");

    /* Timed out before the helper started: cancelled, and the helper never sees it. */
    a = lc_mail_claim(&m);
    lc_mail_post(&m, a);
    CHECK(lc_mail_abandon(&m, a) == LC_CANCELLED && lc_mail_take(&m) == -1, "cancelled before it started");

    /* Timed out while the helper works (it is stuck on this thread's lock, say). The slot stays
     * the helper's until it finishes: a second crash must get the other slot, never this one. */
    a = lc_mail_claim(&m);
    LONG ga = lc_mail_post(&m, a);
    CHECK(lc_mail_take(&m) == a, "helper takes A");
    CHECK(lc_mail_abandon(&m, a) == LC_LEFT_RUNNING && m.state[a] == LC_ABANDONED, "A abandoned mid-report");
    int b = lc_mail_claim(&m);
    LONG gb = lc_mail_post(&m, b);
    CHECK(b >= 0 && b != a && gb > ga, "B gets the other slot and a later generation");
    CHECK(lc_mail_claim(&m) == -1, "both busy: a third crash gets no slot (and skips its report)");

    /* The helper finishes A late: A's slot is freed, and B is not marked done by it. */
    lc_mail_finish(&m, a);
    CHECK(m.state[a] == LC_FREE && !lc_mail_collect(&m, b), "A's late finish frees A only");
    CHECK(lc_mail_take(&m) == b, "then the helper takes B");
    lc_mail_finish(&m, b);
    CHECK(lc_mail_collect(&m, b), "and B completes");

    /* Finished just before the deadline check: counts as done, slot freed. */
    a = lc_mail_claim(&m);
    lc_mail_post(&m, a);
    lc_mail_take(&m);
    lc_mail_finish(&m, a);
    CHECK(lc_mail_abandon(&m, a) == LC_WAS_DONE && m.state[a] == LC_FREE, "done at the deadline");

    /* Two posted: the older generation is taken first. */
    a = lc_mail_claim(&m);
    b = lc_mail_claim(&m);
    lc_mail_post(&m, b);
    lc_mail_post(&m, a);
    CHECK(lc_mail_take(&m) == b, "oldest first");

    CHECK(lc_left(1000, 400) == 600 && lc_left(1000, 1000) == 0 && lc_left(1000, 5000) == 0, "left");
    CHECK(lc_left(5, 0xFFFFFFF0u) == 21, "left across the tick counter wrapping");
}

static void test_pe_bounds(void)
{
    CHECK(lc_pe_nt_ok(0x80, 248, 4096), "ordinary e_lfanew");
    CHECK(!lc_pe_nt_ok(0x10, 248, 4096), "inside the DOS header");
    CHECK(!lc_pe_nt_ok(0x82, 248, 4096), "misaligned");
    CHECK(!lc_pe_nt_ok(4000, 248, 4096), "NT headers run past what was read");
    CHECK(!lc_pe_nt_ok(0x7FFFFFFC, 248, 4096), "huge e_lfanew");
    CHECK(lc_pe_nt_ok(4096 - 248, 248, 4096), "NT headers ending exactly at the edge");

    CHECK(lc_pe_range_ok(0x1000, 40, 0x2000), "export directory inside the image");
    CHECK(!lc_pe_range_ok(0, 40, 0x2000), "no directory");
    CHECK(!lc_pe_range_ok(0x1FF0, 40, 0x2000), "directory running off the end");
    CHECK(!lc_pe_range_ok(0xFFFFFFF0u, 40, 0x2000), "RVA that would wrap");

    CHECK(lc_pe_name_cap(0x1000, 0x2000, 47) == 47, "name, full cap");
    CHECK(lc_pe_name_cap(0x1FF8, 0x2000, 47) == 8, "name near the end: only what is inside");
    CHECK(lc_pe_name_cap(0x2000, 0x2000, 47) == 0 && lc_pe_name_cap(0, 0x2000, 47) == 0, "name outside");
}

static DWORD fake_clock;
static int waits[3];

static DWORD fake_now(void)
{
    return fake_clock;
}

static void fake_wait(int kind)
{
    waits[kind]++;
    fake_clock += kind == LC_SLEEP ? 1 : 0;
}

static void test_locks(void)
{
    CHECK(lc_backoff(0) == LC_SPIN && lc_backoff(3) == LC_SPIN, "spins first");
    CHECK(lc_backoff(4) == LC_YIELD && lc_backoff(15) == LC_YIELD, "then yields to the owner");
    CHECK(lc_backoff(16) == LC_SLEEP && lc_backoff(100000) == LC_SLEEP, "then sleeps: never spins forever");

    volatile LONG lock = 0;
    fake_clock = 1000;
    CHECK(lc_lock_until(&lock, 2000, fake_now, fake_wait) && lock == 1, "free lock taken at once");

    /* Held and never released (the owner starved or dead): the crash path gives up at its
     * deadline, having yielded and slept rather than spun. */
    waits[0] = waits[1] = waits[2] = 0;
    CHECK(!lc_lock_until(&lock, 1050, fake_now, fake_wait) && fake_clock == 1050, "gives up at the deadline");
    CHECK(waits[LC_SPIN] == 4 && waits[LC_YIELD] == 12 && waits[LC_SLEEP] == 50, "backoff: %d spins, %d yields, %d sleeps",
        waits[0], waits[1], waits[2]);

    LC_ADMIT a = { 0, 0, 0, 0 };
    CHECK(lc_admit_check(&a, 0xC0000005, 0x401000, 7, 4), "first crash admitted");

    /* Checked but no slot claimed: nothing counted, and it is not a duplicate next time. */
    CHECK(a.reports == 0 && lc_admit_check(&a, 0xC0000005, 0x401000, 7, 4), "a check alone commits nothing");

    lc_admit_commit(&a, 0xC0000005, 0x401000, 7);
    CHECK(a.reports == 1 && !lc_admit_check(&a, 0xC0000005, 0x401000, 7, 4), "committed: counted and deduplicated");
    CHECK(lc_admit_check(&a, 0xC0000005, 0x401000, 8, 4), "another thread's fault is new");

    a.reports = 4;
    CHECK(!lc_admit_check(&a, 0xC0000094, 0x402000, 9, 4), "cap reached");
}

int main(void)
{
    test_text();
    test_modules();
    test_ring();
    test_names();
    test_report();
    test_sha256();
    test_chain();
    test_mailbox();
    test_pe_bounds();
    test_locks();
    test_hang_outcome();

    printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail != 0;
}
