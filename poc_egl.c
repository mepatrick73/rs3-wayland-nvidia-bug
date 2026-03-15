/*
 * poc_egl.c - Demonstrates the Mesa vs Nvidia EGL dispatch bug on Nvidia+Wayland
 *
 * Compile:
 *   gcc poc_egl.c -o poc_egl -ldl
 *
 * Run:
 *   ./poc_egl
 *
 * What this shows:
 *   BUG:  eglGetDisplay(EGL_DEFAULT_DISPLAY) via GLVND → Mesa → llvmpipe (CPU)
 *   FIX:  eglGetPlatformDisplayEXT(EGL_PLATFORM_X11_EXT, x11_display) → GPU
 *         (Option A fix: explicit platform + display pointer, one-line change)
 *
 * The vendor ICD libraries (libEGL_mesa.so.0, libEGL_nvidia.so.0) do NOT export
 * standard egl* symbols — they use GLVND's internal dispatch table. You cannot
 * dlopen them and call eglGetDisplay directly. GLVND dispatches internally based
 * on which vendor's eglGetDisplay succeeds for the given display/platform.
 *
 * Root cause: EGL_DEFAULT_DISPLAY + X11 platform → GLVND routes to Mesa.
 * Mesa has no DRI2/DRI3 driver for nvidia.ko → silent fallback to llvmpipe.
 * Nvidia's EGL ICD requires an explicit platform handle to accept the display.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

typedef void*          EGLDisplay;
typedef void*          EGLConfig;
typedef void*          EGLContext;
typedef void*          EGLSurface;
typedef int            EGLint;
typedef unsigned int   EGLBoolean;
typedef unsigned int   EGLenum;
typedef void*          EGLNativeDisplayType;

#define EGL_DEFAULT_DISPLAY         ((EGLNativeDisplayType)0)
#define EGL_NO_DISPLAY              ((EGLDisplay)0)
#define EGL_NO_CONTEXT              ((EGLContext)0)
#define EGL_NO_SURFACE              ((EGLSurface)0)
#define EGL_OPENGL_ES_API           0x30A0
#define EGL_RENDERABLE_TYPE         0x3040
#define EGL_OPENGL_ES2_BIT          0x0004
#define EGL_CONTEXT_CLIENT_VERSION  0x3098
#define EGL_PBUFFER_BIT             0x0001
#define EGL_SURFACE_TYPE            0x3033
#define EGL_WIDTH                   0x3057
#define EGL_HEIGHT                  0x3056
#define EGL_VENDOR                  0x3053
#define EGL_NONE                    0x3038
#define EGL_BAD_MATCH               0x3009
#define GL_RENDERER                 0x1F01
#define GL_VENDOR                   0x1F00

/* EGL_EXT_platform_base — used by Option A fix */
#define EGL_PLATFORM_X11_EXT        0x31D5

typedef void XDisplay;

typedef EGLDisplay   (*fn_eglGetDisplay)            (EGLNativeDisplayType);
typedef EGLDisplay   (*fn_eglGetPlatformDisplayEXT) (EGLenum, void*, const EGLint*);
typedef EGLBoolean   (*fn_eglInitialize)            (EGLDisplay, EGLint*, EGLint*);
typedef EGLBoolean   (*fn_eglBindAPI)               (EGLenum);
typedef EGLBoolean   (*fn_eglChooseConfig)          (EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*);
typedef EGLContext   (*fn_eglCreateContext)         (EGLDisplay, EGLConfig, EGLContext, const EGLint*);
typedef EGLSurface   (*fn_eglCreatePbufferSurface)  (EGLDisplay, EGLConfig, const EGLint*);
typedef EGLBoolean   (*fn_eglMakeCurrent)           (EGLDisplay, EGLSurface, EGLSurface, EGLContext);
typedef EGLint       (*fn_eglGetError)              (void);
typedef const char*  (*fn_eglQueryString)           (EGLDisplay, EGLint);
typedef EGLBoolean   (*fn_eglTerminate)             (EGLDisplay);
typedef void*        (*fn_eglGetProcAddress)        (const char*);
typedef const unsigned char* (*fn_glGetString)      (unsigned int);

typedef XDisplay*    (*fn_XOpenDisplay)             (const char*);
typedef int          (*fn_XCloseDisplay)            (XDisplay*);

/* Returns 0=PASS(GPU), 1=SOFTWARE(llvmpipe), 2=FAIL */
static int check_renderer(fn_eglGetProcAddress eglGetProcAddress,
                           fn_eglMakeCurrent eglMakeCurrent,
                           fn_eglCreatePbufferSurface eglCreatePbufferSurface,
                           fn_eglGetError eglGetError,
                           EGLDisplay dpy, EGLConfig cfg, EGLContext ctx)
{
    const EGLint surf_attrs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, surf_attrs);

    if (surf == EGL_NO_SURFACE || !eglMakeCurrent(dpy, surf, surf, ctx)) {
        printf("  (could not make current — cannot query renderer)\n");
        return 2;
    }

    fn_glGetString glGetString = (fn_glGetString)eglGetProcAddress("glGetString");
    if (!glGetString) {
        printf("  (glGetString unavailable)\n");
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        return 2;
    }

    const char* gl_vendor   = (const char*)glGetString(GL_VENDOR);
    const char* gl_renderer = (const char*)glGetString(GL_RENDERER);
    printf("  GL vendor:   \"%s\"\n", gl_vendor   ? gl_vendor   : "(null)");
    printf("  GL renderer: \"%s\"\n", gl_renderer ? gl_renderer : "(null)");

    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    if (!gl_renderer) return 2;
    if (strstr(gl_renderer, "llvmpipe") ||
        strstr(gl_renderer, "softpipe") ||
        strstr(gl_renderer, "Software")) {
        return 1;
    }
    return 0;
}

