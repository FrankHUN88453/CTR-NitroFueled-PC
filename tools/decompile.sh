#!/bin/sh
# usage: tools/decompile.sh <out.c> <depth> <Class|0xaddr>...
RE="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$(cygpath -w "$(realpath -m "$1")")"; shift
JAVA_HOME='C:\Program Files\Eclipse Adoptium\jdk-25.0.1.8-hotspot' GHIDRA_HEADLESS_MAXMEM=16G \
  "/f/PS4/CRTNF/_tools/ghidra_12.1.4_PUBLIC/support/analyzeHeadless.bat" "$(cygpath -w "$RE/ghidra_proj")" CTR \
  -process eboot.ghidra.elf -noanalysis -readOnly -scriptPath "$(cygpath -w "$RE/ghidra_scripts")" \
  -postScript DecompileClasses.java "$OUT" "$@" 2>&1 | grep -E 'decompiled|ERROR|no namespace'
