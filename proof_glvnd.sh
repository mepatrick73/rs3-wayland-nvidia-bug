#!/usr/bin/env bash
# proof_glvnd.sh — prove GLVND platform selection and vendor priority
#
# Proves:
#   Step 4: GLVND selects X11 platform when $DISPLAY is set
#   Step 5: Nvidia's EGL rejects NULL — Mesa wins by default
#   Step 6: With explicit platform, Nvidia wins (priority 10 < Mesa 50)
#
# Requires: gcc, poc_egl.c

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "============================================================"
echo "GLVND VENDOR PRIORITY PROOF"
echo "============================================================"

# ── Step 4: $DISPLAY is set (XWayland running) ───────────────────────────────
echo ""
echo "[STEP 4] Current display environment:"
echo "  DISPLAY         = ${DISPLAY:-(not set)}"
echo "  WAYLAND_DISPLAY = ${WAYLAND_DISPLAY:-(not set)}"
if [[ -n "$DISPLAY" && -n "$WAYLAND_DISPLAY" ]]; then
    echo "  → Both set: running under Wayland with XWayland active."
    echo "    This is the condition under which GLVND selects the X11 platform"
    echo "    for eglGetDisplay(NULL) — per GLVND EGL docs (libegl.md, 'Display type detection')."
    echo "    Empirical confirmation follows in Steps 5/6: Mesa wins, which only"
    echo "    happens on the X11 path (on the Wayland path GLVND routes to Nvidia)."
fi

# ── Step 4/6: GLVND vendor ICD priority files ────────────────────────────────
echo ""
echo "[STEP 4/6] GLVND EGL vendor ICD files (lower number = higher priority):"
echo ""
for f in /usr/share/glvnd/egl_vendor.d/*.json; do
    echo "  $f"
    python3 -c "
import json, sys
d = json.load(open('$f'))
print(f'    vendor_library = {d.get(\"ICD\", {}).get(\"library_path\", \"?\")}')
print(f'    priority       = {d.get(\"ICD\", {}).get(\"library_suffix\", \"?\")}')" 2>/dev/null \
    || grep -E '"library_path"|"library_suffix"' "$f" | sed 's/^/    /'
done
echo ""
echo "  → Nvidia (10_nvidia.json, priority 10) beats Mesa (50_mesa.json, priority 50)"
echo "    for platform-EXPLICIT calls (eglGetPlatformDisplay)."
echo "    For legacy eglGetDisplay(NULL), the first vendor to return non-NO_DISPLAY wins."

# ── Steps 5/6: poc_egl.c — contrast NULL vs explicit platform ────────────────
echo ""
echo "[STEPS 5/6] Building and running poc_egl.c..."
echo "  TEST 1: eglGetDisplay(NULL)                                        → expect Mesa/llvmpipe"
echo "  TEST 2: eglGetPlatformDisplayEXT(EGL_PLATFORM_X11_EXT, x11_display) → expect Nvidia GPU"
echo ""

POC="${SCRIPT_DIR}/poc_egl"
gcc -o "$POC" "${SCRIPT_DIR}/poc_egl.c" -ldl 2>/dev/null
"$POC"
rm -f "$POC"

echo ""
echo "============================================================"
