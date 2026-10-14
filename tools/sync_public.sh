#!/bin/sh
# Copies the committed own code of this repository (everything except out/, the game-derived data)
# into the public repository's clone and commits it there. This repository's history stays private
# (older commits contain game data). Review, then push the public clone yourself.
# usage: tools/sync_public.sh <public clone> "<commit message>"
set -e
cd "$(dirname "$0")/.."
PUB=${1:?public clone}
MSG=${2:?commit message}
[ -d "$PUB/.git" ] || { echo "$PUB is not a git clone"; exit 1; }
(cd "$PUB" && git ls-files -z | xargs -0 rm -f)
git archive HEAD -- . ':!out' | tar -x -C "$PUB"
grep -qx 'out/' "$PUB/.gitignore" || printf 'out/\n' >> "$PUB/.gitignore"
cd "$PUB"
git add -A
if git ls-files | grep -q '^out/'; then echo "out/ must not be published"; exit 1; fi
git diff --cached --quiet && { echo "nothing to sync"; exit 0; }
git commit -q -m "$MSG"
git log -1 --stat --format="%h %s" | tail -20
