#!/usr/bin/env bash
# Scratch mutation of the pinned v2.4.2 library. The hard gate must report
# a non-zero differing-run count. This script exits 0 only when that happens.
# It does not modify the worktree and it does not fetch a branch.
set -euo pipefail

CI_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REF="$CI_DIR/reference/v242_f885fca"
CXX="${CXX:-g++}"
OUT="${NEG_OUT:-/tmp/si5351-v242-negative}"
command -v "$CXX" >/dev/null || { echo "missing $CXX" >&2; exit 2; }

( cd "$REF" && sha256sum -c SHA256SUMS ) || { echo "FAIL  pinned reference hash" >&2; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT"
cp "$REF/si5351.cpp" "$OUT/si5351.cpp"
cp "$REF/si5351.h" "$OUT/si5351.h"
python3 - "$OUT/si5351.cpp" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = "reg_val &= ~(1<<(uint8_t)clk);"
n = s.count(old)
if n != 1:
    sys.stderr.write("mutation site count is %d, expected 1\n" % n)
    sys.exit(2)
# Flip output_enable()'s enable path so a requested enable sets the OEB bit.
s = s.replace(old, "reg_val |= (1<<(uint8_t)clk);", 1)
open(p, "w").write(s)
PY

CXXFLAGS="-std=gnu++17 -O1 -g -Wall -Wno-unused-variable -Wno-unused-result"
echo 'int main(){return 0;}' > "$OUT/probe.cpp"
if $CXX -ftrivial-auto-var-init=pattern "$OUT/probe.cpp" -o "$OUT/probe" 2>/dev/null; then
  CXXFLAGS="$CXXFLAGS -ftrivial-auto-var-init=pattern"
fi

$CXX $CXXFLAGS -I"$CI_DIR/equiv" -I"$CI_DIR/stubs" -I"$OUT" \
  "$CI_DIR/equiv/harness.cpp" "$CI_DIR/stubs/Wire.cpp" "$OUT/si5351.cpp" \
  -o "$OUT/h_mut" > "$OUT/build_mut.log" 2>&1
$CXX $CXXFLAGS -I"$CI_DIR/equiv" -I"$CI_DIR/stubs" -I"$REF" \
  "$CI_DIR/equiv/harness.cpp" "$CI_DIR/stubs/Wire.cpp" "$REF/si5351.cpp" \
  -o "$OUT/h_pin" > "$OUT/build_pin.log" 2>&1
"$OUT/h_mut" "$OUT/mut.jsonl" > "$OUT/run_mut.log" 2>&1
"$OUT/h_pin" "$OUT/pin.jsonl" > "$OUT/run_pin.log" 2>&1
python3 "$CI_DIR/equiv/compare.py" "$OUT/mut.jsonl" "$OUT/pin.jsonl" "$OUT/equiv_report.txt" \
  > "$OUT/compare.log" 2>&1 || true
line="$(grep -E '^Equivalence:' "$OUT/equiv_report.txt" || true)"
echo "$line"
diffs="$(printf '%s\n' "$line" | sed -n 's/.*differing runs: \([0-9][0-9]*\).*/\1/p')"
if [[ -z "$diffs" ]]; then
  echo "FAIL  negative control produced no equivalence summary" >&2
  exit 1
fi
if [[ "$diffs" -eq 0 ]]; then
  echo "FAIL  mutation was not detected (differing runs: 0)" >&2
  exit 1
fi
echo "PASS  negative control: mutated output_enable differs in $diffs runs"
exit 0