/* Test one EGLDisplay. Returns 0=PASS, 1=SOFTWARE, 2=FAIL */
static int test_display(EGLDisplay dpy,
                         fn_eglInitialize eglInitialize,
                         fn_eglBindAPI eglBindAPI,
                         fn_eglChooseConfig eglChooseConfig,
                         fn_eglCreateContext eglCreateContext,
                         fn_eglCreatePbufferSurface eglCreatePbufferSurface,
                         fn_eglMakeCurrent eglMakeCurrent,
                         fn_eglGetError eglGetError,
                         fn_eglQueryString eglQueryString,
                         fn_eglTerminate eglTerminate,
                         fn_eglGetProcAddress eglGetProcAddress)
{
    EGLint major = 0, minor = 0;
    if (!eglInitialize(dpy, &major, &minor)) {
        printf("  eglInitialize: FAIL (0x%x)\n", eglGetError());
        eglTerminate(dpy);
        return 2;
    }
    const char* vendor = eglQueryString(dpy, EGL_VENDOR);
    printf("  eglInitialize: OK — EGL %d.%d\n", major, minor);
    printf("  EGL vendor:    \"%s\"\n", vendor ? vendor : "(null)");

    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint cfg_attrs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_NONE,
    };
    EGLConfig cfg;
    EGLint ncfg = 0;
    if (!eglChooseConfig(dpy, cfg_attrs, &cfg, 1, &ncfg) || ncfg == 0) {
        printf("  eglChooseConfig: FAIL (0x%x)\n", eglGetError());
        eglTerminate(dpy);
        return 2;
    }

    const EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (ctx == EGL_NO_CONTEXT) {
        EGLint err = eglGetError();
        printf("  eglCreateContext: FAIL (0x%x", err);
        if (err == EGL_BAD_MATCH)
            printf(" = EGL_BAD_MATCH: no GPU driver for this display");
        printf(")\n");
        eglTerminate(dpy);
        return 2;
    }

    int result = check_renderer(eglGetProcAddress, eglMakeCurrent,
                                 eglCreatePbufferSurface, eglGetError,
                                 dpy, cfg, ctx);
    eglTerminate(dpy);
    return result;
}

static const char* result_label(int r) {
    if (r == 0) return "PASS — hardware GPU";
    if (r == 1) return "SOFTWARE FALLBACK — llvmpipe (CPU, not GPU)";
    return "FAIL — no context";
}

