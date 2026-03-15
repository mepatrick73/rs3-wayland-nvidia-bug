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
 *   FIX:  eglGetPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, nvidia_device) → GPU
 *
 * The vendor ICD libraries (libEGL_mesa.so.0, libEGL_nvidia.so.0) do NOT export
 * standard egl* symbols — they use GLVND's internal dispatch table. You cannot
 * dlopen them and call eglGetDisplay directly. GLVND dispatches internally based
 * on which vendor's eglGetDisplay succeeds for the given display/platform.
 *
 * Root cause: EGL_DEFAULT_DISPLAY + X11 platform → GLVND routes to Mesa.
 * Mesa has no DRI2/DRI3 driver for nvidia.ko → silent fallback to llvmpipe.
 * Nvidia's EGL ICD requires an explicit Wayland or device platform handle.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

typedef void*          EGLDisplay;
typedef void*          EGLConfig;
typedef void*          EGLContext;
typedef void*          EGLSurface;
typedef void*          EGLDeviceEXT;
typedef int            EGLint;
typedef long           EGLAttrib;
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

/* EGL_EXT_device_enumeration + EGL_EXT_platform_device */
#define EGL_PLATFORM_DEVICE_EXT     0x313F
#define EGL_DRM_DEVICE_FILE_EXT     0x3233

typedef EGLDisplay   (*fn_eglGetDisplay)         (EGLNativeDisplayType);
typedef EGLDisplay   (*fn_eglGetPlatformDisplay) (EGLenum, void*, const EGLAttrib*);
typedef EGLBoolean   (*fn_eglInitialize)         (EGLDisplay, EGLint*, EGLint*);
typedef EGLBoolean   (*fn_eglBindAPI)            (EGLenum);
typedef EGLBoolean   (*fn_eglChooseConfig)       (EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*);
typedef EGLContext   (*fn_eglCreateContext)      (EGLDisplay, EGLConfig, EGLContext, const EGLint*);
typedef EGLSurface   (*fn_eglCreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint*);
typedef EGLBoolean   (*fn_eglMakeCurrent)        (EGLDisplay, EGLSurface, EGLSurface, EGLContext);
typedef EGLint       (*fn_eglGetError)           (void);
typedef const char*  (*fn_eglQueryString)        (EGLDisplay, EGLint);
typedef EGLBoolean   (*fn_eglTerminate)          (EGLDisplay);
typedef void*        (*fn_eglGetProcAddress)     (const char*);
typedef const unsigned char* (*fn_glGetString)   (unsigned int);

