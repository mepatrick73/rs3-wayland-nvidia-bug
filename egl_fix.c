/*
 * egl_fix.c — LD_PRELOAD interceptor for RS3's EGL initialisation on Nvidia+Wayland
 *
 * Problem:
 *   RS3 calls eglGetDisplay(x11_display) or eglGetDisplay(EGL_DEFAULT_DISPLAY).
 *   GLVND routes both to Mesa because Mesa claims the X11 platform.
 *   Mesa has no DRI2 driver for nvidia.ko and silently falls back to llvmpipe.
 *
 * Fix:
 *   Intercept eglGetDisplay and replace with
 *   eglGetPlatformDisplay(EGL_PLATFORM_X11_KHR, x11_display, NULL).
 *   GLVND dispatches platform-explicit calls in vendor priority order;
 *   Nvidia (10_nvidia.json, priority 10) beats Mesa (50_mesa.json, priority 50).
 *
 *   For the EGL_DEFAULT_DISPLAY case we need a real X11 Display* — we capture it
 *   by intercepting XOpenDisplay so we always use the same connection as RS3's
 *   windows (a mismatch would crash eglCreateWindowSurface).
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>

typedef void*        EGLDisplay;
typedef void*        EGLNativeDisplayType;
typedef unsigned int EGLenum;
typedef long         EGLAttrib;

#define EGL_NO_DISPLAY       ((EGLDisplay)0)
#define EGL_DEFAULT_DISPLAY  ((EGLNativeDisplayType)0)
#define EGL_PLATFORM_X11_KHR 0x31D5

typedef EGLDisplay (*fn_eglGetDisplay)        (EGLNativeDisplayType);
typedef EGLDisplay (*fn_eglGetPlatformDisplay)(EGLenum, void*, const EGLAttrib*);
typedef void*      (*fn_XOpenDisplay)         (const char*);

/* First X11 Display* opened in this process — captured from XOpenDisplay */
static _Atomic(void*) s_x11_display = (void*)0;

void* XOpenDisplay(const char* display_name) {
    fn_XOpenDisplay real = (fn_XOpenDisplay)dlsym(RTLD_NEXT, "XOpenDisplay");
    if (!real) return NULL;
    void* dpy = real(display_name);
    /* Store the first successfully opened display for use in eglGetDisplay */
    void* expected = NULL;
    atomic_compare_exchange_strong(&s_x11_display, &expected, dpy);
    return dpy;
}

EGLDisplay eglGetDisplay(EGLNativeDisplayType native_display) {
    fn_eglGetDisplay         real_eglGetDisplay         = dlsym(RTLD_NEXT, "eglGetDisplay");
    fn_eglGetPlatformDisplay real_eglGetPlatformDisplay = dlsym(RTLD_NEXT, "eglGetPlatformDisplay");

    if (!real_eglGetPlatformDisplay) {
        fprintf(stderr, "[Bolt] eglGetDisplay: eglGetPlatformDisplay unavailable, passing through\n");
        if (real_eglGetDisplay) return real_eglGetDisplay(native_display);
        return EGL_NO_DISPLAY;
    }

    void* x11_display = (void*)native_display;

    if (native_display == EGL_DEFAULT_DISPLAY) {
        /* Use the X11 connection RS3 already opened, so window surfaces match */
        x11_display = atomic_load(&s_x11_display);
        fprintf(stderr, "[Bolt] eglGetDisplay(DEFAULT) — captured x11_display=%p\n", x11_display);
    } else {
        fprintf(stderr, "[Bolt] eglGetDisplay(x11=%p)\n", x11_display);
    }

    if (x11_display) {
        EGLDisplay dpy = real_eglGetPlatformDisplay(EGL_PLATFORM_X11_KHR, x11_display, NULL);
        if (dpy != EGL_NO_DISPLAY) {
            fprintf(stderr, "[Bolt] eglGetDisplay: redirected to EGL_PLATFORM_X11_KHR — dpy=%p\n", dpy);
            return dpy;
        }
        fprintf(stderr, "[Bolt] eglGetDisplay: EGL_PLATFORM_X11_KHR returned NO_DISPLAY\n");
    } else {
        fprintf(stderr, "[Bolt] eglGetDisplay: no x11_display captured yet, falling back\n");
    }

    if (real_eglGetDisplay) return real_eglGetDisplay(native_display);
    return EGL_NO_DISPLAY;
}
