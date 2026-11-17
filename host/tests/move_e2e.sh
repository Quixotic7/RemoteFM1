#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# rfm_move end to end (modelled on abletonmoveremote's tests/loopback.sh): Move Remote's simulator started on a
# spare port with its test hook in a temporary directory, then move_e2e connects, injects presses through the
# simulator's FIFO and checks the panel actions, the LEDs and the screen. Exit 77 (ctest: skipped) without the
# simulator.
#   move_e2e.sh MOVE_E2E_BINARY [MOVEREMOTE_SIM]     ($MOVEREMOTE_SIM wins over the second argument)
E2E="$1"
SIM="${MOVEREMOTE_SIM:-$2}"
if [ -z "$SIM" ] || [ ! -x "$SIM" ]; then
    echo "move_e2e: skipped (no moveremote-sim: build abletonmoveremote with 'make sim' and set MOVEREMOTE_SIM or"
    echo "          -DRFM_MOVEREMOTE_SIM=.../build/sim/moveremote-sim)"
    exit 77
fi
PORT=$((17800 + $$ % 1000))
DIR="$(mktemp -d "${TMPDIR:-/tmp}/rfm-move-e2e.XXXXXX")"
"$SIM" --port "$PORT" --dir "$DIR" 2>"$DIR/sim.log" &
PID=$!
cleanup() {
    kill "$PID" 2>/dev/null
    wait "$PID" 2>/dev/null
    rm -f "$DIR"/mr-cmd "$DIR"/mr-state "$DIR"/mr-test "$DIR"/mr-screen.pbm "$DIR"/sim.log "$DIR"/*.tmp
    rmdir "$DIR" 2>/dev/null
}
trap cleanup EXIT
i=0
while [ ! -p "$DIR/mr-cmd" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
if [ ! -p "$DIR/mr-cmd" ]; then
    echo "move_e2e: the simulator did not start"
    cat "$DIR/sim.log"
    exit 1
fi
"$E2E" --port "$PORT" --dir "$DIR"
rc=$?
echo "quit" > "$DIR/mr-cmd"
[ $rc -eq 0 ] || cat "$DIR/sim.log"
exit $rc
