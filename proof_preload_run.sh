#!/usr/bin/env bash
# proof_preload_run.sh — run rs2client with LD_PRELOAD proof library
#
# Workflow:
#   1. Launch RS3 through Bolt, wait for loading screen, run capture_args.sh
#   2. Close the game
#   3. Run this script — no gdb, no ptrace, just LD_PRELOAD
#   4. Check output at /tmp/rs3_proof.log

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RS2CLIENT="${RS2CLIENT:-${HOME}/.var/app/com.adamcake.Bolt/data/bolt-launcher/Jagex/launcher/rs2client}"
DATA_DIR="${DATA_DIR:-${HOME}/.var/app/com.adamcake.Bolt/data/bolt-launcher/Jagex/launcher}"
SAVED_ARGS="${SCRIPT_DIR}/args.bin"
SO="${SCRIPT_DIR}/proof_preload.so"

# Build the .so if needed
if [[ ! -f "$SO" || "$SO" -ot "${SCRIPT_DIR}/proof_preload.c" ]]; then
    echo "[*] Building proof_preload.so..."
    gcc -shared -fPIC -O0 -o "$SO" "${SCRIPT_DIR}/proof_preload.c" -ldl
    echo "[*] Built $SO"
fi

# Load saved args
RS2CLIENT_ARGS=()
if [[ -f "$SAVED_ARGS" ]]; then
    first=1
    while IFS= read -r -d '' arg; do
        if [[ $first -eq 1 ]]; then first=0; continue; fi
        RS2CLIENT_ARGS+=("$arg")
    done < "$SAVED_ARGS"
    echo "[*] Loaded ${#RS2CLIENT_ARGS[@]} args from args.bin"
else
    echo "[*] No args.bin — running without session args (game may exit before EGL)"
fi

rm -f /tmp/rs3_proof.log

echo ""
echo "============================================================"
echo "Running rs2client with proof_preload.so"
echo "Log: /tmp/rs3_proof.log"
echo "============================================================"
echo ""
echo "The game window will open. Wait for the loading screen, then"
echo "close the game. The log will contain the proof output."
echo ""

export HOME="$DATA_DIR"
export SDL_VIDEODRIVER="x11"
export PULSE_PROP_OVERRIDE="application.name='RuneScape' application.icon_name='runescape' media.role='game'"
export LD_PRELOAD="$SO"

cd "$DATA_DIR"
"$RS2CLIENT" "${RS2CLIENT_ARGS[@]}" &
GAME_PID=$!

# Wait for the log to appear and tail it live
sleep 2
if [[ -f /tmp/rs3_proof.log ]]; then
    echo "[*] Live proof output:"
    echo ""
    tail -f /tmp/rs3_proof.log &
    TAIL_PID=$!
    wait $GAME_PID 2>/dev/null || true
    kill $TAIL_PID 2>/dev/null || true
else
    wait $GAME_PID 2>/dev/null || true
fi

echo ""
echo "============================================================"
echo "FINAL LOG: /tmp/rs3_proof.log"
echo "============================================================"
cat /tmp/rs3_proof.log 2>/dev/null || echo "(no log generated)"
