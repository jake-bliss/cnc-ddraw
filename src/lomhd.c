#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "dd.h"
#include "ddsurface.h"
#include "opengl_utils.h"
#include "lomhd.h"
#include "lomhd_match.h"

/* Lords of Magic HD overlay.
 *
 * The game draws at 640x480 in 16 bpp, exactly as it always has. This finds portraits in the
 * finished frame and draws their upscaled versions over them after cnc-ddraw has scaled the frame
 * to the window. Nothing in lomse.exe is hooked: a portrait is recognised by its pixels, which the
 * engine copies into the frame as exact RGB565 values -- Observed 2026-09-22 for both the native
 * bottom-strip slot and the script-drawn info panel, 100% of pixels. So detection covers every
 * place a portrait can appear, including ones no script names.
 *
 * THE ONE RULE: the render thread never touches a file. It holds g_ddraw.cs whenever it calls in
 * here, and doing file I/O there wedged the render thread under Wine in an early build. The render
 * thread scans, copies and draws; one worker thread owns every file operation. */

static HANDLE g_worker_wake;
static volatile LONG g_frames, g_want_frame, g_frame_ready, g_pack_ready;
static volatile LONG g_seen_seq;

/* Debug mode: a file named `lomhd_debug` beside lomse.exe when the game starts. Players get three
 * kinds of log line -- the pack loaded, the overlay turned itself off, or an error. Debug adds the
 * detection record, a five-second watchdog and the `lomhd_dump` frame trigger: the instruments
 * that found every problem in this file, kept for the next platform it has to be proven on. */
static volatile LONG g_debug;
static volatile const char* g_renderer;
static volatile const char* g_gl_missing;   /* set by the render thread, logged by the worker */

static BYTE* g_frame;
static DWORD g_frame_w, g_frame_h, g_frame_bpp;

static BYTE* g_pack_bytes;
static LOMHD_PACK g_pack;

static PLACEMENT g_placements[LOMHD_MAX_PLACEMENTS];
static int g_placement_count;

/* What lomhd_draw draws, built by lomhd_scan while g_ddraw.cs is held. The draw runs after the
 * renderer has RELEASED that lock, and cnc-ddraw frees the primary's buffer when the game releases
 * the surface -- so the draw must never read the game's frame or surface. It reads only these.
 * (Found by cross-model review, 2026-09-22: the first version built them inside the draw.) */
#define LOMHD_MAX_HD_PIXELS (LOMHD_MAX_HD_SIDE * LOMHD_MAX_HD_SIDE)
static DWORD* g_slot_rgba[LOMHD_MAX_PLACEMENTS];
static int g_slot_w[LOMHD_MAX_PLACEMENTS], g_slot_h[LOMHD_MAX_PLACEMENTS];
static float g_quad[LOMHD_MAX_PLACEMENTS][4];      /* x0, y0, x1, y1 in NDC */
static int g_slot_count;
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
/* The portrait pack (worker thread, once)                                                     */
/* ------------------------------------------------------------------------------------------- */

static BOOL lomhd_load_pack(void)
{
    char path[MAX_PATH];
    lomhd_path(path, sizeof(path), "lomhd_portraits.pack");

    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);

    if (f == INVALID_HANDLE_VALUE)
    {
        lomhd_log("pack: lomhd_portraits.pack not found -- overlay off, game unchanged");
        return FALSE;
    }

    DWORD size = GetFileSize(f, NULL), got = 0, bad = 0;
    g_pack_bytes = HeapAlloc(GetProcessHeap(), 0, size ? size : 1);
    BOOL ok = g_pack_bytes && ReadFile(f, g_pack_bytes, size, &got, NULL) && got == size;
    CloseHandle(f);

    if (!ok || !lomhd_pack_parse(g_pack_bytes, size, &g_pack, &bad))
    {
        lomhd_logf("pack: unreadable or corrupt near byte %ld of %ld -- overlay off",
            (long)bad, (long)size, 0);
        return FALSE;
    }

    lomhd_logf("pack: %ld portraits loaded, %ld bytes, probe width %ld",
        g_pack.count, (long)size, g_pack.probe_width);
    return TRUE;
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
            _snprintf(line, sizeof(line), "overlay OFF: missing %s -- portraits stay vanilla", missing);
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
                lomhd_log("seen: no portraits");

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
            lomhd_logf("watchdog: frames=%ld want_frame=%ld pack=%ld",
                InterlockedExchange(&g_frames, 0), g_want_frame, g_pack_ready);
            last_report = now;
        }
    }

    return 0;
}

/* ------------------------------------------------------------------------------------------- */
/* Matching (render thread, g_ddraw.cs held -- memory only)                                    */
/* ------------------------------------------------------------------------------------------- */

/* The upscale as RGBA, with alpha cleared wherever the frame no longer shows the original pixel
 * at that position -- the cursor, a tooltip, a dialog edge drawn on top. Each original pixel
 * decides the upscale pixels that cover it. */
