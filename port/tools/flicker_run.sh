#!/bin/sh
# Long hub drive (about 7800 frames) that looks for single-frame glitches: CTR_GLITCH_TRAP keeps only
# the frames that suddenly turn dark, logged as "glitch trap". Needs a save that reaches the hub in
# run/userdata (copy the player's saves there). Extra environment variables pass through.
# usage: tools/flicker_run.sh [recorded frames]
cd "$(dirname "$0")/.."
rm -rf run/userdata/capture
steer=""
for t in 48 60 72 84 96 108 120 132 144 156 168 180; do
    steer="$steer;$t-$((t+2)):LeftStickLeft;$((t+6))-$((t+7)):LeftStickRight"
done
CTR_GLITCH_TRAP=${CTR_GLITCH_TRAP:-1} CTR_RECORD_AT=2400 CTR_RECORD_FRAMES=${1:-7800} CTR_RECORD_SIZE=480x270 \
CTR_TEST_PAD1="6:Triangle;8:Triangle;10:Triangle;12:Triangle;14:Triangle;20:Cross;27:Cross;32:Cross;36:Cross;40-190:Cross$steer" \
    ./run.sh 195
echo "$(grep -ac 'glitch trap:' run/stdout.txt) glitch(es); last: $(grep -a 'gfx: frame' run/stdout.txt | tail -1 | cut -c1-40)"
grep -a "glitch trap:" run/stdout.txt | sed 's/^.*glitch trap: //'
