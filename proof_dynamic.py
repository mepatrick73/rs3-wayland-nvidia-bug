# proof_dynamic.py — GDB Python script to prove RS3/llvmpipe claims at runtime
#
# Proves:
#   1. rs2client calls eglGetDisplay(EGL_DEFAULT_DISPLAY) — arg is NULL
#   2. The EGL display is owned by Mesa, not Nvidia
#   3. The GL renderer is llvmpipe (software)
#   4. The native window passed to eglCreateWindowSurface matches
#      SDL_GetWindowWMInfo's x11.window field
#
# Usage (see proof_run.sh):
#   gdb -x proof_dynamic.py --args /path/to/rs2client [--configURI ...]

import gdb

results = {}

# EGL constants
EGL_VENDOR  = 0x3054
EGL_VERSION = 0x3053
EGL_TRUE    = 1

# GL constants
GL_VENDOR   = 0x1F00
GL_RENDERER = 0x1F01

# SDL2 SDL_SYSWM_TYPE enum value for X11
SDL_SYSWM_X11 = 2

# Byte offsets in SDL_SysWMinfo on 64-bit Linux (SDL2):
#   struct SDL_version { uint8 major, minor, patch; };  // 3 bytes
#   1 byte padding
#   SDL_SYSWM_TYPE subsystem;                            // 4 bytes  @ offset 4
#   union { struct { Display *display; Window window; } x11; } info;
#                                                        // ptr @ offset 8, Window @ offset 16
WMINFO_SUBSYSTEM_OFFSET   = 4
WMINFO_X11_DISPLAY_OFFSET = 8
WMINFO_X11_WINDOW_OFFSET  = 16


# ── Claim 1: eglGetDisplay(NULL) ─────────────────────────────────────────────

class EglGetDisplayBP(gdb.Breakpoint):
    def __init__(self):
        super().__init__("eglGetDisplay", gdb.BP_BREAKPOINT, internal=False)
        self.silent = True
        self.hit = False

    def stop(self):
        if self.hit:
            return False
        self.hit = True
        arg = int(gdb.parse_and_eval("(unsigned long long)$rdi"))
        results['eglGetDisplay_arg'] = arg
        verdict = "EGL_DEFAULT_DISPLAY (NULL) — BUG CONFIRMED" if arg == 0 else f"0x{arg:x} (not NULL)"
        print(f"\n[PROOF 1] eglGetDisplay(native_display=0x{arg:x})")
        print(f"          {verdict}")
        return False


# ── Claim 2: EGL vendor is Mesa ───────────────────────────────────────────────

class EglInitializeBP(gdb.Breakpoint):
    def __init__(self):
        super().__init__("eglInitialize", gdb.BP_BREAKPOINT, internal=False)
        self.silent = True
        self.hit = False

    def stop(self):
        if self.hit:
            return False
        self.hit = True
        display = int(gdb.parse_and_eval("(unsigned long long)$rdi"))
        results['egl_display'] = display
        try:
            _EglInitializeFinishBP(display)
        except Exception as e:
            print(f"[PROOF 2] Could not install finish breakpoint: {e}")
        return False


class _EglInitializeFinishBP(gdb.FinishBreakpoint):
    def __init__(self, display):
        super().__init__(gdb.newest_frame(), internal=True)
        self.silent = True
        self.display = display

    def stop(self):
        ret = int(gdb.parse_and_eval("(int)$rax"))
        if ret != EGL_TRUE:
            print(f"\n[PROOF 2] eglInitialize returned {ret} (FAILED)")
            return False
        try:
            vendor  = gdb.parse_and_eval(f"eglQueryString((void*){self.display:#x}, {EGL_VENDOR})").string()
            version = gdb.parse_and_eval(f"eglQueryString((void*){self.display:#x}, {EGL_VERSION})").string()
            results['egl_vendor']  = vendor
            results['egl_version'] = version
            verdict = "Mesa — BUG CONFIRMED" if "Mesa" in vendor else f"unexpected vendor"
            print(f"\n[PROOF 2] eglQueryString(EGL_VENDOR)  = \"{vendor}\"")
            print(f"[PROOF 2] eglQueryString(EGL_VERSION) = \"{version}\"")
            print(f"          {verdict}")
        except Exception as e:
            print(f"\n[PROOF 2] eglQueryString call failed: {e}")
        return False


# ── Claim 3: GL renderer is llvmpipe ─────────────────────────────────────────

class GlGetStringBP(gdb.Breakpoint):
    def __init__(self):
        super().__init__("glGetString", gdb.BP_BREAKPOINT, internal=False)
        self.silent = True
        self.got_renderer = False
        self.got_vendor   = False

    def stop(self):
        name = int(gdb.parse_and_eval("(unsigned int)$rdi"))
        if name == GL_RENDERER and not self.got_renderer:
            self.got_renderer = True
            try:
                _GlGetStringFinishBP("GL_RENDERER")
            except Exception as e:
                print(f"[PROOF 3] Could not install finish breakpoint: {e}")
        elif name == GL_VENDOR and not self.got_vendor:
            self.got_vendor = True
            try:
                _GlGetStringFinishBP("GL_VENDOR")
            except Exception as e:
                print(f"[PROOF 3] Could not install finish breakpoint: {e}")
        if self.got_renderer and self.got_vendor:
            self.enabled = False
        return False