static void build_masked_rgba(const PLACEMENT* p, const WORD* frame, int pitch_px, DWORD* out)
{
    const PORTRAIT* r = &g_pack.portraits[p->portrait];
    const WORD* t = r->templ[p->rule];

    for (int y = 0; y < r->hh; y++)
    {
        int sy = y * r->h / r->hh;
        const WORD* frame_row = frame + (p->y + sy) * pitch_px + p->x;

        for (int x = 0; x < r->hw; x++)
        {
            int sx = x * r->w / r->hw;
            const BYTE* c = r->hd_pal + r->hd_idx[y * r->hw + x] * 3;
            BOOL visible = frame_row[sx] == t[sy * r->w + sx];

            out[y * r->hw + x] = (visible ? 0xFF000000u : 0) | (c[2] << 16) | (c[1] << 8) | c[0];
        }
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

    int slots = 0;
    float fw = (float)primary->width, fh = (float)primary->height;

    for (int i = 0; i < found; i++)
    {
        const PORTRAIT* r = &g_pack.portraits[result[i].portrait];

        if (r->hw * r->hh > LOMHD_MAX_HD_PIXELS)
            continue;

        if (!g_slot_rgba[slots])
            g_slot_rgba[slots] = HeapAlloc(GetProcessHeap(), 0, LOMHD_MAX_HD_PIXELS * 4);

        if (!g_slot_rgba[slots])
            break;

        build_masked_rgba(&result[i], frame, primary->pitch / 2, g_slot_rgba[slots]);
        g_slot_w[slots] = r->hw;
        g_slot_h[slots] = r->hh;
        g_quad[slots][0] = result[i].x / fw * 2 - 1;
        g_quad[slots][1] = 1 - result[i].y / fh * 2;
        g_quad[slots][2] = (result[i].x + r->w) / fw * 2 - 1;
        g_quad[slots][3] = 1 - (result[i].y + r->h) / fh * 2;
        slots++;
    }

    g_slot_count = slots;

    if (changed)
    {
        memcpy(g_seen, result, sizeof(PLACEMENT) * found);
        g_seen_count = found;
        InterlockedIncrement(&g_seen_seq);
        SetEvent(g_worker_wake);
    }
}

/* ------------------------------------------------------------------------------------------- */
/* Drawing (render thread, g_ddraw.cs held, the GL context current)                            */
/* ------------------------------------------------------------------------------------------- */

static char LOMHD_VERT[] =
    "#version 150\n"
    "in vec2 pos;\n"
    "in vec2 uv;\n"
    "out vec2 tc;\n"
    "void main() { gl_Position = vec4(pos, 0.0, 1.0); tc = uv; }\n";

/* Discard rather than blend: the texture's alpha is a 0/1 mask of which pixels still show the
 * portrait, and discarding needs no blend state, which this context does not expose. */
static char LOMHD_FRAG[] =
    "#version 150\n"
    "uniform sampler2D tex;\n"
    "in vec2 tc;\n"
    "out vec4 color;\n"
    "void main() { vec4 c = texture(tex, tc); if (c.a < 0.5) discard; color = vec4(c.rgb, 1.0); }\n";

static GLuint g_program, g_vao, g_vbo, g_ebo, g_tex[LOMHD_MAX_PLACEMENTS];
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
static GLint g_pos_loc, g_uv_loc, g_tex_loc;
static BOOL g_gl_failed;
static BOOL g_gl_failed_permanently;   /* a missing entry point does not come back */

/* Whether the current context can run the overlay at all. Decided BEFORE lomhd_draw queries any
 * state: GL_VERTEX_ARRAY_BINDING is GL 3.0 state, and querying it in a 2.x context raises
 * GL_INVALID_ENUM, which cnc-ddraw's first-ten-frames glGetError check in render_ogl.c reads as its
 * own failure and turns the OpenGL renderer off -- the whole game, not just the overlay.
 * (Found by cross-model review, second pass, 2026-09-22.) */
static BOOL lomhd_gl_usable(void)
{
    /* cnc-ddraw creates a fresh GL context every time its render thread starts -- window resize,
     * fullscreen toggle, display mode change -- and deletes the old one. Object names from the old
     * context mean nothing in the new one, and binding a stale texture name leaves cnc-ddraw's own
     * frame texture bound, which the upload below would then redefine at portrait size. So the
     * objects are owned by the context they were made in, and remade when it changes.
     * (Found by cross-model review, 2026-09-22.) */
    HGLRC current = lomhd_wglGetCurrentContext();

    if (current != g_gl_context)
    {
        g_program = g_vao = g_vbo = g_ebo = 0;
        memset(g_tex, 0, sizeof(g_tex));
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
     * one turns the overlay off -- vanilla portraits -- and the worker logs which; never a crash. */
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

    glGenTextures(LOMHD_MAX_PLACEMENTS, g_tex);

    for (int i = 0; i < LOMHD_MAX_PLACEMENTS; i++)
    {
        glBindTexture(GL_TEXTURE_2D, g_tex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    return TRUE;
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
     * context; capturing after it would "restore" lomhd's bindings instead of cnc-ddraw's. */
    GLint old_program, old_vao, old_active, old_tex, old_array_buffer;
    lomhd_glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &old_array_buffer);
    lomhd_glGetIntegerv(GL_CURRENT_PROGRAM, &old_program);
    lomhd_glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &old_vao);
    lomhd_glGetIntegerv(GL_ACTIVE_TEXTURE, &old_active);
    glActiveTexture(GL_TEXTURE0);
    lomhd_glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex);

    if (!lomhd_gl_init())
    {
        glActiveTexture(old_active);
        return;
    }

    glUseProgram(g_program);
    glUniform1i(g_tex_loc, 0);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);

    for (int i = 0; i < g_slot_count; i++)
    {
        glBindTexture(GL_TEXTURE_2D, g_tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_slot_w[i], g_slot_h[i], 0, GL_RGBA,
            GL_UNSIGNED_BYTE, g_slot_rgba[i]);

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

    glBindVertexArray(old_vao);
    glBindBuffer(GL_ARRAY_BUFFER, old_array_buffer);
    glBindTexture(GL_TEXTURE_2D, old_tex);
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
     * has not changed cannot have moved a portrait, so it keeps the last scan. */
    if (strcmp(renderer, "opengl") == 0 && (g_ddraw.render.surface_updated || !g_placement_count))
        lomhd_scan();
}
