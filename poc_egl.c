/*
 * poc_egl.c - Demonstrates the Mesa vs Nvidia EGL dispatch bug on Nvidia+Wayland
 *
 * Compile:
 *   gcc poc_egl.c -o poc_egl -ldl
 *
 * Run:
 *   ./poc_egl
 *
 * Tests:
 *   1. eglGetDisplay(NULL)                              — RS3 code path, expect llvmpipe
 *   2. eglGetPlatformDisplayEXT + pbuffer               — explicit platform, no window
 *   3. EGL_NATIVE_VISUAL_ID + XCreateWindow + SDL_CreateWindowFrom — full correct fix:
 *                                                         query visual from EGL first, create
 *                                                         Xlib window with matching visual,
 *                                                         wrap with SDL, create window surface
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
#define EGL_WINDOW_BIT              0x0004
#define EGL_SURFACE_TYPE            0x3033
#define EGL_WIDTH                   0x3057
#define EGL_HEIGHT                  0x3056
#define EGL_VENDOR                  0x3053
#define EGL_NONE                    0x3038
#define EGL_BAD_MATCH               0x3009
#define GL_RENDERER                 0x1F01
#define GL_VENDOR                   0x1F00

#define EGL_PLATFORM_X11_EXT        0x31D5
#define EGL_NATIVE_VISUAL_ID        0x302E

typedef void XDisplay;

/* SDL2 types — minimal subset needed */
#define SDL_INIT_VIDEO              0x00000020u
#define SDL_WINDOW_SHOWN            0x00000004u
#define SDL_WINDOWPOS_UNDEFINED     0x1FFF0000
#define SDL_SYSWM_X11               2
/* X11 types for Xlib window creation (Test 4) */
typedef unsigned long XWindow;
typedef unsigned long XColormap;

typedef struct {
    void*         visual;
    unsigned long visualid;
    int           screen;
    int           depth;
    int           class;
    unsigned long red_mask, green_mask, blue_mask;
    int           colormap_size, bits_per_rgb;
} XVisualInfo;

typedef struct {
    unsigned long background_pixmap, background_pixel;
    unsigned long border_pixmap,     border_pixel;
    int           bit_gravity,       win_gravity, backing_store;
    unsigned long backing_planes,    backing_pixel;
    int           save_under;
    long          event_mask,        do_not_propagate_mask;
    int           override_redirect;
    unsigned long colormap, cursor;
} XSetWindowAttributes;

#define VisualIDMask    0x1L
#define CWBorderPixel   (1L<<3)
#define CWColormap      (1L<<13)
#define InputOutput     1
#define AllocNone       0

typedef int          (*fn_XDefaultScreen)  (XDisplay*);
typedef XWindow      (*fn_XRootWindow)     (XDisplay*, int);
typedef XVisualInfo* (*fn_XGetVisualInfo)  (XDisplay*, long, XVisualInfo*, int*);
typedef XColormap    (*fn_XCreateColormap) (XDisplay*, XWindow, void*, int);
typedef XWindow      (*fn_XCreateWindow)   (XDisplay*, XWindow, int, int,
                                            unsigned int, unsigned int, unsigned int,
                                            int, unsigned int, void*,
                                            unsigned long, XSetWindowAttributes*);
typedef int          (*fn_XDestroyWindow)  (XDisplay*, XWindow);
typedef int          (*fn_XFreeColormap)   (XDisplay*, XColormap);
typedef int          (*fn_XSync)           (XDisplay*, int);
typedef int          (*fn_XFree)           (void*);

typedef void SDL_Window;

/*
 * SDL_SysWMinfo layout on 64-bit Linux (SDL2):
 *   offset 0: version (3 bytes: major, minor, patch)
 *   offset 4: subsystem (int)
 *   offset 8: info union — x11.display (Display*, 8 bytes), x11.window (Window/ulong, 8 bytes)
 */
typedef struct {
    unsigned char major;
    unsigned char minor;
    unsigned char patch;
    int           subsystem;
    union {
        struct {
            void*         display;
            unsigned long window;
        } x11;
        unsigned char dummy[64];
    } info;
} SDL_SysWMinfo;

typedef int         (*fn_SDL_Init)             (unsigned int);
typedef SDL_Window* (*fn_SDL_CreateWindow)     (const char*, int, int, int, int, unsigned int);
typedef int         (*fn_SDL_GetWindowWMInfo)  (SDL_Window*, SDL_SysWMinfo*);
typedef void        (*fn_SDL_GetVersion)       (unsigned char*, unsigned char*, unsigned char*);
typedef SDL_Window* (*fn_SDL_CreateWindowFrom) (const void* native_handle);
typedef void        (*fn_SDL_DestroyWindow)    (SDL_Window*);
typedef void        (*fn_SDL_Quit)             (void);

