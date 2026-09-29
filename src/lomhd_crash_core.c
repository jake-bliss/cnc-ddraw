#include <stddef.h>
#include <windows.h>
#include "lomhd_crash.h"

/* The pure half of the crash reporter; see inc/lomhd_crash.h. It calls nothing: no heap, no C
 * runtime, no Windows. The chain functions run on a crashing thread; the rest on the report helper
 * thread, which may be working while the crashing thread holds the heap lock. */

void lc_text_init(LC_TEXT* t, char* buf, unsigned cap)
{
    t->buf = buf;
    t->cap = cap;
    t->len = 0;
    t->dropped = 0;

    if (cap)
        buf[0] = 0;
}

void lc_putn(LC_TEXT* t, const char* s, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
    {
        if (t->len + 1 < t->cap)
            t->buf[t->len++] = s[i];
        else
            t->dropped++;
    }

    if (t->cap)
        t->buf[t->len] = 0;
}

void lc_puts(LC_TEXT* t, const char* s)
{
    unsigned n = 0;

    if (!s)
        s = "(null)";

    while (s[n])
        n++;

    lc_putn(t, s, n);
}

void lc_hex(LC_TEXT* t, DWORD v, int digits)
{
    char out[8];

    if (digits < 1)
        digits = 1;

    if (digits > 8)
        digits = 8;

    for (int i = digits - 1; i >= 0; i--)
    {
        out[i] = "0123456789ABCDEF"[v & 15];
        v >>= 4;
    }

    lc_putn(t, out, (unsigned)digits);
}

