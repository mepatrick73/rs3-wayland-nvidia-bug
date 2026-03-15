#!/usr/bin/env bash
# proof_run.sh — run rs2client under gdb and capture runtime EGL proofs
#
# Preferred workflow (for claim 4 — window ID match):
#   1. Launch RS3 through Bolt normally
#   2. While loading screen is visible, run: bash capture_args.sh
#   3. Close the game
#   4. Immediately run this script — window/EGL init happens before auth,
#      so all breakpoints fire even if session tokens have since expired
#
# Fallback: run without args (proves claims 1-3 only; game exits before EGL
# if no session tokens are provided).

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RS2CLIENT="${RS2CLIENT:-${HOME}/.var/app/com.adamcake.Bolt/data/bolt-launcher/Jagex/launcher/rs2client}"
DATA_DIR="${DATA_DIR:-${HOME}/.var/app/com.adamcake.Bolt/data/bolt-launcher/Jagex/launcher}"
SAVED_ARGS="${SCRIPT_DIR}/args.bin"

if [[ ! -f "$RS2CLIENT" ]]; then
    echo "ERROR: rs2client not found at $RS2CLIENT"
    exit 1
fi

# Build argv from saved args.bin (null-separated, from /proc/<pid>/cmdline)
# Skip argv[0] (the binary path — we supply it ourselves via --args)
RS2CLIENT_ARGS=()
if [[ -f "$SAVED_ARGS" ]]; then
    echo "[*] Loading args from $SAVED_ARGS"
    first=1
    while IFS= read -r -d '' arg; do
        if [[ $first -eq 1 ]]; then
            first=0
            continue
        fi
        RS2CLIENT_ARGS+=("$arg")
    done < "$SAVED_ARGS"
    echo "[*] Loaded ${#RS2CLIENT_ARGS[@]} arguments"
else
    echo "[*] No args.bin found — running without session args"
    echo "    (run capture_args.sh while the game is open to enable claim 4)"
fi

echo ""
echo "============================================================"
echo "DYNAMIC PROOF — rs2client EGL runtime analysis"
echo "Binary : $RS2CLIENT"
echo "HOME   : $DATA_DIR"
echo "Args   : ${#RS2CLIENT_ARGS[@]} args from args.bin"
echo "============================================================"
echo ""
echo "The game window will open (or fail at auth — that's fine, EGL"
echo "init happens first). Quit the game when done."
echo ""

export HOME="$DATA_DIR"
export SDL_VIDEODRIVER="x11"
export PULSE_PROP_OVERRIDE="application.name='RuneScape' application.icon_name='runescape' media.role='game'"

# Bolt chdirs to data_dir before exec — match that behaviour
cd "$DATA_DIR"

gdb \
    -ex "set pagination off" \
    -ex "set print thread-events off" \
    -ex "set follow-fork-mode child" \
    -ex "set detach-on-fork off" \
    -ex "source ${SCRIPT_DIR}/proof_dynamic.py" \
    -ex "handle SIGSEGV stop print" \
    --args "$RS2CLIENT" "${RS2CLIENT_ARGS[@]}"