typedef EGLDisplay   (*fn_eglGetDisplay)            (EGLNativeDisplayType);
typedef EGLDisplay   (*fn_eglGetPlatformDisplayEXT) (EGLenum, void*, const EGLint*);
typedef EGLBoolean   (*fn_eglInitialize)            (EGLDisplay, EGLint*, EGLint*);
typedef EGLBoolean   (*fn_eglBindAPI)               (EGLenum);
typedef EGLBoolean   (*fn_eglChooseConfig)          (EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*);
typedef EGLBoolean   (*fn_eglGetConfigAttrib)       (EGLDisplay, EGLConfig, EGLint, EGLint*);
typedef EGLContext   (*fn_eglCreateContext)         (EGLDisplay, EGLConfig, EGLContext, const EGLint*);
typedef EGLSurface   (*fn_eglCreatePbufferSurface)  (EGLDisplay, EGLConfig, const EGLint*);
typedef EGLSurface   (*fn_eglCreateWindowSurface)   (EGLDisplay, EGLConfig, unsigned long, const EGLint*);
typedef EGLBoolean   (*fn_eglMakeCurrent)           (EGLDisplay, EGLSurface, EGLSurface, EGLContext);
typedef EGLint       (*fn_eglGetError)              (void);
typedef const char*  (*fn_eglQueryString)           (EGLDisplay, EGLint);
typedef EGLBoolean   (*fn_eglTerminate)             (EGLDisplay);
typedef void*        (*fn_eglGetProcAddress)        (const char*);
typedef const unsigned char* (*fn_glGetString)      (unsigned int);

typedef XDisplay*    (*fn_XOpenDisplay)             (const char*);
typedef int          (*fn_XCloseDisplay)            (XDisplay*);

static int classify_renderer(const char* r) {
    if (!r) return 2;
    if (strstr(r, "llvmpipe") || strstr(r, "softpipe") || strstr(r, "Software"))
        return 1;
    return 0;
}

static const char* result_label(int r) {
    if (r == 0) return "PASS — hardware GPU";
    if (r == 1) return "SOFTWARE FALLBACK — llvmpipe (CPU, not GPU)";
    return "FAIL — no context";
}

/* Test one EGLDisplay with pbuffer. Returns 0=PASS, 1=SOFTWARE, 2=FAIL */
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
    printf("  eglInitialize: OK — EGL %d.%d\n", major, minor);
    printf("  EGL vendor:    \"%s\"\n", eglQueryString(dpy, EGL_VENDOR));

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

    const EGLint surf_attrs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, surf_attrs);
    if (surf == EGL_NO_SURFACE || !eglMakeCurrent(dpy, surf, surf, ctx)) {
        printf("  eglMakeCurrent: FAIL (0x%x)\n", eglGetError());
        eglTerminate(dpy);
        return 2;
    }

    fn_glGetString glGetString = (fn_glGetString)eglGetProcAddress("glGetString");
    int result = 2;
    if (glGetString) {
        const char* gl_v = (const char*)glGetString(GL_VENDOR);
        const char* gl_r = (const char*)glGetString(GL_RENDERER);
        printf("  GL vendor:   \"%s\"\n", gl_v ? gl_v : "(null)");
        printf("  GL renderer: \"%s\"\n", gl_r ? gl_r : "(null)");
        result = classify_renderer(gl_r);
    }
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglTerminate(dpy);
    return result;
}