class _GlGetStringFinishBP(gdb.FinishBreakpoint):
    def __init__(self, label):
        super().__init__(gdb.newest_frame(), internal=True)
        self.silent = True
        self.label = label

    def stop(self):
        try:
            ptr = int(gdb.parse_and_eval("(unsigned long long)$rax"))
            if ptr:
                s = gdb.parse_and_eval(f"(const char *){ptr:#x}").string()
                results[f'gl_{self.label}'] = s
                print(f"\n[PROOF 3] glGetString({self.label}) = \"{s}\"")
                if self.label == "GL_RENDERER":
                    verdict = "llvmpipe — BUG CONFIRMED (software rendering)" if "llvmpipe" in s else s
                    print(f"          {verdict}")
        except Exception as e:
            print(f"[PROOF 3] glGetString({self.label}) read failed: {e}")
        return False


# ── Claim 4: native window comes from SDL_GetWindowWMInfo ────────────────────

class SDLGetWindowWMInfoBP(gdb.Breakpoint):
    def __init__(self):
        super().__init__("SDL_GetWindowWMInfo", gdb.BP_BREAKPOINT, internal=False)
        self.silent = True
        self.hit = False

    def stop(self):
        if self.hit:
            return False
        self.hit = True
        wminfo_ptr = int(gdb.parse_and_eval("(unsigned long long)$rsi"))
        try:
            _SDLGetWindowWMInfoFinishBP(wminfo_ptr)
        except Exception as e:
            print(f"[PROOF 4a] Could not install finish breakpoint: {e}")
        return False


class _SDLGetWindowWMInfoFinishBP(gdb.FinishBreakpoint):
    def __init__(self, wminfo_ptr):
        super().__init__(gdb.newest_frame(), internal=True)
        self.silent = True
        self.wminfo_ptr = wminfo_ptr

    def stop(self):
        try:
            subsystem = int(gdb.parse_and_eval(
                f"*(unsigned int *)({self.wminfo_ptr:#x} + {WMINFO_SUBSYSTEM_OFFSET})"))
            display   = int(gdb.parse_and_eval(
                f"*(unsigned long *)({self.wminfo_ptr:#x} + {WMINFO_X11_DISPLAY_OFFSET})"))
            window    = int(gdb.parse_and_eval(
                f"*(unsigned long *)({self.wminfo_ptr:#x} + {WMINFO_X11_WINDOW_OFFSET})"))
            results['sdl_subsystem']    = subsystem
            results['sdl_x11_display']  = display
            results['sdl_x11_window']   = window
            subsys_name = "X11" if subsystem == SDL_SYSWM_X11 else f"unknown ({subsystem})"
            print(f"\n[PROOF 4a] SDL_GetWindowWMInfo returned:")
            print(f"           subsystem   = {subsystem} ({subsys_name})")
            print(f"           x11.display = 0x{display:x}")
            print(f"           x11.window  = 0x{window:x}")
        except Exception as e:
            print(f"[PROOF 4a] Failed to read SDL_SysWMinfo struct: {e}")
        return False


class EglCreateWindowSurfaceBP(gdb.Breakpoint):
    def __init__(self):
        super().__init__("eglCreateWindowSurface", gdb.BP_BREAKPOINT, internal=False)
        self.silent = True
        self.hit = False

    def stop(self):
        if self.hit:
            return False
        self.hit = True
        display    = int(gdb.parse_and_eval("(unsigned long long)$rdi"))
        native_win = int(gdb.parse_and_eval("(unsigned long long)$rdx"))
        results['eglCreateWindowSurface_win'] = native_win
        print(f"\n[PROOF 4b] eglCreateWindowSurface:")
        print(f"           display    = 0x{display:x}")
        print(f"           native_win = 0x{native_win:x}")
        sdl_win = results.get('sdl_x11_window')
        if sdl_win is not None:
            if native_win == sdl_win:
                print(f"           MATCH — native_win == SDL_GetWindowWMInfo x11.window ✓")
            else:
                print(f"           MISMATCH — SDL reported 0x{sdl_win:x}")
        else:
            print(f"           SDL_GetWindowWMInfo not yet seen (may fire after this)")
        return False


# ── Summary on exit ───────────────────────────────────────────────────────────

def _print_summary(event):
    print("\n" + "=" * 60)
    print("PROOF SUMMARY")
    print("=" * 60)

    arg = results.get('eglGetDisplay_arg')
    if arg is not None:
        print(f"1. eglGetDisplay arg     : 0x{arg:x}{' (NULL = EGL_DEFAULT_DISPLAY)' if arg == 0 else ''}")

    vendor = results.get('egl_vendor')
    if vendor:
        print(f"2. EGL_VENDOR            : {vendor}")

    renderer = results.get('gl_GL_RENDERER')
    if renderer:
        print(f"3. GL_RENDERER           : {renderer}")

    sdl_win = results.get('sdl_x11_window')
    egl_win = results.get('eglCreateWindowSurface_win')
    if sdl_win is not None:
        print(f"4. SDL x11.window        : 0x{sdl_win:x}")
    if egl_win is not None:
        print(f"   eglCreateWindowSurface: 0x{egl_win:x}")
    if sdl_win is not None and egl_win is not None:
        print(f"   Match                 : {'YES' if sdl_win == egl_win else 'NO'}")

    print("=" * 60)

gdb.events.exited.connect(_print_summary)


# ── Install everything ────────────────────────────────────────────────────────

print("[*] Installing proof breakpoints...")
EglGetDisplayBP()
EglInitializeBP()
GlGetStringBP()
SDLGetWindowWMInfoBP()
EglCreateWindowSurfaceBP()
print("[*] Done. Run 'run' to start (or use proof_run.sh).\n")
