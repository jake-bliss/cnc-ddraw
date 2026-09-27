#ifndef LOMHD_CRASH_H
#define LOMHD_CRASH_H

#include <windows.h>

/* The pure half of the crash and hang reporter (src/lomhd_crash_core.c): report text, module+offset
 * annotation, the lomhd.log ring buffer, report file names and SHA-256. No Windows calls, so it
 * builds natively for tests (tests/lomhd_crash_test.c) and runs on a crashing thread: no heap, no
 * locks, no C runtime formatting -- everything writes into caller-owned fixed buffers. */

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

    BOOL has_regs;
    LC_REGS regs;
    DWORD stack_addr;                   /* where stack[0] was read */
    DWORD stack[LC_STACK_DWORDS];
    int stack_count;                    /* dwords actually readable */
    BYTE code_bytes[LC_CODE_BYTES];
    int code_count;

    const LC_MODULE* mods;
    int mod_count;
    DWORD mods_age_ms;                  /* how old the module snapshot is */

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

#endif
