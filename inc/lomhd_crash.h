#ifndef LOMHD_CRASH_H
#define LOMHD_CRASH_H

#include <windows.h>

/* The pure half of the crash and hang reporter (src/lomhd_crash_core.c): report text, module+offset
 * annotation, the lomhd.log ring buffer, report file names, SHA-256, and the bookkeeping for the
 * chain of top-level exception filters. No Windows calls, no heap, no C runtime: it builds natively
 * for tests (tests/lomhd_crash_test.c), and the report helper thread can run it while another
 * thread holds the heap lock -- everything writes into caller-owned fixed buffers. */

/* Text into a fixed buffer. Always NUL-terminated; what does not fit is dropped and counted. */
typedef struct
{
    char* buf;
    unsigned cap, len, dropped;
} LC_TEXT;

void lc_text_init(LC_TEXT* t, char* buf, unsigned cap);
void lc_puts(LC_TEXT* t, const char* s);
void lc_putn(LC_TEXT* t, const char* s, unsigned n);
void lc_hex(LC_TEXT* t, DWORD v, int digits);       /* upper case, zero padded, no prefix */
void lc_dec(LC_TEXT* t, DWORD v);
void lc_nl(LC_TEXT* t);                              /* CRLF: the reader is on Windows as often as not */

/* A loaded module. The name is the file name without its directory. */
#define LC_MODULE_NAME 48
typedef struct
{
    DWORD base, size;
    char name[LC_MODULE_NAME];
} LC_MODULE;

/* Sorts by base, so lc_find_module can stop early and the report lists them in address order. */
void lc_sort_modules(LC_MODULE* mods, int count);
const LC_MODULE* lc_find_module(const LC_MODULE* mods, int count, DWORD addr);

/* "lomse", "storm", "ddraw" or "other", from a module name; "none" for NULL. */
const char* lc_module_kind(const char* name);

/* "storm.dll+0x0001A2B3", or "0x12345678" outside every module. */
void lc_addr(LC_TEXT* t, const LC_MODULE* mods, int count, DWORD addr);

const char* lc_exception_name(DWORD code);

/* The last lines of lomhd.log, kept in memory so the crash path never reads a file. A push claims
 * its slot with an interlocked increment, so writers on different threads never share one; a reader
 * racing a writer may see one torn line, which is the price of taking no lock on a crashing thread. */
#define LC_RING_LINES 32
#define LC_RING_WIDTH 160
typedef struct
{
    volatile LONG next;                              /* lines ever pushed */
    char line[LC_RING_LINES][LC_RING_WIDTH];
} LC_RING;

void lc_ring_push(LC_RING* r, const char* line);    /* drops a trailing CR/LF; truncates */
int lc_ring_emit(const LC_RING* r, LC_TEXT* t, int max_lines);   /* oldest first; returns lines */

/* "lomhd_<kind>_YYYYMMDD_HHMMSS<.ext>", with "_2".."_9" before the extension for attempt > 1, so
 * two reports in one second do not overwrite each other. */
void lc_report_name(char* out, unsigned cap, const char* kind, const WORD when[6], int attempt,
    const char* ext);

/* A first-chance exception the vectored handler saw. It may well have been handled: these are
 * context for a report, never a report of their own. */
typedef struct
{
    DWORD code, address, access, target, thread, tick;
} LC_SEEN;

typedef struct
{
    DWORD eax, ebx, ecx, edx, esi, edi, ebp, esp, eip, eflags;
    DWORD cs, ds, es, fs, gs, ss;
} LC_REGS;

#define LC_STACK_DWORDS 64
#define LC_CODE_BYTES 16
#define LC_MAX_SEEN 4