int main(void) {
    void* egl_lib = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!egl_lib) { fprintf(stderr, "Cannot load libEGL.so.1: %s\n", dlerror()); return 1; }

    void* x11_lib = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
    if (!x11_lib) { fprintf(stderr, "Cannot load libX11.so.6: %s\n", dlerror()); dlclose(egl_lib); return 1; }

    void* sdl_lib = dlopen("libSDL2-2.0.so.0", RTLD_LAZY | RTLD_LOCAL);
    if (!sdl_lib) { fprintf(stderr, "Cannot load libSDL2-2.0.so.0: %s\n", dlerror()); }

    fn_eglGetDisplay           eglGetDisplay           = dlsym(egl_lib, "eglGetDisplay");
    fn_eglInitialize           eglInitialize           = dlsym(egl_lib, "eglInitialize");
    fn_eglBindAPI              eglBindAPI              = dlsym(egl_lib, "eglBindAPI");
    fn_eglChooseConfig         eglChooseConfig         = dlsym(egl_lib, "eglChooseConfig");
    fn_eglGetConfigAttrib      eglGetConfigAttrib      = dlsym(egl_lib, "eglGetConfigAttrib");
    fn_eglCreateContext        eglCreateContext        = dlsym(egl_lib, "eglCreateContext");
    fn_eglCreatePbufferSurface eglCreatePbufferSurface = dlsym(egl_lib, "eglCreatePbufferSurface");
    fn_eglCreateWindowSurface  eglCreateWindowSurface  = dlsym(egl_lib, "eglCreateWindowSurface");
    fn_eglMakeCurrent          eglMakeCurrent          = dlsym(egl_lib, "eglMakeCurrent");
    fn_eglGetError             eglGetError             = dlsym(egl_lib, "eglGetError");
    fn_eglQueryString          eglQueryString          = dlsym(egl_lib, "eglQueryString");
    fn_eglTerminate            eglTerminate            = dlsym(egl_lib, "eglTerminate");
    fn_eglGetProcAddress       eglGetProcAddress       = dlsym(egl_lib, "eglGetProcAddress");

    fn_XOpenDisplay    XOpenDisplay    = dlsym(x11_lib, "XOpenDisplay");
    fn_XCloseDisplay   XCloseDisplay   = dlsym(x11_lib, "XCloseDisplay");
    fn_XDefaultScreen  XDefaultScreen  = dlsym(x11_lib, "XDefaultScreen");
    fn_XRootWindow     XRootWindow     = dlsym(x11_lib, "XRootWindow");
    fn_XGetVisualInfo  XGetVisualInfo  = dlsym(x11_lib, "XGetVisualInfo");
    fn_XCreateColormap XCreateColormap = dlsym(x11_lib, "XCreateColormap");
    fn_XCreateWindow   XCreateWindow   = dlsym(x11_lib, "XCreateWindow");
    fn_XDestroyWindow  XDestroyWindow  = dlsym(x11_lib, "XDestroyWindow");
    fn_XFreeColormap   XFreeColormap   = dlsym(x11_lib, "XFreeColormap");
    fn_XSync           XSync           = dlsym(x11_lib, "XSync");
    fn_XFree           XFree           = dlsym(x11_lib, "XFree");

    fn_SDL_Init             SDL_Init             = sdl_lib ? dlsym(sdl_lib, "SDL_Init")             : NULL;
    fn_SDL_CreateWindow     SDL_CreateWindow     = sdl_lib ? dlsym(sdl_lib, "SDL_CreateWindow")     : NULL;
    fn_SDL_CreateWindowFrom SDL_CreateWindowFrom = sdl_lib ? dlsym(sdl_lib, "SDL_CreateWindowFrom") : NULL;
    fn_SDL_GetWindowWMInfo  SDL_GetWindowWMInfo  = sdl_lib ? dlsym(sdl_lib, "SDL_GetWindowWMInfo")  : NULL;
    fn_SDL_GetVersion       SDL_GetVersion       = sdl_lib ? dlsym(sdl_lib, "SDL_GetVersion")       : NULL;
    fn_SDL_DestroyWindow    SDL_DestroyWindow    = sdl_lib ? dlsym(sdl_lib, "SDL_DestroyWindow")    : NULL;
    fn_SDL_Quit             SDL_Quit             = sdl_lib ? dlsym(sdl_lib, "SDL_Quit")             : NULL;

    if (!eglGetDisplay || !eglInitialize || !eglBindAPI || !eglChooseConfig ||
        !eglCreateContext || !eglCreatePbufferSurface || !eglMakeCurrent ||
        !eglGetError || !eglQueryString || !eglTerminate || !eglGetProcAddress) {
        fprintf(stderr, "Missing required EGL symbols\n");
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
    printf("=== TEST 2: eglGetPlatformDisplayEXT(EGL_PLATFORM_X11_EXT) + pbuffer ===\n");
    printf("Explicit platform, GLVND routes by vendor priority: Nvidia (10) beats Mesa (50).\n");
    printf("Pbuffer surface — no window needed, proves GPU path works offscreen.\n\n");

    int result_pbuffer = 2;
    if (!eglGetPlatformDisplayEXT) {
        printf("  SKIP — eglGetPlatformDisplayEXT not available\n\n");
    } else {
        XDisplay* x11_dpy = XOpenDisplay(NULL);
        if (!x11_dpy) {
            printf("  SKIP — XOpenDisplay(NULL) failed\n\n");
        } else {
            EGLDisplay dpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_X11_EXT, x11_dpy, NULL);
            if (dpy == EGL_NO_DISPLAY) {
                printf("  eglGetPlatformDisplayEXT: FAIL (0x%x)\n\n", eglGetError());
            } else {
                printf("  eglGetPlatformDisplayEXT: OK\n");
                result_pbuffer = test_display(dpy,
                    eglInitialize, eglBindAPI, eglChooseConfig,
                    eglCreateContext, eglCreatePbufferSurface, eglMakeCurrent,
                    eglGetError, eglQueryString, eglTerminate, eglGetProcAddress);
            }
            XCloseDisplay(x11_dpy);
        }
    }
    printf("RESULT: %s\n\n", result_label(result_pbuffer));

    /* ------------------------------------------------------------------ */
    printf("=== TEST 3: full fix — EGL_NATIVE_VISUAL_ID + XCreateWindow + SDL_CreateWindowFrom ===\n");
    printf("1. Query EGL_NATIVE_VISUAL_ID from Nvidia config\n");
    printf("2. XCreateWindow with that visual\n");
    printf("3. SDL_CreateWindowFrom to wrap it (SDL handles events as normal)\n");
    printf("4. eglCreateWindowSurface — visual matches, should succeed\n\n");

    int result_window = 2;
    if (!eglGetPlatformDisplayEXT || !eglGetConfigAttrib || !eglCreateWindowSurface ||
        !XDefaultScreen || !XRootWindow || !XGetVisualInfo ||
        !XCreateColormap || !XCreateWindow || !XDestroyWindow || !XFreeColormap || !XFree) {
        printf("  SKIP — missing required Xlib/EGL symbols\n\n");
    } else if (!sdl_lib || !SDL_Init || !SDL_CreateWindowFrom || !SDL_DestroyWindow || !SDL_Quit) {
        printf("  SKIP — SDL2 not available\n\n");
    } else {
        XDisplay* x11_dpy4 = XOpenDisplay(NULL);
        if (!x11_dpy4) {
            printf("  SKIP — XOpenDisplay failed\n\n");
        } else {
            /* Step 1: get Nvidia EGL display */
            EGLDisplay dpy4 = eglGetPlatformDisplayEXT(EGL_PLATFORM_X11_EXT, x11_dpy4, NULL);
            if (dpy4 == EGL_NO_DISPLAY) {
                printf("  eglGetPlatformDisplayEXT: FAIL (0x%x)\n\n", eglGetError());
            } else {
                printf("  eglGetPlatformDisplayEXT: OK\n");
                EGLint maj4 = 0, min4 = 0;
                eglInitialize(dpy4, &maj4, &min4);
                printf("  eglInitialize: OK — EGL %d.%d, vendor: \"%s\"\n",
                       maj4, min4, eglQueryString(dpy4, EGL_VENDOR));

                /* Step 2: choose config, read required visual ID */
                eglBindAPI(EGL_OPENGL_ES_API);
                const EGLint cfg_attrs4[] = {
                    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                    EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
                    EGL_NONE,
                };
                EGLConfig cfg4;
                EGLint ncfg4 = 0;
                if (!eglChooseConfig(dpy4, cfg_attrs4, &cfg4, 1, &ncfg4) || ncfg4 == 0) {
                    printf("  eglChooseConfig: FAIL (0x%x)\n", eglGetError());
                    eglTerminate(dpy4);
                } else {
                    EGLint visual_id = 0;
                    eglGetConfigAttrib(dpy4, cfg4, EGL_NATIVE_VISUAL_ID, &visual_id);
                    printf("  EGL_NATIVE_VISUAL_ID: 0x%x\n", visual_id);

                    /* Step 3: create Xlib window with the EGL-required visual */
                    int screen4 = XDefaultScreen(x11_dpy4);
                    XWindow root4 = XRootWindow(x11_dpy4, screen4);
                    XVisualInfo tmpl4 = {0};
                    tmpl4.visualid = (unsigned long)visual_id;
                    int nvi4 = 0;
                    XVisualInfo* vis4 = XGetVisualInfo(x11_dpy4, VisualIDMask, &tmpl4, &nvi4);
                    if (!vis4 || nvi4 == 0) {
                        printf("  XGetVisualInfo: FAIL — visual 0x%x not found\n", visual_id);
                        eglTerminate(dpy4);
                    } else {
                        printf("  XGetVisualInfo: OK — depth %d\n", vis4[0].depth);
                        XColormap cmap4 = XCreateColormap(x11_dpy4, root4, vis4[0].visual, AllocNone);
                        XSetWindowAttributes swa4 = {0};
                        swa4.colormap     = cmap4;
                        swa4.border_pixel = 0;
                        XWindow xwin4 = XCreateWindow(x11_dpy4, root4, 0, 0, 640, 480, 0,
                                                      vis4[0].depth, InputOutput, vis4[0].visual,
                                                      CWBorderPixel | CWColormap, &swa4);
                        XFree(vis4);
                        if (!xwin4) {
                            printf("  XCreateWindow: FAIL\n");
                            XFreeColormap(x11_dpy4, cmap4);
                            eglTerminate(dpy4);
                        } else {
                            printf("  XCreateWindow: OK — XID 0x%lx\n", xwin4);
                            /* Flush to server so the window exists before SDL's connection sees it */
                            if (XSync) XSync(x11_dpy4, 0);

                            /* Step 4: wrap the Xlib window in SDL */
                            SDL_Init(SDL_INIT_VIDEO);
                            SDL_Window* sdl_win4 = SDL_CreateWindowFrom((void*)xwin4);
                            if (!sdl_win4) {
                                printf("  SDL_CreateWindowFrom: FAIL\n");
                            } else {
                                printf("  SDL_CreateWindowFrom: OK\n");

                                /* Step 5: create EGL window surface — visual matches */
                                EGLSurface surf4 = eglCreateWindowSurface(dpy4, cfg4, xwin4, NULL);
                                if (surf4 == EGL_NO_SURFACE) {
                                    printf("  eglCreateWindowSurface: FAIL (0x%x)\n", eglGetError());
                                    eglTerminate(dpy4);
                                } else {
                                    printf("  eglCreateWindowSurface: OK\n");
                                    const EGLint ctx_attrs4[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
                                    EGLContext ctx4 = eglCreateContext(dpy4, cfg4, EGL_NO_CONTEXT, ctx_attrs4);
                                    if (ctx4 == EGL_NO_CONTEXT) {
                                        printf("  eglCreateContext: FAIL (0x%x)\n", eglGetError());
                                        eglTerminate(dpy4);
                                    } else if (!eglMakeCurrent(dpy4, surf4, surf4, ctx4)) {
                                        printf("  eglMakeCurrent: FAIL (0x%x)\n", eglGetError());
                                        eglTerminate(dpy4);
                                    } else {
                                        fn_glGetString glGetString4 =
                                            (fn_glGetString)eglGetProcAddress("glGetString");
                                        if (glGetString4) {
                                            const char* gl_v = (const char*)glGetString4(GL_VENDOR);
                                            const char* gl_r = (const char*)glGetString4(GL_RENDERER);
                                            printf("  GL vendor:   \"%s\"\n", gl_v ? gl_v : "(null)");
                                            printf("  GL renderer: \"%s\"\n", gl_r ? gl_r : "(null)");
                                            result_window = classify_renderer(gl_r);
                                        }
                                        eglMakeCurrent(dpy4, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                                        eglTerminate(dpy4);
                                    }
                                }
                                SDL_DestroyWindow(sdl_win4);
                            }
                            SDL_Quit();
                            XDestroyWindow(x11_dpy4, xwin4);
                            XFreeColormap(x11_dpy4, cmap4);
                        }
                    }
                }
            }
            XCloseDisplay(x11_dpy4);
        }
    }
    printf("RESULT: %s\n\n", result_label(result_window));

    /* ------------------------------------------------------------------ */
    printf("=== Summary ===\n");
    printf("  TEST 1 — eglGetDisplay(NULL)                                    [RS3 path]:      %s\n", result_label(result_default));
    printf("  TEST 2 — eglGetPlatformDisplayEXT + pbuffer                            [offscreen]: %s\n", result_label(result_pbuffer));
    printf("  TEST 3 — EGL_NATIVE_VISUAL_ID + XCreateWindow + SDL_CreateWindowFrom [full fix]:  %s\n", result_label(result_window));
    printf("\n");

    if (result_default == 1 && result_pbuffer == 0 && result_window == 0) {
        printf("BUG CONFIRMED and full fix verified:\n");
        printf("  Default EGL falls back to llvmpipe (Test 1).\n");
        printf("  eglGetPlatformDisplayEXT routes to Nvidia for pbuffer (Test 2).\n");
        printf("  EGL_NATIVE_VISUAL_ID + XCreateWindow + SDL_CreateWindowFrom gives\n");
        printf("  hardware rendering via a real window surface (Test 3).\n");
    }

    if (sdl_lib) dlclose(sdl_lib);
    dlclose(x11_lib);
    dlclose(egl_lib);
    return 0;
}
