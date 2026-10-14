#!/bin/sh
# usage: ./bisect.sh <shader name> <line> "<float4 expr>" [target base]  -> probe pixel of that target
N=$1; L=$2; E=$3; T=${4:-317140000}
SRC=../out/hlsl/$N.hlsl
OV=run/override/$N.hlsl
mkdir -p run/override
awk -v L="$L" -v E="$E" 'NR==L { print "    { o.c0 = " E "; return o; }" } { print }' "$SRC" > "$OV"
CTR_AUTOPRESS=1 CTR_CAPTURE=2500 CTR_PROBE=${CTR_PROBE:-1100,340} ./run.sh 60 > /dev/null 2>&1
grep -E "override .* failed|cap pixel $T" run/CrashTeamRacingNitroFueled.log | cut -c9-
rm -f "$OV"