typedef struct
{
    const char* kind;                   /* "crash" or "hang" */
    WORD when[6];                       /* local time: year, month, day, hour, minute, second */
    DWORD uptime_ms;

    BOOL has_exception;
    DWORD code, address, nparams, params[4];

    DWORD thread_id;
    BOOL window_thread;                 /* the thread that owns the game window */
    DWORD hang_ms;                      /* hang: how long the window thread made no progress */
    BOOL wine;                          /* hang: Wine cannot tell a background window from a hang */

    BOOL has_regs;
    LC_REGS regs;
    DWORD stack_addr;                   /* where stack[0] was read */
    DWORD stack[LC_STACK_DWORDS];
    int stack_count;                    /* dwords actually readable */
    BYTE code_bytes[LC_CODE_BYTES];
    int code_count;

    const LC_MODULE* mods;
    int mod_count;

    const char* exe_path;
    DWORD exe_size, exe_timestamp, exe_checksum;
    const char* exe_sha256;             /* NULL until the startup hash is done */
    const char* ddraw_version;
    const char* host;                   /* "Windows x.y" or "Wine 10.0" */
    const char* pack;                   /* lomhd_portraits.pack state */
    const char* terrain;

    const LC_SEEN* seen;
    int seen_count;
    DWORD seen_dropped;

    const LC_RING* log;
    int log_lines;

    const char* filter_note;            /* how the filter chain looked */
} LC_REPORT;

void lc_format_report(const LC_REPORT* r, LC_TEXT* t);

/* SHA-256, for the exe's identity -- hashed once at startup on a background thread, never at
 * crash time. */
typedef struct
{
    DWORD h[8];
    DWORD bits_hi, bits_lo;
    BYTE block[64];
    unsigned fill;
} LC_SHA256;

void lc_sha256_init(LC_SHA256* s);
void lc_sha256_update(LC_SHA256* s, const BYTE* data, unsigned len);
void lc_sha256_hex(LC_SHA256* s, char out[65]);     /* finishes the hash */

/* The chain of top-level exception filters (see src/lomhd_crash.c for why).
 *
 * f[0..count-1] are the other filters, oldest first; f[0] is whatever was installed before ours
 * (possibly NULL). Ours is installed as one of LC_MAX_CHAIN distinct entry points, one per level:
 * with `count` entries the process-wide filter is ours at level count-1. Each level has a
 * meaning -- "the chain as it was when f[level] was the newest" -- so:
 *   - a filter installed around the patched import saves "ours at level j" as its predecessor;
 *     when it chains back into that, we continue with f[j], one below it;
 *   - when something restores "ours at level j" (the filter above it uninstalling), every entry
 *     above j is gone, exactly as it would be natively.
 * `stubs` is the table of our entry points, as opaque pointers. None of these functions lock:
 * the caller serializes changes, and the crash path only reads. */
#define LC_MAX_CHAIN 8

typedef struct
{
    void* f[LC_MAX_CHAIN];
    int count;
} LC_CHAIN;

void lc_chain_init(LC_CHAIN* c, void* previous);

/* Which of our entry points `p` is, or -1. */
int lc_chain_level(void* const stubs[LC_MAX_CHAIN], const void* p);

/* SetUnhandledExceptionFilter through a patched import: `filter` becomes the newest entry and the
 * call returns what the real one would have (the entry it displaced). Handing back one of ours
 * (a module that got it from an unpatched call) is a restore. Returns the displaced filter;
 * *install is the level whose entry point must now be the process-wide filter, or -1 for "no
 * change". */
void* lc_chain_set(LC_CHAIN* c, void* const stubs[LC_MAX_CHAIN], void* filter, int* install);

/* The reporter found `current` installed process-wide. Returns the level whose entry point must
 * be installed now. Ours at the top level: nothing changed. Ours at a lower level: a restore, the
 * entries above it are dropped. A filter already in the chain at i: restored natively, entries
 * above i dropped -- never pushed twice. Anything else: a newcomer, pushed (ignored if the chain
 * is full; then *dropped is set). */
int lc_chain_observe(LC_CHAIN* c, void* const stubs[LC_MAX_CHAIN], void* current, int* dropped);

/* The filter ours at `level` hands on to: f[level], or the newest entry for a level the chain no
 * longer has. NULL for none (or one of ours, which must never be called back). */
void* lc_chain_next(const LC_CHAIN* c, void* const stubs[LC_MAX_CHAIN], int level);

/* Per crashing thread, the levels already entered: a filter chaining into a level it already
 * passed would loop. Returns FALSE (do not enter) the second time. */
BOOL lc_chain_enter(DWORD* visited, int level);

/* The same fault delivered again (Wine re-delivers one when winedbg attaches). */
BOOL lc_same_fault(DWORD code, DWORD address, DWORD thread,
    DWORD last_code, DWORD last_address, DWORD last_thread);