/* EGL_EXT_device_enumeration */
typedef EGLBoolean  (*fn_eglQueryDevicesEXT)     (EGLint, EGLDeviceEXT*, EGLint*);
typedef const char* (*fn_eglQueryDeviceStringEXT)(EGLDeviceEXT, EGLint);

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
    void* lib = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!lib) {
        fprintf(stderr, "Cannot load libEGL.so.1: %s\n", dlerror());
        return 1;
    }

    fn_eglGetDisplay         eglGetDisplay         = dlsym(lib, "eglGetDisplay");
    fn_eglGetPlatformDisplay eglGetPlatformDisplay = dlsym(lib, "eglGetPlatformDisplay");
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

    if (!eglGetDisplay || !eglInitialize || !eglBindAPI || !eglChooseConfig ||
        !eglCreateContext || !eglCreatePbufferSurface || !eglMakeCurrent ||
        !eglGetError || !eglQueryString || !eglTerminate || !eglGetProcAddress) {
        fprintf(stderr, "Missing required EGL symbols\n");
        dlclose(lib);
        return 1;
    }

    /* ------------------------------------------------------------------ */
    printf("=== TEST 1: eglGetDisplay(EGL_DEFAULT_DISPLAY) — the RS3 code path ===\n");
    printf("This is what RS3/SDL uses. With DISPLAY set (XWayland), GLVND picks\n");
    printf("the X11 platform and routes to Mesa, which has no DRI2 for nvidia.ko.\n\n");

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
    printf("=== TEST 2: eglQueryDevicesEXT — enumerate all EGL devices ===\n");
    printf("This lists every GPU/device GLVND knows about, bypassing platform auto-detection.\n\n");

    fn_eglQueryDevicesEXT eglQueryDevicesEXT =
        (fn_eglQueryDevicesEXT)eglGetProcAddress("eglQueryDevicesEXT");
    fn_eglQueryDeviceStringEXT eglQueryDeviceStringEXT =
        (fn_eglQueryDeviceStringEXT)eglGetProcAddress("eglQueryDeviceStringEXT");

    int nvidia_device_idx = -1;
    EGLDeviceEXT devices[32];
    EGLint num_devices = 0;

    if (!eglQueryDevicesEXT || !eglQueryDeviceStringEXT || !eglGetPlatformDisplay) {
        printf("  EGL_EXT_device_enumeration or eglGetPlatformDisplay not available\n");
        printf("  Cannot enumerate devices — Nvidia driver may not support this extension\n\n");
    } else if (!eglQueryDevicesEXT(32, devices, &num_devices) || num_devices == 0) {
        printf("  eglQueryDevicesEXT: no devices found (0x%x)\n\n", eglGetError());
    } else {
        printf("  Found %d EGL device(s):\n", num_devices);
        for (int i = 0; i < num_devices; i++) {
            const char* drm_dev = eglQueryDeviceStringEXT(devices[i], EGL_DRM_DEVICE_FILE_EXT);
            printf("  [%d] DRM device: %s\n", i, drm_dev ? drm_dev : "(none/not a DRM device)");
            if (drm_dev && nvidia_device_idx < 0) {
                /* pick first device with a DRM node — on single-GPU systems this is Nvidia */
                nvidia_device_idx = i;
            }
        }
        printf("\n");
    }

    /* ------------------------------------------------------------------ */
    printf("=== TEST 3: eglGetPlatformDisplay(EGL_PLATFORM_DEVICE_EXT) — the fix ===\n");
    printf("Explicitly targets a specific GPU device, bypassing the X11/Wayland platform\n");
    printf("auto-detection that routes GLVND to Mesa.\n\n");

    int result_device = 2;
    if (nvidia_device_idx < 0 || !eglGetPlatformDisplay) {
        printf("  SKIP — device enumeration unavailable or no devices found\n");
        printf("  (Nvidia driver < 435 may not support EGL_EXT_device_enumeration)\n\n");
    } else {
        EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_DEVICE_EXT,
                                                devices[nvidia_device_idx], NULL);
        if (dpy == EGL_NO_DISPLAY) {
            printf("  eglGetPlatformDisplay: FAIL (0x%x)\n\n", eglGetError());
        } else {
            printf("  eglGetPlatformDisplay: OK (device %d)\n", nvidia_device_idx);
            result_device = test_display(dpy,
                eglInitialize, eglBindAPI, eglChooseConfig,
                eglCreateContext, eglCreatePbufferSurface, eglMakeCurrent,
                eglGetError, eglQueryString, eglTerminate, eglGetProcAddress);
        }
    }
    printf("RESULT: %s\n\n", result_label(result_device));

    /* ------------------------------------------------------------------ */
    printf("=== Summary ===\n");
    printf("  TEST 1 — eglGetDisplay(EGL_DEFAULT_DISPLAY) [RS3 code path]: %s\n", result_label(result_default));
    printf("  TEST 3 — eglGetPlatformDisplay(DEVICE_EXT)  [proper fix]:    %s\n", result_label(result_device));
    printf("\n");

    if (result_default == 1 && result_device == 0) {
        printf("BUG CONFIRMED: Default EGL dispatch silently falls back to llvmpipe.\n");
        printf("               Explicit device selection gives hardware rendering.\n");
        printf("\n");
        printf("WHY: eglGetDisplay(EGL_DEFAULT_DISPLAY) with DISPLAY set → X11 platform\n");
        printf("     → GLVND routes to Mesa (handles X11) → Mesa has no DRI2 for nvidia.ko\n");
        printf("     → Mesa creates llvmpipe context silently, game appears to run but uses CPU\n");
        printf("\n");
        printf("FIX: RS3's SDL/EGL init should use eglGetPlatformDisplay with an explicit\n");
        printf("     Wayland or device handle, not EGL_DEFAULT_DISPLAY. Or: ensure\n");
        printf("     SDL_VIDEODRIVER is NOT forced to x11 so SDL uses the Wayland backend,\n");
        printf("     which calls eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, wl_display).\n");
    } else if (result_default == 0) {
        printf("Default EGL already works on this system — bug may not be present.\n");
    } else {
        printf("Check /usr/share/glvnd/egl_vendor.d/ for Nvidia ICD registration.\n");
        printf("Ensure nvidia-open or nvidia driver package is fully installed.\n");
    }

    dlclose(lib);
    return 0;
}
