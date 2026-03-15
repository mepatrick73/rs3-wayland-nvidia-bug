/*
 * poc_egl_wayland.c - Option B: Wayland EGL platform via SDL -> Nvidia GPU
 *
 * Compile:
 *   gcc poc_egl_wayland.c -o poc_egl_wayland -ldl
 *
 * What this shows:
 *   If the game does not bind itself to the X11 platform (i.e. does not call
 *   eglGetDisplay(NULL) with DISPLAY set), GLVND routes to Nvidia directly.
 *
 *   eglGetDisplay(NULL) with DISPLAY set  -> Mesa -> llvmpipe  (the RS3 bug)
 *   eglGetPlatformDisplay(WAYLAND, ...)   -> Nvidia -> GPU     (Option B fix)
 *
 * Note: requires WAYLAND_DISPLAY to be set (standard Wayland session).
 */

#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>

typedef void*         EGLDisplay;
typedef void*         EGLConfig;
typedef void*         EGLContext;
typedef void*         EGLSurface;
typedef void*         EGLNativeDisplayType;
typedef int           EGLint;
typedef long          EGLAttrib;
typedef unsigned int  EGLBoolean;
typedef unsigned int  EGLenum;

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
#define GL_RENDERER                 0x1F01
#define GL_VENDOR                   0x1F00
#define EGL_PLATFORM_WAYLAND_EXT    0x31D8

typedef void*       (*fn_wl_display_connect)    (const char*);
typedef EGLDisplay  (*fn_eglGetDisplay)         (EGLNativeDisplayType);
typedef EGLDisplay  (*fn_eglGetPlatformDisplay) (EGLenum, void*, const EGLAttrib*);
typedef EGLBoolean  (*fn_eglInitialize)         (EGLDisplay, EGLint*, EGLint*);
typedef EGLBoolean  (*fn_eglBindAPI)            (EGLenum);
typedef EGLBoolean  (*fn_eglChooseConfig)       (EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*);
typedef EGLContext  (*fn_eglCreateContext)      (EGLDisplay, EGLConfig, EGLContext, const EGLint*);
typedef EGLSurface  (*fn_eglCreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint*);
typedef EGLBoolean  (*fn_eglMakeCurrent)        (EGLDisplay, EGLSurface, EGLSurface, EGLContext);
typedef EGLint      (*fn_eglGetError)           (void);
typedef const char* (*fn_eglQueryString)        (EGLDisplay, EGLint);
typedef EGLBoolean  (*fn_eglTerminate)          (EGLDisplay);
typedef void*       (*fn_eglGetProcAddress)     (const char*);
typedef const unsigned char* (*fn_glGetString)  (unsigned int);

