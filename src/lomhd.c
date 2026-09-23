#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dd.h"
#include "ddsurface.h"
#include "opengl_utils.h"
#include "lomhd.h"
#include "lomhd_match.h"

/* Lords of Magic HD overlay.
 *
 * The game draws at 640x480 in 16 bpp, exactly as it always has. This finds known pictures --
 * portraits, items, buildings, full screens -- in the finished frame and draws their upscaled
 * versions over them after cnc-ddraw has scaled the frame to the window. Nothing in lomse.exe is
 * hooked: a picture is recognised by its pixels, which the engine copies into the frame as exact
 * RGB565 values -- Observed 2026-09-22 for portraits (100% of pixels) and 2026-09-23 for screens
 * (loading 99.7%, start 99.5%, a keep 94.6%). So detection covers every place a picture appears.
 *
 * THE ONE RULE: the render thread never touches a file. It holds g_ddraw.cs whenever it calls in
 * here, and doing file I/O there wedged the render thread under Wine in an early build. The render
 * thread scans, masks and draws; one worker thread owns every file operation.
 *
 * Upscales are loaded lazily. With full screens the pack is ~900 MB and the game is a 32-bit
 * process, so the worker reads only the index and matching data at start; the first time a
 * picture is found, the render thread asks for its upscale (and, for a large picture, its full
 * indices), the worker loads them, and the picture is drawn from then on. Until then the original
 * shows -- for a frame or two. Each upscale is uploaded to the GPU once and kept, within a budget,
 * least recently drawn first out. */

static HANDLE g_worker_wake;
static volatile LONG g_frames, g_want_frame, g_frame_ready, g_pack_ready;
static volatile LONG g_seen_seq, g_loaded_seq;

/* Debug mode: a file named `lomhd_debug` beside lomse.exe when the game starts. Players get three
 * kinds of log line -- the pack loaded, the overlay turned itself off, or an error. Debug adds the
 * detection record, a five-second watchdog and the `lomhd_dump` frame trigger: the instruments
 * that found every problem in this file, kept for the next platform it has to be proven on. */
static volatile LONG g_debug;
static volatile const char* g_renderer;
static volatile const char* g_gl_missing;   /* set by the render thread, logged by the worker */

static BYTE* g_frame;
static DWORD g_frame_w, g_frame_h, g_frame_bpp;

static HANDLE g_pack_file;                  /* worker only */
static LOMHD_PACK g_pack;

/* Per picture, what has been loaded. The state is the handover between the two threads:
 *   NONE -> REQUESTED   render thread, when the picture is found and not loaded
 *   REQUESTED -> READY  worker, after filling rgb (and idx for a large picture); or -> FAILED
 *   READY -> NONE       render thread, when it evicts the picture (freeing what it holds)
 * The worker writes rgb/idx only while REQUESTED; from READY on they belong to the render thread,
 * which uploads rgb to a texture once and frees it. InterlockedExchange orders the writes. */
enum { IMG_NONE, IMG_REQUESTED, IMG_READY, IMG_FAILED };

typedef struct
{
    volatile LONG state;
    BYTE* rgb;                              /* hw*hh*3, until uploaded */
    BYTE* idx;                              /* w*h, large pictures only (small ones keep theirs) */
    GLuint tex;                             /* the upscale, in g_gl_context */
    DWORD last_used;                        /* scan counter, for eviction */
} LOMHD_IMG;

static LOMHD_IMG* g_img;

/* The GPU budget for upscales. A 1280x960 screen is ~4.9 MB as the driver likely stores it
 * (RGBA8); 160 MB keeps ~30 screens plus every portrait in view. */
#define LOMHD_TEX_BUDGET (160u << 20)
static size_t g_tex_bytes;
static DWORD g_scan_count;
static BOOL g_rescan;
#define LOMHD_STALE_SCANS 60                       /* render thread: scan next frame even if unchanged */

/* What lomhd_draw draws, built by lomhd_scan while g_ddraw.cs is held. The draw runs after the
 * renderer has RELEASED that lock, and cnc-ddraw frees the primary's buffer when the game releases
 * the surface -- so the draw must never read the game's frame or surface. It reads only these.
 * (Found by cross-model review, 2026-09-22: the first version built them inside the draw.) Each
 * slot has its own mask: two copies of one picture can be covered differently. */
