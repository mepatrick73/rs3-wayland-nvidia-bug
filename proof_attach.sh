#!/usr/bin/env bash
# proof_attach.sh — attach to a running rs2client and prove the renderer is llvmpipe
#
# Usage:
#   1. Launch RS3 normally through Bolt
#   2. Wait for the loading screen to appear (EGL is initialised by then)
#   3. Run this script

set -e

PID=$(pgrep -x rs2client 2>/dev/null | head -1)

if [[ -z "$PID" ]]; then
    echo "ERROR: rs2client is not running. Launch RS3 through Bolt first."
    exit 1
fi

echo "============================================================"
echo "DYNAMIC PROOF — attaching to running rs2client (pid $PID)"
echo "============================================================"
echo ""

gdb -batch \
    -ex "set pagination off" \
    -ex "attach $PID" \
    -ex "set \$renderer = (char*)glGetString(0x1F01)" \
    -ex "printf \"[PROOF 3] glGetString(GL_RENDERER) = \\\"%s\\\"\\n\", \$renderer" \
    -ex "set \$vendor_gl = (char*)glGetString(0x1F00)" \
    -ex "printf \"[PROOF 3] glGetString(GL_VENDOR)   = \\\"%s\\\"\\n\", \$vendor_gl" \
    -ex "detach" \
    -ex "quit" \
    2>/dev/null

echo ""
echo "============================================================"
