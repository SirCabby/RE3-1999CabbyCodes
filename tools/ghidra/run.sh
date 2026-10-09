#!/bin/bash
# tools/ghidra/run.sh <xref|dec|funcs> [addresses...] - a headless query against
# the analysed dump (see tools/ghidra/Report.java). The answer is printed and
# also left in ghidra_proj/report.txt.
cd "$(dirname "$0")/../.." || exit 1
OUT="$PWD/ghidra_proj/report.txt"
MAXMEM=8G /opt/ghidra/support/analyzeHeadless ghidra_proj RE3 -process ResidentEvil3.fixed.exe -noanalysis \
  -scriptPath tools/ghidra -postScript Report.java "$OUT" "$@" > ghidra_proj/report.log 2>&1
cat "$OUT"
