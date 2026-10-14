#!/bin/sh
# Build and run ctr_pc against the game dump; log in run/CrashTeamRacingNitroFueled.log
set -e
cd "$(dirname "$0")"
powershell -NoProfile -Command "cmd /c \"$(cygpath -w "$(pwd)/build.bat")\"" | grep -E "error|FAILED" && exit 1
mkdir -p run && cd run
export CTR_LOG="$(cygpath -w "$(pwd)")\CrashTeamRacingNitroFueled.log"
# test runs keep their own saves and settings (run/userdata), away from the player's
mkdir -p userdata
export CTR_USER_DIR="${CTR_USER_DIR:-$(cygpath -w "$(pwd)/userdata")}"
CTR_PROBE=${CTR_PROBE:-960,720} CTR_CAPTURE=${CTR_CAPTURE:-0} CTR_SCREENSHOT=${CTR_SCREENSHOT:-0} CTR_AUTOPRESS=${CTR_AUTOPRESS:-0} timeout $(( ${1:-60} + 30 )) ../build/CrashTeamRacingNitroFueled.exe "F:/PS4/CRTNF/UP0002-CUSA13795_00-CRASHTEAMRACING1" "${1:-60}" > stdout.txt 2>&1 || echo "exit=$?"