int main(void) {
    void* egl_lib = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!egl_lib) {
        fprintf(stderr, "Cannot load libEGL.so.1: %s\n", dlerror());
        return 1;
    }

    void* x11_lib = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
    if (!x11_lib) {
        fprintf(stderr, "Cannot load libX11.so.6: %s\n", dlerror());
        dlclose(egl_lib);
        return 1;
    }

    fn_eglGetDisplay           eglGetDisplay           = dlsym(egl_lib, "eglGetDisplay");
    fn_eglInitialize           eglInitialize           = dlsym(egl_lib, "eglInitialize");
    fn_eglBindAPI              eglBindAPI              = dlsym(egl_lib, "eglBindAPI");
    fn_eglChooseConfig         eglChooseConfig         = dlsym(egl_lib, "eglChooseConfig");
    fn_eglCreateContext        eglCreateContext        = dlsym(egl_lib, "eglCreateContext");
    fn_eglCreatePbufferSurface eglCreatePbufferSurface = dlsym(egl_lib, "eglCreatePbufferSurface");
    fn_eglMakeCurrent          eglMakeCurrent          = dlsym(egl_lib, "eglMakeCurrent");
    fn_eglGetError             eglGetError             = dlsym(egl_lib, "eglGetError");
    fn_eglQueryString          eglQueryString          = dlsym(egl_lib, "eglQueryString");
    fn_eglTerminate            eglTerminate            = dlsym(egl_lib, "eglTerminate");
    fn_eglGetProcAddress       eglGetProcAddress       = dlsym(egl_lib, "eglGetProcAddress");

    fn_XOpenDisplay  XOpenDisplay  = dlsym(x11_lib, "XOpenDisplay");
    fn_XCloseDisplay XCloseDisplay = dlsym(x11_lib, "XCloseDisplay");

    if (!eglGetDisplay || !eglInitialize || !eglBindAPI || !eglChooseConfig ||
        !eglCreateContext || !eglCreatePbufferSurface || !eglMakeCurrent ||
        !eglGetError || !eglQueryString || !eglTerminate || !eglGetProcAddress) {
        fprintf(stderr, "Missing required EGL symbols\n");
        dlclose(x11_lib);
        dlclose(egl_lib);
        return 1;
    }

    if (!XOpenDisplay || !XCloseDisplay) {
        fprintf(stderr, "Missing required X11 symbols\n");
        dlclose(x11_lib);
        dlclose(egl_lib);
        return 1;
    }

    fn_eglGetPlatformDisplayEXT eglGetPlatformDisplayEXT =
        (fn_eglGetPlatformDisplayEXT)eglGetProcAddress("eglGetPlatformDisplayEXT");

    /* ------------------------------------------------------------------ */
    printf("=== TEST 1: eglGetDisplay(EGL_DEFAULT_DISPLAY) — the RS3 code path ===\n");
    printf("With $DISPLAY set (XWayland active), GLVND picks the X11 platform and\n");
    printf("routes to Mesa, which has no DRI2/DRI3 driver for nvidia.ko.\n\n");

    int result_default = 2;
    {
        EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (dpy == EGL_NO_DISPLAY) {
            printf("  eglGetDisplay: FAIL (0x%x)\n", eglGetError());
        } else {
            printf("  eglGetDisplay: OK\n");
            result_default = test_display(dpy,
                eglInitialize, eglBindAPI, eglChooseConfig,
                eglCreateContext, eglCreatePbufferSurface, eglMakeCurrent,
                eglGetError, eglQueryString, eglTerminate, eglGetProcAddress);
        }
    }
    printf("RESULT: %s\n\n", result_label(result_default));

    /* ------------------------------------------------------------------ */
    printf("=== TEST 2: eglGetPlatformDisplayEXT(EGL_PLATFORM_X11_EXT, x11_display) — Option A fix ===\n");
    printf("Explicit platform + Display* pointer. GLVND routes by vendor priority:\n");
    printf("Nvidia (10) beats Mesa (50). x11_display matches what SDL_GetWindowWMInfo provides.\n\n");

    int result_x11ext = 2;
    if (!eglGetPlatformDisplayEXT) {
        printf("  SKIP — eglGetPlatformDisplayEXT not available via eglGetProcAddress\n\n");
    } else {
        XDisplay* x11_dpy = XOpenDisplay(NULL);
        if (!x11_dpy) {
            printf("  SKIP — XOpenDisplay(NULL) failed (is $DISPLAY set?)\n\n");
        } else {
            EGLDisplay dpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_X11_EXT, x11_dpy, NULL);
            if (dpy == EGL_NO_DISPLAY) {
                printf("  eglGetPlatformDisplayEXT: FAIL (0x%x)\n\n", eglGetError());
            } else {
                printf("  eglGetPlatformDisplayEXT: OK\n");
                result_x11ext = test_display(dpy,
                    eglInitialize, eglBindAPI, eglChooseConfig,
                    eglCreateContext, eglCreatePbufferSurface, eglMakeCurrent,
                    eglGetError, eglQueryString, eglTerminate, eglGetProcAddress);
            }
            XCloseDisplay(x11_dpy);
        }
    }
    printf("RESULT: %s\n\n", result_label(result_x11ext));

    /* ------------------------------------------------------------------ */
    printf("=== Summary ===\n");
    printf("  TEST 1 — eglGetDisplay(EGL_DEFAULT_DISPLAY)              [RS3 code path]: %s\n", result_label(result_default));
    printf("  TEST 2 — eglGetPlatformDisplayEXT(EGL_PLATFORM_X11_EXT) [Option A fix]:  %s\n", result_label(result_x11ext));
    printf("\n");

    if (result_default == 1 && result_x11ext == 0) {
        printf("BUG CONFIRMED: Default EGL dispatch falls back to llvmpipe.\n");
        printf("               Explicit X11 platform selection gives hardware rendering.\n");
        printf("\n");
        printf("WHY: eglGetDisplay(EGL_DEFAULT_DISPLAY) with $DISPLAY set → X11 platform\n");
        printf("     → GLVND routes to Mesa (handles NULL on X11) → Mesa has no DRI2 for nvidia.ko\n");
        printf("     → Mesa falls back to llvmpipe, no error returned to the caller\n");
        printf("\n");
        printf("FIX: Replace eglGetDisplay(EGL_DEFAULT_DISPLAY) with\n");
        printf("     eglGetPlatformDisplayEXT(EGL_PLATFORM_X11_EXT, x11_display, NULL)\n");
        printf("     where x11_display comes from SDL_GetWindowWMInfo (already available).\n");
        printf("     With an explicit platform, GLVND routes by vendor priority and Nvidia wins.\n");
    } else if (result_default == 0) {
        printf("Default EGL already works on this system — bug may not be present.\n");
    } else {
        printf("Check /usr/share/glvnd/egl_vendor.d/ for Nvidia ICD registration.\n");
        printf("Ensure nvidia-open or nvidia driver package is fully installed.\n");
    }

    dlclose(x11_lib);
    dlclose(egl_lib);
    return 0;
}
