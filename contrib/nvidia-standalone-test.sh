#!/usr/bin/env bash
#
# nvidia-standalone-test.sh — safe, guarded test of gamescope's standalone DRM
# backend (and HDR) on NVIDIA, with an auto-revert watchdog so a wedged run
# never strands you at a black screen.
#
# HOW TO RUN (important):
#   1. From your desktop, switch to a free TTY:  Ctrl+Alt+F3
#   2. Log in, then:  ~/dev/gamescope/contrib/nvidia-standalone-test.sh
#   3. It runs gamescope standalone for N seconds, logs everything, then exits.
#   4. Switch back to your desktop:  Ctrl+Alt+F2  (or whichever VT it was on)
#
# A background watchdog SIGKILLs gamescope and chvt's you back even if it hangs.
#
set -euo pipefail

DURATION="${1:-12}"                       # seconds to run gamescope
GS="${GS:-$HOME/dev/gamescope/build/src/gamescope}"
CLIENT="${CLIENT:-vkcube}"
HDR="${HDR:-0}"                            # HDR=1 to test --hdr-enabled
LOG="${LOG:-/tmp/gamescope-nvidia-$(date +%s 2>/dev/null || echo run).log}"

die() { echo "FATAL: $*" >&2; exit 1; }

# ── Safety: refuse to run inside a graphical session (it would fight for DRM) ──
if [ -n "${WAYLAND_DISPLAY:-}" ] || [ -n "${DISPLAY:-}" ]; then
    die "Detected WAYLAND_DISPLAY/DISPLAY — you are inside a desktop session.
     Run this from a bare TTY (Ctrl+Alt+F3), not from a terminal in COSMIC."
fi
[ -x "$GS" ] || die "gamescope binary not found/executable at: $GS"
command -v "$CLIENT" >/dev/null || die "test client '$CLIENT' not installed"

ORIG_VT="$(fgconsole 2>/dev/null || echo '')"

echo "=== gamescope NVIDIA standalone test ==="
echo "binary:   $GS ($("$GS" --help 2>&1 | head -1 || true))"
echo "client:   $CLIENT   HDR: $HDR   duration: ${DURATION}s"
echo "log:      $LOG"
echo

# ── Diagnostics captured regardless of whether gamescope succeeds ─────────────
{
    echo "### nvidia driver"; nvidia-smi --query-gpu=driver_version,name --format=csv,noheader 2>/dev/null || true
    echo "### nvidia_drm.modeset"; cat /sys/module/nvidia_drm/parameters/modeset 2>/dev/null || true
    echo "### DRM cards"; ls -l /sys/class/drm/ 2>/dev/null | grep -E 'card[0-9]+($|-)' || true
    echo "### explicit-sync caps (DRM_CAP_SYNCOBJ / TIMELINE)"
    for c in /dev/dri/card*; do echo "-- $c"; done
} >> "$LOG" 2>&1

# ── Watchdog: guarantees we return to the desktop even if gamescope wedges ────
watchdog() {
    sleep "$((DURATION + 8))"
    pkill -KILL -x gamescope 2>/dev/null || true
    [ -n "$ORIG_VT" ] && chvt "$ORIG_VT" 2>/dev/null || true
}
watchdog &
WD_PID=$!
trap 'kill "$WD_PID" 2>/dev/null || true; [ -n "$ORIG_VT" ] && chvt "$ORIG_VT" 2>/dev/null || true' EXIT

# ── Run gamescope standalone (DRM backend), time-boxed ────────────────────────
GS_ARGS=( -W 2560 -H 1440 -O '*' )
[ "$HDR" = "1" ] && GS_ARGS+=( --hdr-enabled --hdr-debug-force-output )

echo "launching: $GS ${GS_ARGS[*]} -- $CLIENT"  | tee -a "$LOG"
set +e
timeout --signal=TERM "$DURATION" \
    "$GS" "${GS_ARGS[@]}" -- "$CLIENT" >> "$LOG" 2>&1
rc=$?
set -e

echo
echo "=== gamescope exited rc=$rc (124 = ran full ${DURATION}s = good) ==="
echo "--- last 30 log lines ---"
tail -n 30 "$LOG"
echo
echo "Full log: $LOG"
echo "Now switch back to your desktop: Ctrl+Alt+F${ORIG_VT:-2}"
