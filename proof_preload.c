/*
 * proof_preload.c — LD_PRELOAD library to log EGL/SDL call arguments
 *
 * Proves at runtime:
 *   1. eglGetDisplay is called with NULL (EGL_DEFAULT_DISPLAY)
 *   2. EGL vendor after eglInitialize
 *   3. GL_RENDERER after context creation
 *   4. SDL_GetWindowWMInfo x11.window matches eglCreateWindowSurface native_win
 *
 * Build:
 *   gcc -shared -fPIC -O0 -o proof_preload.so proof_preload.c -ldl
 *
 * Use (see proof_preload_run.sh):
 *   LD_PRELOAD=/path/to/proof_preload.so ./rs2client [args...]
 *
 * Output: /tmp/rs3_proof.log
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* SDL_SysWMinfo offsets on 64-bit Linux (SDL2):
 *   struct { uint8 major,minor,patch; uint8 pad; uint32 subsystem; union { struct { Display* display; Window window; } x11; } info; }
 *   subsystem at +4, display* at +8, window (unsigned long) at +16
 */
#define WMINFO_SUBSYSTEM_OFFSET   4
#define WMINFO_X11_DISPLAY_OFFSET 8
#define WMINFO_X11_WINDOW_OFFSET  16
#define SDL_SYSWM_X11             2

/* EGL constants */
#define EGL_VENDOR  0x3053
#define EGL_VERSION 0x3054

typedef void*        EGLDisplay;
typedef void*        EGLConfig;
typedef void*        EGLSurface;
typedef void*        EGLContext;
typedef unsigned int EGLenum;
typedef int          EGLint;
typedef int          EGLBoolean;

typedef EGLDisplay (*fn_eglGetDisplay)(void*);
typedef EGLBoolean (*fn_eglInitialize)(EGLDisplay, EGLint*, EGLint*);
typedef EGLSurface (*fn_eglCreateWindowSurface)(EGLDisplay, EGLConfig, void*, const EGLint*);
typedef const char* (*fn_eglQueryString)(EGLDisplay, EGLint);
typedef int         (*fn_SDL_GetWindowWMInfo)(void*, void*);
typedef const unsigned char* (*fn_glGetString)(unsigned int);

static FILE* log_fp = NULL;
static unsigned long sdl_x11_window = 0;

static void log_open(void) {
    if (!log_fp) {
        log_fp = fopen("/tmp/rs3_proof.log", "w");
        if (!log_fp) log_fp = stderr;
        fprintf(log_fp, "=== rs3_proof.log ===\n\n");
        fflush(log_fp);
    }
}

/* ── Claim 1: eglGetDisplay called with NULL ─────────────────────────────── */

EGLDisplay eglGetDisplay(void* native_display) {
    log_open();
    fn_eglGetDisplay real = dlsym(RTLD_NEXT, "eglGetDisplay");
    fprintf(log_fp, "[PROOF 1] eglGetDisplay(native_display=%p)\n", native_display);
    if (native_display == NULL)
        fprintf(log_fp, "          → EGL_DEFAULT_DISPLAY (NULL) — BUG CONFIRMED\n");
    else
        fprintf(log_fp, "          → non-NULL (unexpected)\n");
    fflush(log_fp);
    return real(native_display);
}

/* ── Claim 2: EGL vendor is Mesa ────────────────────────────────────────── */

EGLBoolean eglInitialize(EGLDisplay dpy, EGLint* major, EGLint* minor) {
    log_open();
    fn_eglInitialize real = dlsym(RTLD_NEXT, "eglInitialize");
    EGLBoolean ret = real(dpy, major, minor);
    if (ret) {
        fn_eglQueryString real_query = dlsym(RTLD_NEXT, "eglQueryString");
        const char* vendor  = real_query ? real_query(dpy, EGL_VENDOR)  : "(unavailable)";
        const char* version = real_query ? real_query(dpy, EGL_VERSION) : "(unavailable)";
        fprintf(log_fp, "[PROOF 2] eglInitialize succeeded:\n");
        fprintf(log_fp, "          EGL_VENDOR  = \"%s\"\n", vendor  ? vendor  : "(null)");
        fprintf(log_fp, "          EGL_VERSION = \"%s\"\n", version ? version : "(null)");
        if (vendor && strstr(vendor, "Mesa"))
            fprintf(log_fp, "          → Mesa — BUG CONFIRMED\n");
        fflush(log_fp);
    }
    return ret;
}

/* ── Claim 3: GL renderer is llvmpipe ───────────────────────────────────── */

const unsigned char* glGetString(unsigned int name) {
    log_open();
    fn_glGetString real = dlsym(RTLD_NEXT, "glGetString");
    const unsigned char* ret = real(name);
    if (name == 0x1F01 /* GL_RENDERER */ || name == 0x1F00 /* GL_VENDOR */) {
        const char* label = (name == 0x1F01) ? "GL_RENDERER" : "GL_VENDOR";
        fprintf(log_fp, "[PROOF 3] glGetString(%s) = \"%s\"\n",
                label, ret ? (const char*)ret : "(null)");
        if (name == 0x1F01 && ret && strstr((const char*)ret, "llvmpipe"))
            fprintf(log_fp, "          → llvmpipe — BUG CONFIRMED (software rendering)\n");
        fflush(log_fp);
    }
    return ret;
}

/* ── Claim 4a: SDL_GetWindowWMInfo x11.window ───────────────────────────── */

int SDL_GetWindowWMInfo(void* window, void* info) {
    log_open();
    fn_SDL_GetWindowWMInfo real = dlsym(RTLD_NEXT, "SDL_GetWindowWMInfo");
    int ret = real(window, info);
    if (ret && info) {
        uint32_t      subsystem = *(uint32_t*)      ((char*)info + WMINFO_SUBSYSTEM_OFFSET);
        void*         display   = *(void**)          ((char*)info + WMINFO_X11_DISPLAY_OFFSET);
        unsigned long win       = *(unsigned long*)  ((char*)info + WMINFO_X11_WINDOW_OFFSET);
        sdl_x11_window = win;
        fprintf(log_fp, "[PROOF 4a] SDL_GetWindowWMInfo:\n");
        fprintf(log_fp, "           subsystem   = %u (%s)\n",
                subsystem, subsystem == SDL_SYSWM_X11 ? "X11" : "other");
        fprintf(log_fp, "           x11.display = %p\n", display);
        fprintf(log_fp, "           x11.window  = 0x%lx\n", win);
        fflush(log_fp);
    }
    return ret;
}

/* ── Claim 4b: eglCreateWindowSurface native_win matches x11.window ──────── */

EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config,
                                   void* native_win, const EGLint* attribs) {
    log_open();
    fn_eglCreateWindowSurface real = dlsym(RTLD_NEXT, "eglCreateWindowSurface");
    unsigned long win = (unsigned long)(uintptr_t)native_win;
    fprintf(log_fp, "[PROOF 4b] eglCreateWindowSurface:\n");
    fprintf(log_fp, "           display    = %p\n", dpy);
    fprintf(log_fp, "           native_win = 0x%lx\n", win);
    if (sdl_x11_window != 0) {
        if (win == sdl_x11_window)
            fprintf(log_fp, "           → MATCH: native_win == SDL_GetWindowWMInfo x11.window ✓\n");
        else
            fprintf(log_fp, "           → MISMATCH: SDL reported 0x%lx\n", sdl_x11_window);
    } else {
        fprintf(log_fp, "           → SDL_GetWindowWMInfo not yet called\n");
    }
    fflush(log_fp);
    return real(dpy, config, native_win, attribs);
}
