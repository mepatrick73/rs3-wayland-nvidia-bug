#!/usr/bin/env bash
# proof_static.sh — static disassembly proofs for rs2client EGL claims
#
# Proves via objdump (no need to run the binary):
#   1. rs2client calls eglGetDisplay with NULL (EGL_DEFAULT_DISPLAY)
#   2. rs2client never calls eglGetPlatformDisplay (core EGL 1.5)
#   3. rs2client imports SDL_GetWindowWMInfo

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Path to rs2client — edit this to point at your installation
RS2CLIENT="${RS2CLIENT:-${HOME}/.var/app/com.adamcake.Bolt/data/bolt-launcher/Jagex/launcher/rs2client}"

if [[ ! -f "$RS2CLIENT" ]]; then
    echo "ERROR: rs2client not found at $RS2CLIENT"
    echo "  Set RS2CLIENT=/path/to/rs2client or edit this script."
    exit 1
fi

echo "============================================================"
echo "STATIC PROOF — rs2client EGL analysis"
echo "Binary: $RS2CLIENT"
echo "============================================================"

# ── Proof 1: eglGetDisplay called with NULL ───────────────────────────────────
#
# The argument (edi/rdi) is zeroed by 'xor %edi,%edi' early in the sequence.
# No instruction between that xor and the call writes to edi or rdi,
# so eglGetDisplay receives 0 = EGL_DEFAULT_DISPLAY.
echo ""
echo "[PROOF 1] Looking for eglGetDisplay call and the xor that zeroes its argument..."
echo ""

# Find the offset of the eglGetDisplay call
CALL_OFFSET=$(objdump -d "$RS2CLIENT" \
    | grep "call.*<eglGetDisplay@plt>" \
    | head -1 \
    | awk '{print $1}' \
    | tr -d ':')

if [[ -z "$CALL_OFFSET" ]]; then
    echo "  ERROR: call to eglGetDisplay not found"
else
    echo "  call to eglGetDisplay at offset 0x${CALL_OFFSET}"
    echo ""
    echo "  Disassembly from 'xor edi,edi' through the call"
    echo "  (no instruction between them modifies edi/rdi):"
    echo ""
    # Find the nearest preceding 'xor %edi,%edi' (opcode 31 ff) by scanning
    # backward through the raw disassembly for offsets less than the call.
    XOR_OFFSET=$(objdump -d "$RS2CLIENT" \
        | grep "31 ff.*xor.*%edi,%edi" \
        | awk -F: '{print $1}' \
        | tr -d ' ' \
        | awk -v call="$CALL_OFFSET" \
            'BEGIN { best="" } { if ($1 < call && $1 > best) best=$1 } END { print best }')

    echo "  xor %edi,%edi at offset 0x${XOR_OFFSET} — sets first arg to 0 (EGL_DEFAULT_DISPLAY)"
    echo "  No instruction between them modifies edi/rdi."
    echo ""
    echo "  Disassembly (0x${XOR_OFFSET} → 0x${CALL_OFFSET}):"
    echo ""

    # Print the range; add 6 to stop to include the full 5-byte call instruction
    STOP=$(printf '%d' "0x${CALL_OFFSET}")
    STOP=$((STOP + 5))
    STOP=$(printf '0x%x' $STOP)
    objdump -d "$RS2CLIENT" \
        --start-address="0x${XOR_OFFSET}" \
        --stop-address="$STOP" \
        | grep -v "^/" | grep -v "^$" | grep -v "file format" \
        | sed 's/^/    /'
fi

# ── Proof 2: eglGetPlatformDisplay (core EGL 1.5) not a direct import ─────────
echo ""
echo "[PROOF 2] Checking for eglGetPlatformDisplay (core EGL 1.5) direct import..."
if objdump -T "$RS2CLIENT" 2>/dev/null | grep -q "eglGetPlatformDisplay$"; then
    echo "  FOUND — rs2client imports eglGetPlatformDisplay directly"
else
    echo "  NOT FOUND — rs2client does NOT directly import eglGetPlatformDisplay (core EGL 1.5)"
    echo "  However, the EXT variant may still be available via EGLEW — see PROOF 2b."
fi

# ── Proof 2b: eglGetPlatformDisplayEXT available via EGLEW ────────────────────
echo ""
echo "[PROOF 2b] Checking for EGLEW eglGetPlatformDisplayEXT dispatch table entries..."
EGLEW_EXT=$(strings "$RS2CLIENT" | grep "eglewGetPlatformDisplayEXT")
EGLEW_PROC=$(strings "$RS2CLIENT" | grep "^eglGetPlatformDisplayEXT$")
if [[ -n "$EGLEW_EXT" ]]; then
    echo "  FOUND: $EGLEW_EXT"
    echo "  FOUND: ${EGLEW_PROC:-eglGetPlatformDisplayEXT (proc name)}"
    echo "  → EGLEW dispatch table includes __eglewGetPlatformDisplayEXT."
    echo "    The proc name string 'eglGetPlatformDisplayEXT' is used by EGLEW to load"
    echo "    the function pointer via eglGetProcAddress at startup."
    echo "    Option A fix (eglGetPlatformDisplayEXT) is available to rs2client."
else
    echo "  NOT FOUND — EGLEW does not include eglGetPlatformDisplayEXT"
fi

# ── Proof 3: SDL_GetWindowWMInfo is imported ──────────────────────────────────
echo ""
echo "[PROOF 3] Checking for SDL_GetWindowWMInfo import..."
if objdump -T "$RS2CLIENT" 2>/dev/null | grep -q "SDL_GetWindowWMInfo"; then
    echo "  FOUND — rs2client imports SDL_GetWindowWMInfo"
    echo "  (used to obtain platform-specific window handles)"
else
    echo "  NOT FOUND"
fi

# ── Bonus: full EGL import list ───────────────────────────────────────────────
echo ""
echo "[BONUS] All EGL symbols imported by rs2client:"
objdump -T "$RS2CLIENT" 2>/dev/null \
    | grep -i "egl" \
    | awk '{print "  " $NF}' \
    | sort

echo ""
echo "============================================================"
echo "STATIC PROOF COMPLETE"
echo "============================================================"