static BYTE* g_slot_mask[LOMHD_MAX_PLACEMENTS];   /* w*h: 255 where the frame still shows it */
static size_t g_slot_mask_cap[LOMHD_MAX_PLACEMENTS];
static int g_slot_img[LOMHD_MAX_PLACEMENTS];
static float g_quad[LOMHD_MAX_PLACEMENTS][4];      /* x0, y0, x1, y1 in NDC */
static int g_slot_count;
static PLACEMENT g_placements[LOMHD_MAX_PLACEMENTS];
static int g_placement_count;
static PLACEMENT g_seen[LOMHD_MAX_PLACEMENTS];   /* snapshot for the worker to log */
static int g_seen_count;


/* ------------------------------------------------------------------------------------------- */
/* Files (worker thread only)                                                                  */
/* ------------------------------------------------------------------------------------------- */

static void lomhd_path(char* out, size_t size, const char* name)
{
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, sizeof(exe));
    char* slash = NULL;

    for (DWORD i = 0; i < n && i < sizeof(exe); i++)
        if (exe[i] == '\\' || exe[i] == '/')
            slash = &exe[i];

    if (slash)
        *slash = 0;
    else
        exe[0] = 0;

    _snprintf(out, size, "%s\\%s", exe, name);
    out[size - 1] = 0;
}

static void lomhd_log(const char* line)
{
    char path[MAX_PATH];
    lomhd_path(path, sizeof(path), "lomhd.log");

    HANDLE f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
        OPEN_ALWAYS, 0, NULL);

    if (f == INVALID_HANDLE_VALUE)
        return;

    SYSTEMTIME t;
    GetLocalTime(&t);

    char stamped[256];
    int len = _snprintf(stamped, sizeof(stamped), "%02d:%02d:%02d %s\r\n",
        t.wHour, t.wMinute, t.wSecond, line);

    if (len < 0 || len >= (int)sizeof(stamped))
        len = (int)sizeof(stamped) - 1;

    DWORD written;
    WriteFile(f, stamped, (DWORD)len, &written, NULL);
    CloseHandle(f);
}

static void lomhd_logf(const char* fmt, long a, long b, long c)
{
    char line[200];
    _snprintf(line, sizeof(line), fmt, a, b, c);
    line[sizeof(line) - 1] = 0;
    lomhd_log(line);
}

static void lomhd_write_frame(void)
{
    SYSTEMTIME t;
    GetLocalTime(&t);

    char name[64];
    _snprintf(name, sizeof(name), "lomhd_frame_%02d%02d%02d_%03d.raw",
        t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    name[sizeof(name) - 1] = 0;

    char path[MAX_PATH];
    lomhd_path(path, sizeof(path), name);

    HANDLE f = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);

    if (f == INVALID_HANDLE_VALUE)
    {
        lomhd_log("frame: could not create the dump file");
        return;
    }

    DWORD header[3] = { g_frame_w, g_frame_h, g_frame_bpp };
    DWORD written;

    WriteFile(f, "LOMHDRAW", 8, &written, NULL);
    WriteFile(f, header, sizeof(header), &written, NULL);
    WriteFile(f, g_frame, g_frame_w * g_frame_h * (g_frame_bpp / 8), &written, NULL);
    CloseHandle(f);

    char line[160];
    _snprintf(line, sizeof(line), "frame: wrote %s", name);
    line[sizeof(line) - 1] = 0;
    lomhd_log(line);
}

/* ------------------------------------------------------------------------------------------- */
/* The pack (worker thread)                                                                    */
/* ------------------------------------------------------------------------------------------- */

static BOOL pack_read(void* ctx, DWORD off, DWORD len, BYTE* out)
{
    HANDLE f = ctx;
    DWORD got = 0;
    return SetFilePointer(f, (LONG)off, NULL, FILE_BEGIN) != INVALID_SET_FILE_POINTER &&
        ReadFile(f, out, len, &got, NULL) && got == len;
}

