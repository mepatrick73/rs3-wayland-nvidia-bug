#!/usr/bin/env bash
# capture_args.sh — save rs2client's launch args from a running instance
#
# Usage:
#   1. Launch RS3 through Bolt as normal
#   2. Wait for the loading screen
#   3. Run this script — it saves the args to args.bin
#   4. Close the game
#   5. Immediately run proof_run.sh (window/EGL init happens before auth,
#      so breakpoints will fire even if tokens are no longer valid)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${SCRIPT_DIR}/args.bin"

PID=$(pgrep -x rs2client 2>/dev/null | head -1)

if [[ -z "$PID" ]]; then
    echo "ERROR: rs2client is not running. Launch RS3 through Bolt first."
    exit 1
fi

cp /proc/"$PID"/cmdline "$OUT"
echo "Saved args for pid $PID to $OUT"
echo ""
echo "Next steps:"
echo "  1. Close the game"
echo "  2. Run: bash proof_run.sh"