/* The game window counts as in front when it is the foreground window, or when the foreground
 * window is the ghost Windows puts up for it once it stops responding (ghost_of_foreground is what
 * HungWindowFromGhostWindow says the foreground window stands for; NULL if it is no ghost). */
BOOL lc_in_front(const void* window, const void* foreground, const void* ghost_of_foreground, BOOL iconic);
void lc_hang_outcome(LC_TEXT* t, DWORD stalled_ms, BOOL wine);
BOOL lc_hang_refund(BOOL wine, int count, int refunds, int max_refunds);

/* The handoff between crashing threads and the report helper: LC_SLOTS request slots, each with
 * its own copy of the exception (so the helper never reads a crashing thread's stack after that
 * thread has given up waiting) and its own generation number.
 *   waiter: claim (FREE -> FILLING), copy the exception in, post (-> POSTED, new generation),
 *           then either collect (DONE -> FREE) or, at its deadline, abandon:
 *           POSTED -> FREE (the helper never started: cancelled), TAKEN -> ABANDONED (the helper
 *           is working on it; it frees the slot when it finishes), DONE -> FREE.
 *   helper: take the oldest POSTED (-> TAKEN), work, finish (TAKEN -> DONE, ABANDONED -> FREE).
 * A slot the helper holds is never claimed again until it is finished, so a later request cannot
 * overwrite data the helper is still reading. All transitions are compare-and-swap. */
#define LC_SLOTS 2

enum { LC_FREE, LC_FILLING, LC_POSTED, LC_TAKEN, LC_ABANDONED, LC_DONE };
enum { LC_CANCELLED, LC_LEFT_RUNNING, LC_WAS_DONE };

typedef struct
{
    volatile LONG state[LC_SLOTS];
    volatile LONG gen[LC_SLOTS];
    volatile LONG next_gen;
} LC_MAILBOX;

int lc_mail_claim(LC_MAILBOX* m);                    /* a slot, or -1 when both are busy */
LONG lc_mail_post(LC_MAILBOX* m, int slot);          /* its generation */
int lc_mail_take(LC_MAILBOX* m);                     /* the oldest posted slot, or -1 */
void lc_mail_finish(LC_MAILBOX* m, int slot);
BOOL lc_mail_collect(LC_MAILBOX* m, int slot);       /* TRUE (and freed) once done */
int lc_mail_abandon(LC_MAILBOX* m, int slot);        /* LC_CANCELLED, LC_LEFT_RUNNING, LC_WAS_DONE */

/* PE header bounds, for reading a module's name out of memory. The headers are read from the first
 * `readable` bytes of the image; RVAs must lie inside the image. */
BOOL lc_pe_nt_ok(DWORD e_lfanew, DWORD nt_size, DWORD readable);
BOOL lc_pe_range_ok(DWORD rva, DWORD size, DWORD image_size);
DWORD lc_pe_name_cap(DWORD rva, DWORD image_size, DWORD cap);   /* bytes that may be read at rva */

/* Milliseconds left before `deadline` (GetTickCount time), 0 once passed; wrap-safe. */
DWORD lc_left(DWORD deadline, DWORD now);

/* A spinlock that cannot starve its owner: after a few spins it yields (SwitchToThread), then
 * sleeps (Sleep(1)), so a low-priority owner gets to run and release it. lc_backoff says which,
 * for the n-th failed attempt. lc_lock_until gives up at `deadline` (a crashing thread must reach
 * the game's filter on time); `now` and `wait` are the clock and the backoff, passed in so the
 * policy is testable without threads. */
enum { LC_SPIN, LC_YIELD, LC_SLEEP };
int lc_backoff(int attempt);
BOOL lc_lock_until(volatile LONG* lock, DWORD deadline, DWORD (*now)(void), void (*wait)(int kind));

/* Whether a crash gets a report: not the same fault as the last one reported, and under the cap.
 * Checked first; committed -- counted, and made the duplicate reference -- only once the report is
 * really asked for (a request slot claimed), so a crash that finds both slots busy costs nothing. */
typedef struct
{
    DWORD last_code, last_address, last_thread;
    volatile LONG reports;
} LC_ADMIT;

BOOL lc_admit_check(const LC_ADMIT* a, DWORD code, DWORD address, DWORD thread, LONG max);
void lc_admit_commit(LC_ADMIT* a, DWORD code, DWORD address, DWORD thread);

#endif