static BOOL lomhd_load_pack(void)
{
    char path[MAX_PATH];
    lomhd_path(path, sizeof(path), "lomhd_portraits.pack");

    g_pack_file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);

    if (g_pack_file == INVALID_HANDLE_VALUE)
    {
        g_pack_file = NULL;
        lomhd_log("pack: lomhd_portraits.pack not found -- overlay off, game unchanged");
        return FALSE;
    }

    DWORD size = GetFileSize(g_pack_file, NULL), bad = 0;
    char head[8] = { 0 };
    DWORD got = 0;
    ReadFile(g_pack_file, head, 8, &got, NULL);

    /* Any other LOMHDPK version is one this build does not read. The version was once spelled out
     * here as well as in the reader, and the format-4 reader shipped behind a format-3 check that
     * refused every pack it could read (Claude review, 2026-09-23). */
    if (got == 8 && memcmp(head, LOMHD_PACK_MAGIC, 7) == 0 && head[7] != LOMHD_PACK_MAGIC[7])
    {
        lomhd_log("pack: made by an older setup -- run lomhd_setup.py again. Overlay off");
        return FALSE;
    }

    DWORD start = GetTickCount();

    if (!lomhd_pack_open(pack_read, g_pack_file, size, &g_pack, &bad))
    {
        lomhd_logf("pack: unreadable or corrupt near byte %ld of %ld -- overlay off",
            (long)bad, (long)size, 0);
        return FALSE;
    }

    g_img = calloc(g_pack.count, sizeof(LOMHD_IMG));

    if (!g_img)
    {
        lomhd_log("pack: out of memory -- overlay off");
        return FALSE;
    }

    /* The probe width doubles as a build check: a stale object linked against an older
     * LOMHD_PACK layout once printed a pointer here instead of the width (2026-09-22). */
    lomhd_logf("pack: %ld images loaded, format 4, probe width %ld, %ld ms",
        g_pack.count, g_pack.probe_width, (long)(GetTickCount() - start));
    lomhd_logf("pack: %ld of them sprites, sprite probe width %ld", g_pack.sprites,
        LOMHD_SPRITE_PROBE_W, 0);
    return TRUE;
}

/* Fulfil every outstanding request. A picture whose stream will not load is switched off for the
 * session and logged; the rest of the pack keeps working. */
static void lomhd_serve_requests(void)
{
    for (int p = 0; p < g_pack.count; p++)
    {
        if (g_img[p].state != IMG_REQUESTED)
            continue;

        const PORTRAIT* r = &g_pack.portraits[p];
        BYTE* rgb = lomhd_load_upscale(&g_pack, p, pack_read, g_pack_file);
        BYTE* idx = r->idx ? NULL : lomhd_load_indices(&g_pack, p, pack_read, g_pack_file);

        if (!rgb || (!r->idx && !idx))
        {
            free(rgb);
            free(idx);
            InterlockedExchange(&g_img[p].state, IMG_FAILED);

            char line[160];
            _snprintf(line, sizeof(line), "pack: %s would not load -- that picture stays vanilla", r->name);
            line[sizeof(line) - 1] = 0;
            lomhd_log(line);
            continue;
        }

        g_img[p].rgb = rgb;
        g_img[p].idx = idx;
        InterlockedExchange(&g_img[p].state, IMG_READY);
        InterlockedIncrement(&g_loaded_seq);
    }
}