void lc_dec(LC_TEXT* t, DWORD v)
{
    char out[10];
    int n = 0;

    do
    {
        out[9 - n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < 10);

    lc_putn(t, out + 10 - n, (unsigned)n);
}

void lc_nl(LC_TEXT* t)
{
    lc_putn(t, "\r\n", 2);
}

/* ------------------------------------------------------------------------------------------- */
/* Modules                                                                                     */
/* ------------------------------------------------------------------------------------------- */

void lc_sort_modules(LC_MODULE* mods, int count)
{
    /* Insertion sort in place: a few dozen modules, and no scratch buffer. */
    for (int i = 1; i < count; i++)
    {
        for (int j = i; j > 0 && mods[j - 1].base > mods[j].base; j--)
        {
            LC_MODULE tmp = mods[j];
            mods[j] = mods[j - 1];
            mods[j - 1] = tmp;
        }
    }
}

const LC_MODULE* lc_find_module(const LC_MODULE* mods, int count, DWORD addr)
{
    for (int i = 0; i < count; i++)
    {
        /* Subtraction, not base + size: a module ending at 4 GB would wrap the sum to 0. */
        if (addr >= mods[i].base && addr - mods[i].base < mods[i].size)
            return &mods[i];
    }

    return NULL;
}

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static BOOL name_is(const char* name, const char* want, BOOL prefix)
{
    int i = 0;

    for (; want[i]; i++)
    {
        if (lower(name[i]) != want[i])
            return FALSE;
    }

    return prefix || name[i] == 0;
}

const char* lc_module_kind(const char* name)
{
    if (!name)
        return "none";

    /* lomse.exe, and any renamed or patched copy (lomse_hybrid.exe) the setup might keep. */
    if (name_is(name, "lomse", TRUE))
        return "lomse";

    if (name_is(name, "storm.dll", FALSE))
        return "storm";

    if (name_is(name, "ddraw.dll", FALSE))
        return "ddraw";

    return "other";
}

void lc_addr(LC_TEXT* t, const LC_MODULE* mods, int count, DWORD addr)
{
    const LC_MODULE* m = lc_find_module(mods, count, addr);

    if (m)
    {
        lc_puts(t, m->name);
        lc_puts(t, "+0x");
        lc_hex(t, addr - m->base, 8);
    }
    else
    {
        lc_puts(t, "0x");
        lc_hex(t, addr, 8);
    }
}

const char* lc_exception_name(DWORD code)
{
    switch (code)
    {
    case 0xC0000005: return "ACCESS_VIOLATION";
    case 0xC0000006: return "IN_PAGE_ERROR";
    case 0xC000001D: return "ILLEGAL_INSTRUCTION";
    case 0xC0000025: return "NONCONTINUABLE_EXCEPTION";
    case 0xC000008C: return "ARRAY_BOUNDS_EXCEEDED";
    case 0xC000008D: return "FLT_DENORMAL_OPERAND";
    case 0xC000008E: return "FLT_DIVIDE_BY_ZERO";
    case 0xC000008F: return "FLT_INEXACT_RESULT";
    case 0xC0000090: return "FLT_INVALID_OPERATION";
    case 0xC0000091: return "FLT_OVERFLOW";
    case 0xC0000092: return "FLT_STACK_CHECK";
    case 0xC0000093: return "FLT_UNDERFLOW";
    case 0xC0000094: return "INTEGER_DIVIDE_BY_ZERO";
    case 0xC0000095: return "INTEGER_OVERFLOW";
    case 0xC0000096: return "PRIVILEGED_INSTRUCTION";
    case 0xC00000FD: return "STACK_OVERFLOW";
    case 0xC0000374: return "HEAP_CORRUPTION";
    case 0xC0000409: return "STACK_BUFFER_OVERRUN";
    case 0x80000001: return "GUARD_PAGE";
    case 0x80000002: return "DATATYPE_MISALIGNMENT";
    case 0x80000003: return "BREAKPOINT";
    case 0x80000004: return "SINGLE_STEP";
    case 0xE06D7363: return "C++ EXCEPTION";
    }

    return "UNKNOWN";
}

/* ------------------------------------------------------------------------------------------- */
/* Log ring                                                                                    */
/* ------------------------------------------------------------------------------------------- */

void lc_ring_push(LC_RING* r, const char* line)
{
    LONG seq = InterlockedIncrement(&r->next) - 1;
    char* out = r->line[(DWORD)seq % LC_RING_LINES];
    int n = 0;

    while (line[n] && n < LC_RING_WIDTH - 1)
    {
        out[n] = line[n];
        n++;
    }

    while (n > 0 && (out[n - 1] == '\r' || out[n - 1] == '\n'))
        n--;

    out[n] = 0;
}

int lc_ring_emit(const LC_RING* r, LC_TEXT* t, int max_lines)
{
    DWORD total = (DWORD)r->next;
    DWORD have = total < LC_RING_LINES ? total : LC_RING_LINES;
    DWORD want = max_lines < 0 ? 0 : (DWORD)max_lines;

    if (want > have)
        want = have;

    for (DWORD i = total - want; i != total; i++)
    {
        const char* line = r->line[i % LC_RING_LINES];
        unsigned n = 0;

        /* Bounded: a torn line may have lost its terminator. */
        while (n < LC_RING_WIDTH && line[n])
            n++;

        lc_puts(t, "  ");
        lc_putn(t, line, n);
        lc_nl(t);
    }

    return (int)want;
}

/* ------------------------------------------------------------------------------------------- */
/* Names                                                                                       */
/* ------------------------------------------------------------------------------------------- */

static void put_2(LC_TEXT* t, WORD v)
{
    char d[2] = { (char)('0' + v / 10 % 10), (char)('0' + v % 10) };
    lc_putn(t, d, 2);
}

void lc_report_name(char* out, unsigned cap, const char* kind, const WORD when[6], int attempt,
    const char* ext)
{
    LC_TEXT t;
    lc_text_init(&t, out, cap);
    lc_puts(&t, "lomhd_");
    lc_puts(&t, kind);
    lc_puts(&t, "_");
    put_2(&t, (WORD)(when[0] / 100));
    put_2(&t, (WORD)(when[0] % 100));
    put_2(&t, when[1]);
    put_2(&t, when[2]);
    lc_puts(&t, "_");
    put_2(&t, when[3]);
    put_2(&t, when[4]);
    put_2(&t, when[5]);

    if (attempt > 1)
    {
        lc_puts(&t, "_");
        lc_dec(&t, (DWORD)attempt);
    }

    lc_puts(&t, ext);
}

/* ------------------------------------------------------------------------------------------- */
/* The report                                                                                  */
/* ------------------------------------------------------------------------------------------- */

static void kv(LC_TEXT* t, const char* key, const char* value)
{
    lc_puts(t, key);
    lc_puts(t, value ? value : "unknown");
    lc_nl(t);
}

static void kind_of(LC_TEXT* t, const LC_REPORT* r, DWORD addr)
{
    const LC_MODULE* m = lc_find_module(r->mods, r->mod_count, addr);
    lc_puts(t, " (");
    lc_puts(t, m ? lc_module_kind(m->name) : "no module");
    lc_puts(t, ")");
}

static void reg(LC_TEXT* t, const LC_REPORT* r, const char* name, DWORD v, BOOL annotate)
{
    lc_puts(t, "  ");
    lc_puts(t, name);
    lc_puts(t, " = ");
    lc_hex(t, v, 8);

    if (annotate && lc_find_module(r->mods, r->mod_count, v))
    {
        lc_puts(t, "  ");
        lc_addr(t, r->mods, r->mod_count, v);
    }

    lc_nl(t);
}

static void put_access(LC_TEXT* t, DWORD code, DWORD nparams, const DWORD* params)
{
    if ((code != 0xC0000005 && code != 0xC0000006) || nparams < 2)
        return;

    lc_puts(t, "Access:     ");
    lc_puts(t, params[0] == 0 ? "read of 0x" : params[0] == 1 ? "write to 0x" :
        params[0] == 8 ? "execute (DEP) at 0x" : "unknown access to 0x");
    lc_hex(t, params[1], 8);

    if (params[1] < 0x10000)
        lc_puts(t, "  (a near-NULL pointer)");

    lc_nl(t);
}

void lc_format_report(const LC_REPORT* r, LC_TEXT* t)
{
    BOOL crash = r->kind && r->kind[0] == 'c';

    lc_puts(t, crash ? "Lords of Magic HD -- crash report" : "Lords of Magic HD -- hang report");
    lc_nl(t);
    lc_puts(t, crash ? "An exception the game did not handle. Nothing was changed; the game ended as it "
        "would have without this report." : "The game window stopped responding while in front. The "
        "game was not stopped or changed; this is a snapshot of where its window thread was.");

    if (!crash && r->wine)
        lc_puts(t, " On Wine a game window left in the background can look exactly like this; if the "
            "thread came back, an Outcome line at the end says so.");

    lc_nl(t);
    lc_nl(t);

    lc_puts(t, "Time:       ");
    lc_dec(t, r->when[0]);
    lc_puts(t, "-");
    put_2(t, r->when[1]);
    lc_puts(t, "-");
    put_2(t, r->when[2]);
    lc_puts(t, " ");
    put_2(t, r->when[3]);
    lc_puts(t, ":");
    put_2(t, r->when[4]);
    lc_puts(t, ":");
    put_2(t, r->when[5]);
    lc_puts(t, " local, ");
    lc_dec(t, r->uptime_ms / 1000);
    lc_puts(t, " s after ddraw.dll loaded");
    lc_nl(t);
    kv(t, "ddraw.dll:  ", r->ddraw_version);
    kv(t, "Host:       ", r->host);

    lc_puts(t, "Exe:        ");
    lc_puts(t, r->exe_path ? r->exe_path : "unknown");
    lc_nl(t);
    lc_puts(t, "            ");
    lc_dec(t, r->exe_size);
    lc_puts(t, " bytes, PE timestamp ");
    lc_hex(t, r->exe_timestamp, 8);
    lc_puts(t, ", checksum ");
    lc_hex(t, r->exe_checksum, 8);
    lc_nl(t);
    kv(t, "            sha256 ", r->exe_sha256 ? r->exe_sha256 : "not computed yet (crash within a second of start)");
    kv(t, "Pack:       ", r->pack);
    kv(t, "Terrain:    ", r->terrain);
    lc_nl(t);

    if (r->has_exception)
    {
        lc_puts(t, "Exception:  ");
        lc_hex(t, r->code, 8);
        lc_puts(t, " ");
        lc_puts(t, lc_exception_name(r->code));
        lc_nl(t);
        lc_puts(t, "Address:    ");
        lc_addr(t, r->mods, r->mod_count, r->address);
        kind_of(t, r, r->address);
        lc_nl(t);
        put_access(t, r->code, r->nparams, r->params);

        if (r->nparams)
        {
            lc_puts(t, "Parameters:");

            for (DWORD i = 0; i < r->nparams && i < 4; i++)
            {
                lc_puts(t, " ");
                lc_hex(t, r->params[i], 8);
            }

            lc_nl(t);
        }
    }

    if (!crash)
    {
        lc_puts(t, "No progress for ");
        lc_dec(t, r->hang_ms / 1000);
        lc_puts(t, " s: no DirectDraw calls from the window thread, and Windows/Wine call the "
            "window hung (it is not pumping messages).");
        lc_nl(t);
    }

    lc_puts(t, "Thread:     ");
    lc_dec(t, r->thread_id);
    lc_puts(t, r->window_thread ? " (owns the game window)" : " (not the game window's thread)");
    lc_nl(t);

    if (r->filter_note)
        kv(t, "Filters:    ", r->filter_note);

    lc_nl(t);

    if (r->has_regs)
    {
        const LC_REGS* g = &r->regs;
        lc_puts(t, "Registers");
        lc_nl(t);
        reg(t, r, "EIP", g->eip, TRUE);
        reg(t, r, "ESP", g->esp, FALSE);
        reg(t, r, "EBP", g->ebp, FALSE);
        reg(t, r, "EAX", g->eax, TRUE);
        reg(t, r, "EBX", g->ebx, TRUE);
        reg(t, r, "ECX", g->ecx, TRUE);
        reg(t, r, "EDX", g->edx, TRUE);
        reg(t, r, "ESI", g->esi, TRUE);
        reg(t, r, "EDI", g->edi, TRUE);
        reg(t, r, "EFL", g->eflags, FALSE);
        lc_puts(t, "  CS=");
        lc_hex(t, g->cs, 4);
        lc_puts(t, " DS=");
        lc_hex(t, g->ds, 4);
        lc_puts(t, " ES=");
        lc_hex(t, g->es, 4);
        lc_puts(t, " FS=");
        lc_hex(t, g->fs, 4);
        lc_puts(t, " GS=");
        lc_hex(t, g->gs, 4);
        lc_puts(t, " SS=");
        lc_hex(t, g->ss, 4);
        lc_nl(t);

        lc_puts(t, "  Code at EIP:");

        if (!r->code_count)
            lc_puts(t, " (unreadable)");

        for (int i = 0; i < r->code_count; i++)
        {
            lc_puts(t, " ");
            lc_hex(t, r->code_bytes[i], 2);
        }

        lc_nl(t);
        lc_nl(t);
    }

    lc_puts(t, "Stack from ");
    lc_hex(t, r->stack_addr, 8);
    lc_puts(t, " (");
    lc_dec(t, (DWORD)r->stack_count);
    lc_puts(t, " dwords; values inside a module are annotated -- likely return addresses)");
    lc_nl(t);

    for (int i = 0; i < r->stack_count; i++)
    {
        DWORD v = r->stack[i];
        lc_puts(t, "  ");
        lc_hex(t, r->stack_addr + (DWORD)i * 4, 8);
        lc_puts(t, ": ");
        lc_hex(t, v, 8);

        if (lc_find_module(r->mods, r->mod_count, v))
        {
            lc_puts(t, "  ");
            lc_addr(t, r->mods, r->mod_count, v);
        }

        lc_nl(t);
    }

    lc_nl(t);

    lc_puts(t, "Recent first-chance exceptions (these may have been handled; context only)");
    lc_nl(t);

    if (!r->seen_count)
    {
        lc_puts(t, "  none");
        lc_nl(t);
    }

    for (int i = 0; i < r->seen_count; i++)
    {
        const LC_SEEN* s = &r->seen[i];
        lc_puts(t, "  ");
        lc_hex(t, s->code, 8);
        lc_puts(t, " ");
        lc_puts(t, lc_exception_name(s->code));
        lc_puts(t, " at ");
        lc_addr(t, r->mods, r->mod_count, s->address);

        if (s->code == 0xC0000005)
        {
            lc_puts(t, s->access == 1 ? " writing " : s->access == 8 ? " executing " : " reading ");
            lc_hex(t, s->target, 8);
        }

        lc_puts(t, ", thread ");
        lc_dec(t, s->thread);
        lc_puts(t, ", ");
        lc_dec(t, r->uptime_ms >= s->tick ? (r->uptime_ms - s->tick) : 0);
        lc_puts(t, " ms before this report");
        lc_nl(t);
    }

    if (r->seen_dropped)
    {
        lc_puts(t, "  (");
        lc_dec(t, r->seen_dropped);
        lc_puts(t, " earlier ones not kept)");
        lc_nl(t);
    }

    lc_nl(t);

    lc_puts(t, "Modules (read from memory at report time)");
    lc_nl(t);

    for (int i = 0; i < r->mod_count; i++)
    {
        lc_puts(t, "  ");
        lc_hex(t, r->mods[i].base, 8);
        lc_puts(t, "-");
        lc_hex(t, r->mods[i].base + r->mods[i].size - 1, 8);
        lc_puts(t, "  ");
        lc_puts(t, r->mods[i].name);
        lc_nl(t);
    }

    lc_nl(t);

    lc_puts(t, "Last lines of lomhd.log");
    lc_nl(t);

    if (!r->log || !lc_ring_emit(r->log, t, r->log_lines))
    {
        lc_puts(t, "  (none this session)");
        lc_nl(t);
    }
}

/* ------------------------------------------------------------------------------------------- */
/* SHA-256 (FIPS 180-4)                                                                        */
/* ------------------------------------------------------------------------------------------- */

static const DWORD K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(LC_SHA256* s, const BYTE* p)
{
    DWORD w[64];

    for (int i = 0; i < 16; i++)
        w[i] = (DWORD)p[i * 4] << 24 | (DWORD)p[i * 4 + 1] << 16 | (DWORD)p[i * 4 + 2] << 8 | p[i * 4 + 3];

    for (int i = 16; i < 64; i++)
    {
        DWORD s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        DWORD s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    DWORD a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
    DWORD e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];

    for (int i = 0; i < 64; i++)
    {
        DWORD t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        DWORD t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    s->h[0] += a;
    s->h[1] += b;
    s->h[2] += c;
    s->h[3] += d;
    s->h[4] += e;
    s->h[5] += f;
    s->h[6] += g;
    s->h[7] += h;
}

void lc_sha256_init(LC_SHA256* s)
{
    static const DWORD init[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };

    for (int i = 0; i < 8; i++)
        s->h[i] = init[i];

    s->bits_hi = s->bits_lo = 0;
    s->fill = 0;
}

void lc_sha256_update(LC_SHA256* s, const BYTE* data, unsigned len)
{
    for (unsigned i = 0; i < len; i++)
    {
        s->block[s->fill++] = data[i];

        if (s->fill == 64)
        {
            sha_block(s, s->block);
            s->fill = 0;
        }
    }

    DWORD lo = s->bits_lo + (DWORD)len * 8;
    s->bits_hi += (DWORD)((unsigned long long)len * 8 >> 32) + (lo < s->bits_lo);
    s->bits_lo = lo;
}

void lc_sha256_hex(LC_SHA256* s, char out[65])
{
    DWORD hi = s->bits_hi, lo = s->bits_lo;
    BYTE pad = 0x80, zero = 0, len[8];

    lc_sha256_update(s, &pad, 1);

    while (s->fill != 56)
        lc_sha256_update(s, &zero, 1);

    for (int i = 0; i < 4; i++)
    {
        len[i] = (BYTE)(hi >> (24 - i * 8));
        len[4 + i] = (BYTE)(lo >> (24 - i * 8));
    }

    lc_sha256_update(s, len, 8);

    for (int i = 0; i < 32; i++)
    {
        BYTE b = (BYTE)(s->h[i / 4] >> (24 - (i % 4) * 8));
        out[i * 2] = "0123456789abcdef"[b >> 4];
        out[i * 2 + 1] = "0123456789abcdef"[b & 15];
    }

    out[64] = 0;
}

/* ------------------------------------------------------------------------------------------- */
/* The filter chain                                                                            */
/* ------------------------------------------------------------------------------------------- */

void lc_chain_init(LC_CHAIN* c, void* previous)
{
    for (int i = 0; i < LC_MAX_CHAIN; i++)
        c->f[i] = NULL;

    c->f[0] = previous;
    c->count = 1;
}

int lc_chain_level(void* const stubs[LC_MAX_CHAIN], const void* p)
{
    for (int i = 0; i < LC_MAX_CHAIN; i++)
        if (p && stubs[i] == p)
            return i;

    return -1;
}

void* lc_chain_set(LC_CHAIN* c, void* const stubs[LC_MAX_CHAIN], void* filter, int* install)
{
    void* old = c->f[c->count - 1];
    int level = lc_chain_level(stubs, filter);

    if (level >= 0)
    {
        /* Ours handed back: a restore to that level. */
        if (level < c->count - 1)
            c->count = level + 1;

        *install = c->count - 1;
    }
    else
    {
        c->f[c->count - 1] = filter;
        *install = -1;
    }

    return old;
}

int lc_chain_observe(LC_CHAIN* c, void* const stubs[LC_MAX_CHAIN], void* current, int* dropped)
{
    int level = lc_chain_level(stubs, current);
    *dropped = 0;

    if (level >= 0)
    {
        if (level < c->count - 1)
            c->count = level + 1;

        return c->count - 1;
    }

    for (int i = c->count - 1; i >= 0; i--)
    {
        if (c->f[i] == current)
        {
            c->count = i + 1;
            return i;
        }
    }

    if (c->count < LC_MAX_CHAIN)
    {
        c->f[c->count++] = current;
        return c->count - 1;
    }

    *dropped = 1;
    return c->count - 1;
}

void* lc_chain_next(const LC_CHAIN* c, void* const stubs[LC_MAX_CHAIN], int level)
{
    int count = c->count;

    if (count < 1)
        return NULL;

    if (level < 0)
        return NULL;

    if (level >= count)
        level = count - 1;

    void* f = c->f[level];
    return lc_chain_level(stubs, f) >= 0 ? NULL : f;
}

BOOL lc_chain_enter(DWORD* visited, int level)
{
    DWORD bit = (DWORD)1 << (level & 31);

    if (*visited & bit)
        return FALSE;

    *visited |= bit;
    return TRUE;
}

BOOL lc_same_fault(DWORD code, DWORD address, DWORD thread,
    DWORD last_code, DWORD last_address, DWORD last_thread)
{
    return last_code != 0 && code == last_code && address == last_address && thread == last_thread;
}

BOOL lc_in_front(const void* window, const void* foreground, const void* ghost_of_foreground, BOOL iconic)
{
    if (!window || iconic)
        return FALSE;

    return foreground == window || (ghost_of_foreground && ghost_of_foreground == window);
}

/* The line appended to a hang report when the window thread comes back. On Wine a thread that only
 * sat still while the player was in another app looks exactly like a hang: Wine keeps it the
 * foreground window, because the stalled thread is the one that would process the switch. */
/* Whether a recovered hang goes back to the per-session cap: only on Wine, and only max_refunds
 * times, so a game that really stalls and recovers again and again still stops writing reports. */
BOOL lc_hang_refund(BOOL wine, int count, int refunds, int max_refunds)
{
    return wine && count > 0 && refunds < max_refunds;
}

void lc_hang_outcome(LC_TEXT* t, DWORD stalled_ms, BOOL wine)
{
    lc_puts(t, "Outcome:    the window thread resumed after ");
    lc_dec(t, stalled_ms / 1000);
    lc_puts(t, " s");

    if (wine)
        lc_puts(t, " -- on Wine this is most likely the game window sitting in the background, not a hang");

    lc_puts(t, ".\r\n");
}

/* ------------------------------------------------------------------------------------------- */
/* The request mailbox                                                                         */
/* ------------------------------------------------------------------------------------------- */

static BOOL cas(volatile LONG* p, LONG from, LONG to)
{
    return InterlockedCompareExchange(p, to, from) == from;
}

int lc_mail_claim(LC_MAILBOX* m)
{
    for (int i = 0; i < LC_SLOTS; i++)
        if (cas(&m->state[i], LC_FREE, LC_FILLING))
            return i;

    return -1;
}

LONG lc_mail_post(LC_MAILBOX* m, int slot)
{
    LONG gen = InterlockedIncrement(&m->next_gen);
    InterlockedExchange(&m->gen[slot], gen);
    InterlockedExchange(&m->state[slot], LC_POSTED);
    return gen;
}

int lc_mail_take(LC_MAILBOX* m)
{
    for (;;)
    {
        int best = -1;

        for (int i = 0; i < LC_SLOTS; i++)
            if (m->state[i] == LC_POSTED && (best < 0 || m->gen[i] - m->gen[best] < 0))
                best = i;

        if (best < 0)
            return -1;

        /* Lost to a waiter cancelling it: look again. */
        if (cas(&m->state[best], LC_POSTED, LC_TAKEN))
            return best;
    }
}

void lc_mail_finish(LC_MAILBOX* m, int slot)
{
    if (!cas(&m->state[slot], LC_TAKEN, LC_DONE))
        cas(&m->state[slot], LC_ABANDONED, LC_FREE);
}

BOOL lc_mail_collect(LC_MAILBOX* m, int slot)
{
    return cas(&m->state[slot], LC_DONE, LC_FREE);
}

int lc_mail_abandon(LC_MAILBOX* m, int slot)
{
    if (cas(&m->state[slot], LC_POSTED, LC_FREE))
        return LC_CANCELLED;

    if (cas(&m->state[slot], LC_TAKEN, LC_ABANDONED))
        return LC_LEFT_RUNNING;

    /* Finished between the last look and now. */
    cas(&m->state[slot], LC_DONE, LC_FREE);
    return LC_WAS_DONE;
}

/* ------------------------------------------------------------------------------------------- */
/* PE bounds, deadlines                                                                        */
/* ------------------------------------------------------------------------------------------- */

BOOL lc_pe_nt_ok(DWORD e_lfanew, DWORD nt_size, DWORD readable)
{
    /* After the 64-byte DOS header, 4-aligned, and the NT headers wholly inside what was read. */
    return e_lfanew >= 64 && (e_lfanew & 3) == 0 && e_lfanew <= readable && nt_size <= readable - e_lfanew;
}

BOOL lc_pe_range_ok(DWORD rva, DWORD size, DWORD image_size)
{
    return rva != 0 && rva < image_size && size <= image_size - rva;
}

DWORD lc_pe_name_cap(DWORD rva, DWORD image_size, DWORD cap)
{
    if (rva == 0 || rva >= image_size)
        return 0;

    return image_size - rva < cap ? image_size - rva : cap;
}

DWORD lc_left(DWORD deadline, DWORD now)
{
    LONG left = (LONG)(deadline - now);
    return left > 0 ? (DWORD)left : 0;
}

/* ------------------------------------------------------------------------------------------- */
/* Locks and admission                                                                         */
/* ------------------------------------------------------------------------------------------- */

int lc_backoff(int attempt)
{
    return attempt < 4 ? LC_SPIN : attempt < 16 ? LC_YIELD : LC_SLEEP;
}

BOOL lc_lock_until(volatile LONG* lock, DWORD deadline, DWORD (*now)(void), void (*wait)(int kind))
{
    for (int attempt = 0;; attempt++)
    {
        if (InterlockedCompareExchange(lock, 1, 0) == 0)
            return TRUE;

        if (!lc_left(deadline, now()))
            return FALSE;

        wait(lc_backoff(attempt));
    }
}

BOOL lc_admit_check(const LC_ADMIT* a, DWORD code, DWORD address, DWORD thread, LONG max)
{
    return !lc_same_fault(code, address, thread, a->last_code, a->last_address, a->last_thread) &&
        a->reports < max;
}

void lc_admit_commit(LC_ADMIT* a, DWORD code, DWORD address, DWORD thread)
{
    a->last_code = code;
    a->last_address = address;
    a->last_thread = thread;
    InterlockedIncrement(&a->reports);
}