static void probe(const char *label, EGLDisplay dpy,
                  fn_eglInitialize eglInitialize, fn_eglBindAPI eglBindAPI,
                  fn_eglChooseConfig eglChooseConfig, fn_eglCreateContext eglCreateContext,
                  fn_eglCreatePbufferSurface eglCreatePbufferSurface,
                  fn_eglMakeCurrent eglMakeCurrent, fn_eglGetError eglGetError,
                  fn_eglQueryString eglQueryString, fn_eglTerminate eglTerminate,
                  fn_eglGetProcAddress eglGetProcAddress)
{
    printf("%s\n", label);
    if (dpy == EGL_NO_DISPLAY) { printf("  FAIL -- EGL_NO_DISPLAY (0x%x)\n\n", eglGetError()); return; }

    EGLint major = 0, minor = 0;
    if (!eglInitialize(dpy, &major, &minor)) {
        printf("  eglInitialize FAIL (0x%x)\n\n", eglGetError());
        eglTerminate(dpy); return;
    }
    printf("  EGL vendor:  \"%s\"\n", eglQueryString(dpy, EGL_VENDOR));

    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint cfg_a[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                              EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE };
    EGLConfig cfg; EGLint n = 0;
    if (!eglChooseConfig(dpy, cfg_a, &cfg, 1, &n) || n == 0) {
        printf("  eglChooseConfig FAIL\n\n"); eglTerminate(dpy); return;
    }
    const EGLint ctx_a[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_a);
    if (ctx == EGL_NO_CONTEXT) {
        printf("  eglCreateContext FAIL (0x%x)\n\n", eglGetError());
        eglTerminate(dpy); return;
    }
    const EGLint surf_a[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, surf_a);
    if (surf != EGL_NO_SURFACE && eglMakeCurrent(dpy, surf, surf, ctx)) {
        fn_glGetString glGetString = (fn_glGetString)eglGetProcAddress("glGetString");
        if (glGetString) {
            printf("  GL vendor:   \"%s\"\n", (const char*)glGetString(GL_VENDOR));
            printf("  GL renderer: \"%s\"\n", (const char*)glGetString(GL_RENDERER));
        }
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    eglTerminate(dpy);
    printf("\n");
}

int main(void) {
    /* Connect to Wayland -- same connection SDL would give via SDL_GetWindowWMInfo
     * on a Wayland-native window (i.e. without SDL_VIDEODRIVER=x11) */
    void *wl_lib = dlopen("libwayland-client.so.0", RTLD_LAZY | RTLD_LOCAL);
    if (!wl_lib) { fprintf(stderr, "Cannot load libwayland-client: %s\n", dlerror()); return 1; }
    fn_wl_display_connect wl_display_connect = dlsym(wl_lib, "wl_display_connect");
    void *wl_dpy = wl_display_connect(NULL);
    if (!wl_dpy) { fprintf(stderr, "Cannot connect to Wayland display -- is WAYLAND_DISPLAY set?\n"); return 1; }

    void *lib = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "Cannot load libEGL.so.1: %s\n", dlerror()); return 1; }

    fn_eglGetDisplay         eglGetDisplay         = dlsym(lib, "eglGetDisplay");
    fn_eglInitialize         eglInitialize         = dlsym(lib, "eglInitialize");
    fn_eglBindAPI            eglBindAPI            = dlsym(lib, "eglBindAPI");
    fn_eglChooseConfig       eglChooseConfig       = dlsym(lib, "eglChooseConfig");
    fn_eglCreateContext      eglCreateContext      = dlsym(lib, "eglCreateContext");
    fn_eglCreatePbufferSurface eglCreatePbufferSurface = dlsym(lib, "eglCreatePbufferSurface");
    fn_eglMakeCurrent        eglMakeCurrent        = dlsym(lib, "eglMakeCurrent");
    fn_eglGetError           eglGetError           = dlsym(lib, "eglGetError");
    fn_eglQueryString        eglQueryString        = dlsym(lib, "eglQueryString");
    fn_eglTerminate          eglTerminate          = dlsym(lib, "eglTerminate");
    fn_eglGetProcAddress     eglGetProcAddress     = dlsym(lib, "eglGetProcAddress");

    fn_eglGetPlatformDisplay eglGetPlatformDisplay =
        (fn_eglGetPlatformDisplay)eglGetProcAddress("eglGetPlatformDisplay");
    if (!eglGetPlatformDisplay)
        eglGetPlatformDisplay =
            (fn_eglGetPlatformDisplay)eglGetProcAddress("eglGetPlatformDisplayEXT");

    /* TEST 1: the RS3 bug path */
    probe("=== TEST 1: eglGetDisplay(NULL) -- the RS3 bug ===",
          eglGetDisplay(EGL_DEFAULT_DISPLAY),
          eglInitialize, eglBindAPI, eglChooseConfig, eglCreateContext,
          eglCreatePbufferSurface, eglMakeCurrent, eglGetError,
          eglQueryString, eglTerminate, eglGetProcAddress);

    /* TEST 2: Option B fix -- use the wl_display SDL would provide on a Wayland window */
    if (eglGetPlatformDisplay) {
        probe("=== TEST 2: eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_EXT, wl_display) -- Option B fix ===",
              eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_EXT, wl_dpy, NULL),
              eglInitialize, eglBindAPI, eglChooseConfig, eglCreateContext,
              eglCreatePbufferSurface, eglMakeCurrent, eglGetError,
              eglQueryString, eglTerminate, eglGetProcAddress);
    } else {
        printf("eglGetPlatformDisplay not available\n");
    }

    dlclose(lib);
    dlclose(wl_lib);
    return 0;
}