BOOL lomhd_wants_opengl(void)
{
    /* Startup, not the render thread: a single attribute query is the whole cost. Found 2026-09-22:
     * the Steam install on Windows has no ddraw.ini, so renderer=auto chose Direct3D 9 there. */
    char path[MAX_PATH];
    lomhd_path(path, sizeof(path), "lomhd_portraits.pack");
    DWORD attributes = GetFileAttributesA(path);

    /* A directory by that name is not a pack. (Codex review.) */
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

/* ------------------------------------------------------------------------------------------- */
/* Worker                                                                                      */
/* ------------------------------------------------------------------------------------------- */

static DWORD WINAPI lomhd_worker(LPVOID unused)
{
    (void)unused;

    char debug_flag[MAX_PATH];
    lomhd_path(debug_flag, sizeof(debug_flag), "lomhd_debug");

    if (GetFileAttributesA(debug_flag) != INVALID_FILE_ATTRIBUTES)
    {
        InterlockedExchange(&g_debug, 1);
        lomhd_log("debug: on (lomhd_debug present)");
    }

    if (lomhd_load_pack())
        InterlockedExchange(&g_pack_ready, 1);

    DWORD last_report = GetTickCount();
    LONG logged_seq = 0;
    const char* reported_renderer = NULL;
    char trigger[MAX_PATH];
    lomhd_path(trigger, sizeof(trigger), "lomhd_dump");

    for (;;)
    {
        WaitForSingleObject(g_worker_wake, 250);

        if (g_pack_ready)
            lomhd_serve_requests();

        const char* renderer = (const char*)g_renderer;

        if (renderer != reported_renderer && (g_debug || reported_renderer))
        {
            char line[160];
            _snprintf(line, sizeof(line), "renderer: %s%s", renderer ? renderer : "?",
                reported_renderer ? " (CHANGED)" : "");
            line[sizeof(line) - 1] = 0;
            lomhd_log(line);
        }

        reported_renderer = renderer;

        if (InterlockedCompareExchange(&g_frame_ready, 0, 1) == 1)
            lomhd_write_frame();

        static BOOL reported_gl;
        const char* missing = (const char*)g_gl_missing;

        if (missing && !reported_gl)
        {
            char line[200];
            _snprintf(line, sizeof(line), "overlay OFF: missing %s -- pictures stay vanilla", missing);
            line[sizeof(line) - 1] = 0;
            lomhd_log(line);
            reported_gl = TRUE;
        }

        /* What the matcher sees, logged only when it changes: this is the acceptance record. */
        LONG seq = g_seen_seq;

        if (seq != logged_seq && g_debug)
        {
            PLACEMENT seen[LOMHD_MAX_PLACEMENTS];
            int n;

            EnterCriticalSection(&g_ddraw.cs);
            n = g_seen_count;
            memcpy(seen, g_seen, sizeof(seen));
            LeaveCriticalSection(&g_ddraw.cs);

            if (n == 0)
                lomhd_log("seen: nothing known");

            for (int i = 0; i < n; i++)
            {
                char line[160];
                _snprintf(line, sizeof(line), "seen: %s at (%d,%d) %s",
                    g_pack.portraits[seen[i].portrait].name, seen[i].x, seen[i].y,
                    seen[i].rule ? "round" : "truncate");
                line[sizeof(line) - 1] = 0;
                lomhd_log(line);
            }

            logged_seq = seq;
        }

        if (g_debug && GetFileAttributesA(trigger) != INVALID_FILE_ATTRIBUTES)
        {
            DeleteFileA(trigger);
            InterlockedExchange(&g_want_frame, 1);
        }

        DWORD now = GetTickCount();

        if (g_debug && now - last_report >= 5000)
        {
            lomhd_logf("watchdog: frames=%ld want_frame=%ld textures=%ld KB",
                InterlockedExchange(&g_frames, 0), g_want_frame, (long)(g_tex_bytes >> 10));
            last_report = now;
        }
    }

    return 0;
}

/* ------------------------------------------------------------------------------------------- */
/* Matching (render thread, g_ddraw.cs held -- memory only)                                    */
/* ------------------------------------------------------------------------------------------- */

/* 255 wherever the frame still shows the original's exact pixel, 0 where something covers it --
 * the cursor, a tooltip, text on a page, the live map under the interface bar, a unit in front of
 * a tree. The shader draws the upscale only where this is set, so a partly covered picture is still
 * drawn correctly. A sprite's transparent and shadow pixels are never its own: 0 as well. */
static void build_mask(const PLACEMENT* pl, const BYTE* idx, const WORD* frame, int pitch_px, BYTE* out)
{
    const PORTRAIT* r = &g_pack.portraits[pl->portrait];
    const WORD* lut = r->lut[pl->rule];

    for (int y = 0; y < r->h; y++)
    {
        const WORD* row = frame + (pl->y + y) * pitch_px + pl->x;
        const BYTE* t = idx + y * r->w;
        BYTE* m = out + y * r->w;

        for (int x = 0; x < r->w; x++)
            m[x] = !r->skip[t[x]] && row[x] == lut[t[x]] ? 255 : 0;
    }
}

static void lomhd_scan(void)
{
    IDirectDrawSurfaceImpl* primary = g_ddraw.primary;

    if (!primary || primary->bpp != 16 || !g_pack_ready)
    {
        g_placement_count = 0;
        g_slot_count = 0;
        return;
    }

    const WORD* frame = dds_GetBuffer(primary);

    if (!frame)
    {
        g_placement_count = 0;
        g_slot_count = 0;
        return;
    }

    PLACEMENT result[LOMHD_MAX_PLACEMENTS];
    int found = lomhd_find(&g_pack, frame, primary->width, primary->height, primary->pitch / 2,
        result, LOMHD_MAX_PLACEMENTS);

    BOOL changed = found != g_seen_count;

    for (int k = 0; k < found && !changed; k++)
        changed = memcmp(&result[k], &g_seen[k], sizeof(PLACEMENT)) != 0;

    memcpy(g_placements, result, sizeof(PLACEMENT) * found);
    g_placement_count = found;
    g_scan_count++;

    int slots = 0;
    BOOL requested = FALSE;
    float fw = (float)primary->width, fh = (float)primary->height;

    for (int i = 0; i < found; i++)
    {
        int p = result[i].portrait;
        const PORTRAIT* r = &g_pack.portraits[p];
        LOMHD_IMG* img = &g_img[p];

        img->last_used = g_scan_count;

        if (img->state == IMG_NONE)
        {
            InterlockedExchange(&img->state, IMG_REQUESTED);
            requested = TRUE;
        }

        if (img->state != IMG_READY)
            continue;                       /* drawn once loaded; the original shows until then */

        size_t need = (size_t)r->w * r->h;

        if (g_slot_mask_cap[slots] < need)
        {
            free(g_slot_mask[slots]);
            g_slot_mask[slots] = malloc(need);
            g_slot_mask_cap[slots] = g_slot_mask[slots] ? need : 0;
        }

        if (!g_slot_mask[slots])
            break;

        build_mask(&result[i], r->idx ? r->idx : img->idx, frame, primary->pitch / 2, g_slot_mask[slots]);
        g_slot_img[slots] = p;
        g_quad[slots][0] = result[i].x / fw * 2 - 1;
        g_quad[slots][1] = 1 - result[i].y / fh * 2;
        g_quad[slots][2] = (result[i].x + r->w) / fw * 2 - 1;
        g_quad[slots][3] = 1 - (result[i].y + r->h) / fh * 2;
        slots++;
    }

    g_slot_count = slots;

    /* A picture that finished loading after it left the screen -- a loading screen seen for one
     * frame -- would keep its upscale in memory for the session, outside the texture budget: only
     * the upload frees it. So a loaded picture unseen for LOMHD_STALE_SCANS scans is dropped, to
     * be loaded again if it comes back. From READY on the buffers are the render thread's.
     * (Claude review, 2026-09-23.) */
    for (int p = 0; p < g_pack.count; p++)
    {
        LOMHD_IMG* img = &g_img[p];

        if (img->state == IMG_READY && !img->tex && g_scan_count - img->last_used > LOMHD_STALE_SCANS)
        {
            free(img->rgb);
            free(img->idx);
            img->rgb = img->idx = NULL;
            InterlockedExchange(&img->state, IMG_NONE);
        }
    }

    if (requested)
        SetEvent(g_worker_wake);

    if (changed)
    {
        memcpy(g_seen, result, sizeof(PLACEMENT) * found);
        g_seen_count = found;
        InterlockedIncrement(&g_seen_seq);
        SetEvent(g_worker_wake);
    }
}

/* ------------------------------------------------------------------------------------------- */
/* Drawing (render thread, the GL context current)                                             */
/* ------------------------------------------------------------------------------------------- */

static char LOMHD_VERT[] =
    "#version 150\n"
    "in vec2 pos;\n"
    "in vec2 uv;\n"
    "out vec2 tc;\n"
    "void main() { gl_Position = vec4(pos, 0.0, 1.0); tc = uv; }\n";

/* Discard rather than blend: discarding needs no blend state, which cnc-ddraw never sets and this
 * overlay does not want to own. For a picture the mask is sampled NEAREST, so each original pixel
 * decides the upscale pixels over it. A sprite's edge is its upscale's own alpha, times the mask
 * sampled LINEAR, cut at one half: the outline follows the upscale at twice the resolution rather
 * than the original's pixel steps, and a cover (a unit in front) still cuts it out. */
static char LOMHD_FRAG[] =
    "#version 150\n"
    "uniform sampler2D tex;\n"
    "uniform sampler2D mask;\n"
    "uniform int masked;\n"
    "in vec2 tc;\n"
    "out vec4 color;\n"
    "void main() {\n"
    "  vec4 t = texture(tex, tc);\n"
    "  float keep = masked != 0 ? t.a * texture(mask, tc).r : texture(mask, tc).r;\n"
    "  if (keep < 0.5) discard;\n"
    "  color = vec4(t.rgb, 1.0);\n"
    "}\n";

static GLuint g_program, g_vao, g_vbo, g_ebo, g_mask_tex[LOMHD_MAX_PLACEMENTS];
static PFNGLGETINTEGERVPROC lomhd_glGetIntegerv;
static HGLRC g_gl_context;              /* the context g_program and friends were created in */

typedef HGLRC (WINAPI* PFNWGLGETCURRENTCONTEXT)(void);
static PFNWGLGETCURRENTCONTEXT lomhd_wglGetCurrentContext;

/* cnc-ddraw fetches glGetIntegerv through wglGetProcAddress, which returns NULL for a GL 1.1
 * function on Wine, and guards its own use of it. The first live build did not: it called through
 * NULL and crashed on the first portrait (EIP 0, GL_CURRENT_PROGRAM on the stack). GL 1.1 and wgl
 * functions come from opengl32.dll itself. */
static BOOL lomhd_resolve_basics(void)
{
    HMODULE gl = GetModuleHandleA("opengl32.dll");

    if (!lomhd_glGetIntegerv)
        lomhd_glGetIntegerv = glGetIntegerv ? glGetIntegerv :
            (gl ? (PFNGLGETINTEGERVPROC)GetProcAddress(gl, "glGetIntegerv") : NULL);

    if (!lomhd_wglGetCurrentContext && gl)
        lomhd_wglGetCurrentContext = (PFNWGLGETCURRENTCONTEXT)GetProcAddress(gl, "wglGetCurrentContext");

    return lomhd_glGetIntegerv && lomhd_wglGetCurrentContext;
}
static GLint g_pos_loc, g_uv_loc, g_tex_loc, g_mask_loc, g_masked_loc;
static BOOL g_gl_failed;
static BOOL g_gl_failed_permanently;   /* a missing entry point does not come back */

/* Every texture this overlay made belongs to the context it was made in. */
static void lomhd_forget_textures(void)
{
    for (int p = 0; g_img && p < g_pack.count; p++)
    {
        if (!g_img[p].tex)
            continue;

        /* The upscale's CPU copy was freed at upload, so the picture must be loaded again. */
        g_img[p].tex = 0;
        free(g_img[p].idx);
        g_img[p].idx = NULL;
        InterlockedExchange(&g_img[p].state, IMG_NONE);
    }

    g_tex_bytes = 0;
    memset(g_mask_tex, 0, sizeof(g_mask_tex));

    /* The slots already built name pictures that are now unloaded. On a still screen nothing else
     * would rescan, so they would never be asked for again. (Codex review, 2026-09-23.) */
    g_rescan = TRUE;
}

/* Whether the current context can run the overlay at all. Decided BEFORE lomhd_draw queries any
 * state: GL_VERTEX_ARRAY_BINDING is GL 3.0 state, and querying it in a 2.x context raises
 * GL_INVALID_ENUM, which cnc-ddraw's first-ten-frames glGetError check in render_ogl.c reads as its
 * own failure and turns the OpenGL renderer off -- the whole game, not just the overlay.
 * (Found by cross-model review, second pass, 2026-09-22.) */
static BOOL lomhd_gl_usable(void)
{
    /* cnc-ddraw creates a fresh GL context every time its render thread starts -- window resize,
     * fullscreen toggle, display mode change -- and deletes the old one. Object names from the old
     * context mean nothing in the new one, so the objects are owned by the context they were made
     * in, and remade when it changes. (Found by cross-model review, 2026-09-22.) */
    HGLRC current = lomhd_wglGetCurrentContext();

    if (current != g_gl_context)
    {
        g_program = g_vao = g_vbo = g_ebo = 0;
        lomhd_forget_textures();
        g_gl_failed = FALSE;
        g_gl_context = current;
    }

    if (g_program || g_gl_failed)
        return g_program != 0;

    /* glGetString(GL_VERSION) is valid in every context and needs no newer state to ask. */
    const char* version = glGetString ? (const char*)glGetString(GL_VERSION) : NULL;

    if (!version || version[0] < '3' || version[0] > '9')
    {
        g_gl_missing = "an OpenGL 3 context (this one is older)";
        g_gl_failed = TRUE;
        SetEvent(g_worker_wake);
        return FALSE;
    }

    /* Every entry point lomhd_draw and lomhd_gl_init use, checked before the first call. A missing
     * one turns the overlay off -- vanilla pictures -- and the worker logs which; never a crash. */
    struct { const char* name; void* fn; } needed[] = {
        { "glGetIntegerv", (void*)lomhd_glGetIntegerv }, { "glUseProgram", (void*)glUseProgram },
        { "glGetAttribLocation", (void*)glGetAttribLocation },
        { "glGetUniformLocation", (void*)glGetUniformLocation }, { "glUniform1i", (void*)glUniform1i },
        { "glGenVertexArrays", (void*)glGenVertexArrays }, { "glBindVertexArray", (void*)glBindVertexArray },
        { "glGenBuffers", (void*)glGenBuffers }, { "glBindBuffer", (void*)glBindBuffer },
        { "glBufferData", (void*)glBufferData }, { "glBufferSubData", (void*)glBufferSubData },
        { "glVertexAttribPointer", (void*)glVertexAttribPointer },
        { "glEnableVertexAttribArray", (void*)glEnableVertexAttribArray },
        { "glActiveTexture", (void*)glActiveTexture }, { "glGenTextures", (void*)glGenTextures },
        { "glBindTexture", (void*)glBindTexture }, { "glTexParameteri", (void*)glTexParameteri },
        { "glTexImage2D", (void*)glTexImage2D }, { "glDrawElements", (void*)glDrawElements },
        { "glDeleteTextures", (void*)glDeleteTextures }, { "glPixelStorei", (void*)glPixelStorei },
    };

    for (size_t i = 0; i < sizeof(needed) / sizeof(needed[0]); i++)
    {
        if (!needed[i].fn)
        {
            g_gl_missing = needed[i].name;
            g_gl_failed = TRUE;
            SetEvent(g_worker_wake);
            return FALSE;
        }
    }

    return TRUE;
}

static void texture_params(GLint filter)
{
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

/* Objects for the current context. lomhd_gl_usable has already vetted it. */
static BOOL lomhd_gl_init(void)
{
    if (g_program || g_gl_failed)
        return g_program != 0;

    g_program = oglu_build_program(LOMHD_VERT, LOMHD_FRAG, TRUE);

    if (!g_program)
    {
        g_gl_missing = "the overlay shader (it did not compile or link)";
        g_gl_failed = TRUE;
        SetEvent(g_worker_wake);
        return FALSE;
    }

    g_pos_loc = glGetAttribLocation(g_program, "pos");
    g_uv_loc = glGetAttribLocation(g_program, "uv");
    g_tex_loc = glGetUniformLocation(g_program, "tex");
    g_mask_loc = glGetUniformLocation(g_program, "mask");
    g_masked_loc = glGetUniformLocation(g_program, "masked");

    static const GLushort indices[6] = { 0, 1, 2, 0, 2, 3 };

    glGenVertexArrays(1, &g_vao);
    glBindVertexArray(g_vao);
    glGenBuffers(1, &g_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * 16, NULL, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(g_pos_loc, 2, GL_FLOAT, GL_FALSE, sizeof(GLfloat) * 4, (void*)0);
    glEnableVertexAttribArray(g_pos_loc);
    glVertexAttribPointer(g_uv_loc, 2, GL_FLOAT, GL_FALSE, sizeof(GLfloat) * 4,
        (void*)(sizeof(GLfloat) * 2));
    glEnableVertexAttribArray(g_uv_loc);
    glGenBuffers(1, &g_ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    glBindVertexArray(0);

    glGenTextures(LOMHD_MAX_PLACEMENTS, g_mask_tex);

    for (int i = 0; i < LOMHD_MAX_PLACEMENTS; i++)
    {
        glBindTexture(GL_TEXTURE_2D, g_mask_tex[i]);
        texture_params(GL_NEAREST);
    }

    return TRUE;
}

static BOOL on_screen(int p)
{
    for (int i = 0; i < g_slot_count; i++)
        if (g_slot_img[i] == p)
            return TRUE;

    return FALSE;
}

/* Over budget: drop the least recently drawn upscale that is not on screen this frame. */
static void lomhd_evict(void)
{
    while (g_tex_bytes > LOMHD_TEX_BUDGET)
    {
        int oldest = -1;

        for (int p = 0; p < g_pack.count; p++)
            if (g_img[p].tex && !on_screen(p) && (oldest < 0 || g_img[p].last_used < g_img[oldest].last_used))
                oldest = p;

        if (oldest < 0)
            return;

        LOMHD_IMG* img = &g_img[oldest];
        const PORTRAIT* r = &g_pack.portraits[oldest];
        glDeleteTextures(1, &img->tex);
        img->tex = 0;
        g_tex_bytes -= (size_t)r->hw * r->hh * 4;
        free(img->idx);
        img->idx = NULL;
        InterlockedExchange(&img->state, IMG_NONE);
    }
}

void lomhd_draw(void)
{
    /* Runs WITHOUT g_ddraw.cs: reads only the slots lomhd_scan built under it, on this same render
     * thread, never the game's surface. */
    if (!g_slot_count || g_gl_failed_permanently)
        return;

    if (!lomhd_resolve_basics() || !glActiveTexture)
    {
        g_gl_missing = "glGetIntegerv, wglGetCurrentContext or glActiveTexture";
        g_gl_failed_permanently = TRUE;
        SetEvent(g_worker_wake);
        return;
    }

    if (!lomhd_gl_usable())
        return;

    /* Captured BEFORE lomhd_gl_init, which binds objects of its own the first time it runs in a
     * context; capturing after it would "restore" lomhd's bindings instead of cnc-ddraw's. Both
     * texture units the shader samples, and the unpack state: cnc-ddraw sets its own row length
     * for its frame upload, and R8 and RGB rows are rarely a multiple of four bytes. */
    GLint old_program, old_vao, old_active, old_tex0, old_tex1, old_array_buffer, old_align, old_row;
    lomhd_glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &old_array_buffer);
    lomhd_glGetIntegerv(GL_CURRENT_PROGRAM, &old_program);
    lomhd_glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &old_vao);
    lomhd_glGetIntegerv(GL_ACTIVE_TEXTURE, &old_active);
    lomhd_glGetIntegerv(GL_UNPACK_ALIGNMENT, &old_align);
    lomhd_glGetIntegerv(GL_UNPACK_ROW_LENGTH, &old_row);
    glActiveTexture(GL_TEXTURE1);
    lomhd_glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex1);
    glActiveTexture(GL_TEXTURE0);
    lomhd_glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex0);

    if (!lomhd_gl_init())
    {
        glActiveTexture(old_active);
        return;
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    /* Upscales that arrived since the last draw: upload once, then the CPU copy goes. */
    for (int i = 0; i < g_slot_count; i++)
    {
        LOMHD_IMG* img = &g_img[g_slot_img[i]];
        const PORTRAIT* r = &g_pack.portraits[g_slot_img[i]];

        if (img->tex || !img->rgb)
            continue;

        glGenTextures(1, &img->tex);
        glBindTexture(GL_TEXTURE_2D, img->tex);
        texture_params(GL_LINEAR);
        if (r->masked)
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, r->hw, r->hh, 0, GL_RGBA, GL_UNSIGNED_BYTE, img->rgb);
        else
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, r->hw, r->hh, 0, GL_RGB, GL_UNSIGNED_BYTE, img->rgb);
        free(img->rgb);
        img->rgb = NULL;
        g_tex_bytes += (size_t)r->hw * r->hh * 4;
    }

    lomhd_evict();

    glUseProgram(g_program);
    glUniform1i(g_tex_loc, 0);
    glUniform1i(g_mask_loc, 1);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);

    for (int i = 0; i < g_slot_count; i++)
    {
        LOMHD_IMG* img = &g_img[g_slot_img[i]];
        const PORTRAIT* r = &g_pack.portraits[g_slot_img[i]];

        if (!img->tex)
            continue;

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, g_mask_tex[i]);
        texture_params(r->masked ? GL_LINEAR : GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, r->w, r->h, 0, GL_RED, GL_UNSIGNED_BYTE, g_slot_mask[i]);
        glUniform1i(g_masked_loc, r->masked);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, img->tex);

        /* Texture row 0 is the top of the picture, the convention cnc-ddraw uses for the frame. */
        float x0 = g_quad[i][0], y0 = g_quad[i][1], x1 = g_quad[i][2], y1 = g_quad[i][3];
        GLfloat quad[16] = {
            x0, y0, 0, 0,
            x1, y0, 1, 0,
            x1, y1, 1, 1,
            x0, y1, 0, 1,
        };

        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(quad), quad);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, old_align);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, old_row);
    glBindVertexArray(old_vao);
    glBindBuffer(GL_ARRAY_BUFFER, old_array_buffer);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, old_tex1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, old_tex0);
    glActiveTexture(old_active);
    glUseProgram(old_program);
}

/* ------------------------------------------------------------------------------------------- */
/* Per frame (render thread, g_ddraw.cs held)                                                  */
/* ------------------------------------------------------------------------------------------- */

static void lomhd_copy_frame_if_wanted(void)
{
    if (!g_want_frame || g_frame_ready)
        return;

    IDirectDrawSurfaceImpl* primary = g_ddraw.primary;

    if (!primary)
        return;

    DWORD row = primary->width * primary->bytes_pp;

    if (!g_frame)
        g_frame = HeapAlloc(GetProcessHeap(), 0, 1920 * 1440 * 4);

    if (!g_frame || row * primary->height > 1920 * 1440 * 4)
        return;

    BYTE* src = dds_GetBuffer(primary);

    if (!src)
        return;

    for (DWORD y = 0; y < primary->height; y++)
        memcpy(g_frame + y * row, src + y * primary->pitch, row);

    g_frame_w = primary->width;
    g_frame_h = primary->height;
    g_frame_bpp = primary->bytes_pp * 8;

    InterlockedExchange(&g_want_frame, 0);
    InterlockedExchange(&g_frame_ready, 1);
    SetEvent(g_worker_wake);
}

void lomhd_on_frame(const char* renderer)
{
    if (!g_worker_wake)
    {
        g_worker_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
        CloseHandle(CreateThread(NULL, 0, lomhd_worker, NULL, 0, NULL));
    }

    g_renderer = renderer;
    InterlockedIncrement(&g_frames);
    lomhd_copy_frame_if_wanted();

    /* Only the OpenGL renderer can draw the overlay, so only it pays for the scan. A frame that
     * has not changed cannot have moved a picture, so it keeps the last scan -- unless an upscale
     * arrived since: on a still screen nothing else would ever rebuild the slots to draw it. */
    static LONG scanned_loads;
    LONG loads = g_loaded_seq;

    if (strcmp(renderer, "opengl") == 0 &&
        (g_ddraw.render.surface_updated || !g_placement_count || loads != scanned_loads || g_rescan))
    {
        scanned_loads = loads;
        g_rescan = FALSE;
        lomhd_scan();
    }
}
