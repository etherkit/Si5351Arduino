#!/usr/bin/env bash
# run_v231_checks.sh - repeatable Si5351Arduino v2.3.1 acceptance + regression checks.
# Self-contained: everything it needs is vendored next to this script. See README.md.
set -euo pipefail

CI_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_URL="https://github.com/etherkit/Si5351Arduino.git"
SRC=""            # path to a source tree / clone (optional)
REF=""            # git ref or SHA (optional; default: master of REPO_URL, or HEAD of --src clone)
OUT=""
CORES=""          # "" = no board compiles; "all" or comma list of board keys
REQUIRE_FIXED=0
INSTALL_CORES=1
ARDUINO_CLI="${ARDUINO_CLI:-}"
CXX="${CXX:-g++}"
PY="${PYTHON:-python3}"

usage() { cat <<'U'
Usage: run_v231_checks.sh [options]
  --src PATH         library source: a git clone or a plain source tree (must contain src/si5351.cpp)
  --ref REF          git ref/branch/tag/SHA. With --src: archived from that clone. Without --src:
                     fetched read-only from --repo. Default: master HEAD of --repo.
  --repo URL         upstream repo (default https://github.com/etherkit/Si5351Arduino.git)
  --out DIR          output dir (default ./runs/<timestamp>_<sha7>)
  --require-fixed    the 25 v2.3.1 acceptance tests MUST pass (default: failures are expected-fail)
  --cores [LIST]     also arduino-cli compile on boards: "all" (default when no list) or comma list
                     of keys from boards.txt (uno,mega,nano_every,esp32,esp8266,bluepill,nano33ble,zero,pico)
  --no-install       do not install missing arduino cores (missing core => FAIL)
  --arduino-cli PATH arduino-cli binary (default: $ARDUINO_CLI, ~/bin/arduino-cli, or PATH)
  -h, --help
Exit: 0 when every check is PASS/XFAIL/SKIP, 1 on any unexpected failure, 2 on usage/setup error.
U
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --src) SRC="$2"; shift 2;;
    --ref) REF="$2"; shift 2;;
    --repo) REPO_URL="$2"; shift 2;;
    --out) OUT="$2"; shift 2;;
    --require-fixed) REQUIRE_FIXED=1; shift;;
    --cores) if [[ $# -gt 1 && "$2" != --* ]]; then CORES="$2"; shift 2; else CORES="all"; shift; fi;;
    --no-install) INSTALL_CORES=0; shift;;
    --arduino-cli) ARDUINO_CLI="$2"; shift 2;;
    -h|--help) usage; exit 0;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 2;;
  esac
done

for t in git "$CXX" "$PY" tar; do command -v "$t" >/dev/null || { echo "missing required tool: $t" >&2; exit 2; }; done

STARTED="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
TMPOUT="${OUT:-$CI_DIR/runs/run_$(date +%Y%m%d_%H%M%S)_$$}"
mkdir -p "$TMPOUT"; OUT="$(cd "$TMPOUT" && pwd)"
LOGS="$OUT/logs"; LIB="$OUT/lib"; BUILD="$OUT/build"
mkdir -p "$LOGS" "$BUILD"
RESULTS="$OUT/results.tsv"; : > "$RESULTS"
echo "== run_v231_checks: output in $OUT"

# record NAME STATUS DETAIL LOG   (STATUS: PASS FAIL XFAIL SKIP INFO)
record() { printf '%s\t%s\t%s\t%s\n' "$1" "$2" "${3//$'\t'/ }" "${4:-}" >> "$RESULTS"; printf '  %-34s %-5s %s\n' "$1" "$2" "$3"; }

# ---------------------------------------------------------------- 1. pin the library source
SRC_KIND=""; SHA="unknown"; DIRTY="n/a"; DESC=""
rm -rf "$LIB"; mkdir -p "$LIB"
if [[ -z "$SRC" ]]; then
  SRC_KIND="fetched"; R="${REF:-master}"
  W="$OUT/work/repo"; rm -rf "$W"; mkdir -p "$W"
  git -C "$W" init -q
  git -C "$W" remote add origin "$REPO_URL"
  git -C "$W" -c protocol.version=2 fetch -q --depth 1 origin "$R" > "$LOGS/git_fetch.log" 2>&1 \
    || { cat "$LOGS/git_fetch.log" >&2; echo "fetch of '$R' from $REPO_URL failed" >&2; exit 2; }
  SHA="$(git -C "$W" rev-parse FETCH_HEAD)"; DESC="$REPO_URL $R"
  git -C "$W" archive "$SHA" | tar -x -C "$LIB"
elif [[ -d "$SRC/.git" ]] || git -C "$SRC" rev-parse --git-dir >/dev/null 2>&1; then
  SRC_KIND="git-clone"; R="${REF:-HEAD}"
  SHA="$(git -C "$SRC" rev-parse --verify "$R^{commit}")"; DESC="$(cd "$SRC" && pwd) $R"
  if [[ -z "$REF" && -n "$(git -C "$SRC" status --porcelain -- src 2>/dev/null)" ]]; then DIRTY="yes (uncommitted src/ changes ignored: tested $SHA as committed)"; else DIRTY="no"; fi
  git -C "$SRC" archive "$SHA" | tar -x -C "$LIB"      # pinned: unaffected by later edits to the clone
elif [[ -f "$SRC/src/si5351.cpp" || -f "$SRC/si5351.cpp" ]]; then
  SRC_KIND="tree"; DESC="$(cd "$SRC" && pwd)"
  [[ -n "$REF" ]] && { echo "--ref needs a git clone; $SRC is a plain tree" >&2; exit 2; }
  if [[ -f "$SRC/src/si5351.cpp" ]]; then (cd "$SRC" && tar --exclude=.git -cf - .) | tar -x -C "$LIB"
  else mkdir -p "$LIB/src"; cp "$SRC"/si5351.{cpp,h} "$LIB/src/"; fi
  SHA="tree-sha256:$(cat "$LIB/src/si5351.cpp" "$LIB/src/si5351.h" | sha256sum | cut -c1-16)"
else
  echo "--src $SRC: neither a git clone nor a tree containing src/si5351.cpp" >&2; exit 2
fi
[[ -f "$LIB/src/si5351.cpp" && -f "$LIB/src/si5351.h" ]] || { echo "pinned tree lacks src/si5351.{cpp,h}" >&2; exit 2; }
( cd "$LIB" && sha256sum src/si5351.cpp src/si5351.h ) > "$OUT/PINNED_SHA256SUMS"
echo "$SHA" > "$OUT/PINNED_SHA"
LIBVER="$(sed -n 's/^version=//p' "$LIB/library.properties" 2>/dev/null || true)"
HAS_TWOWIRE=0; grep -q 'TwoWire *\*' "$LIB/src/si5351.h" && HAS_TWOWIRE=1
echo "   source: $SRC_KIND $DESC -> $SHA (library.properties version ${LIBVER:-?}, TwoWire ctor: $HAS_TWOWIRE)"
cat > "$OUT/meta.env" <<M
SHA=$SHA
SRC_KIND=$SRC_KIND
SRC_DESC=$DESC
DIRTY=$DIRTY
LIBVER=$LIBVER
HAS_TWOWIRE=$HAS_TWOWIRE
REQUIRE_FIXED=$REQUIRE_FIXED
CORES=$CORES
STARTED=$STARTED
HOST=$(hostname)
OS=$(uname -srm)
CXX_VERSION=$($CXX --version | head -1)
PY_VERSION=$($PY --version 2>&1)
M

CXXFLAGS="-std=gnu++17 -O1 -g -Wall -Wno-unused-variable -Wno-unused-result"
# pattern-init makes reads of uninitialised locals deterministic (needs gcc>=12 / clang>=8); drop if unsupported
echo 'int main(){return 0;}' > "$BUILD/probe.cpp"
if $CXX -ftrivial-auto-var-init=pattern "$BUILD/probe.cpp" -o "$BUILD/probe" 2>/dev/null; then CXXFLAGS="$CXXFLAGS -ftrivial-auto-var-init=pattern"
else echo "   note: $CXX lacks -ftrivial-auto-var-init=pattern; equivalence transcripts may differ on uninitialised paths"; fi

# ---------------------------------------------------------------- 2. v2.3.1 acceptance + must-not-break
echo "== host checks"
ACC="$BUILD/acc"; ACCJ="$LOGS/acc.jsonl"
if $CXX $CXXFLAGS -I"$CI_DIR/v231" -I"$CI_DIR/stubs" -I"$LIB/src" "$CI_DIR/v231/acceptance.cpp" "$CI_DIR/stubs/Wire.cpp" "$LIB/src/si5351.cpp" -o "$ACC" > "$LOGS/build_acceptance.log" 2>&1; then
  record build_acceptance PASS "acceptance runner built against pinned source" logs/build_acceptance.log
  if "$ACC" "$ACCJ" > "$LOGS/run_acceptance.log" 2>&1; then
    record run_acceptance PASS "$(wc -l < "$ACCJ") JSONL runs" logs/run_acceptance.log
    MODE=""; [[ $REQUIRE_FIXED == 1 ]] && MODE="--require-fixed"
    set +e; $PY "$CI_DIR/v231/test_v231.py" "$ACCJ" $MODE > "$LOGS/test_v231.log" 2>&1; TRC=$?; set -e
    while read -r st id rest; do
      if [[ $st == PASS ]]; then record "acceptance_$id" PASS "$rest" logs/test_v231.log
      elif [[ $REQUIRE_FIXED == 1 ]]; then record "acceptance_$id" FAIL "$rest" logs/test_v231.log
      else record "acceptance_$id" XFAIL "$rest (expected to fail until v2.3.1)" logs/test_v231.log; fi
    done < <(grep -E '^(PASS|FAIL) A[0-9]+ ' "$LOGS/test_v231.log")
    NACC=$(grep -cE '^(PASS|FAIL) A[0-9]+ ' "$LOGS/test_v231.log" || true)
    [[ $NACC == 25 ]] || record acceptance_count FAIL "expected 25 acceptance results, parsed $NACC" logs/test_v231.log
    MNB="$(grep -E '^== MUST-NOT-BREAK' "$LOGS/test_v231.log" || true)"
    if [[ $MNB =~ ([0-9]+)\ checks,\ ([0-9]+)\ failures ]] && [[ ${BASH_REMATCH[2]} == 0 ]]; then
      record must_not_break PASS "${BASH_REMATCH[1]} checks, 0 failures" logs/test_v231.log
    else record must_not_break FAIL "${MNB:-no MUST-NOT-BREAK summary} (test_v231 rc=$TRC)" logs/test_v231.log; fi
  else record run_acceptance FAIL "runner crashed (rc=$?)" logs/run_acceptance.log; fi
else record build_acceptance FAIL "compile error" logs/build_acceptance.log; fi

# ---------------------------------------------------------------- 3. issue #106 TwoWire test
if [[ $HAS_TWOWIRE == 1 ]]; then
  if $CXX -std=gnu++17 -Dprivate=public -I"$CI_DIR/issue106/stubs" -I"$LIB/src" "$CI_DIR/issue106/t106.cpp" "$LIB/src/si5351.cpp" -o "$BUILD/t106" > "$LOGS/build_t106.log" 2>&1; then
    if "$BUILD/t106" > "$LOGS/t106.log" 2>&1 && grep -q '^ALL PASS' "$LOGS/t106.log"; then
      record t106_twowire PASS "$(grep -c '^PASS' "$LOGS/t106.log") checks, $(tail -1 "$LOGS/t106.log")" logs/t106.log
    else record t106_twowire FAIL "$(grep -c '^FAIL' "$LOGS/t106.log" || true) failing checks" logs/t106.log; fi
  else record t106_twowire FAIL "compile error" logs/build_t106.log; fi
else record t106_twowire SKIP "source has no TwoWire* constructor (pre-#106)"; fi

# ---------------------------------------------------------------- 4. goldens + equivalence vs master 1ddd43e (C++ harness)
EQ="$CI_DIR/equiv"
if $CXX $CXXFLAGS -I"$EQ" -I"$CI_DIR/stubs" -I"$LIB/src" "$EQ/harness.cpp" "$CI_DIR/stubs/Wire.cpp" "$LIB/src/si5351.cpp" -o "$BUILD/h_lib" > "$LOGS/build_harness_lib.log" 2>&1 \
 && $CXX $CXXFLAGS -I"$EQ" -I"$CI_DIR/stubs" -I"$CI_DIR/reference/master_1ddd43e" "$EQ/harness.cpp" "$CI_DIR/stubs/Wire.cpp" "$CI_DIR/reference/master_1ddd43e/si5351.cpp" -o "$BUILD/h_ref" > "$LOGS/build_harness_ref.log" 2>&1; then
  "$BUILD/h_lib" "$LOGS/harness_lib.jsonl" 2> "$LOGS/run_harness_lib.log" || record run_harness_lib FAIL "harness crashed" logs/run_harness_lib.log
  "$BUILD/h_ref" "$LOGS/harness_master1ddd43e.jsonl" 2> "$LOGS/run_harness_ref.log" || record run_harness_ref FAIL "harness crashed" logs/run_harness_ref.log
  set +e; $PY "$EQ/compare.py" "$LOGS/harness_lib.jsonl" "$LOGS/harness_master1ddd43e.jsonl" "$LOGS/equiv_report.txt" > "$LOGS/compare.log" 2>&1; set -e
  G="$(grep -E '^Goldens vs ORIGINAL C\+\+' "$LOGS/equiv_report.txt" || true)"
  if [[ $G =~ ([0-9]+)\ field\ checks,\ ([0-9]+)\ mismatches ]]; then
    if [[ ${BASH_REMATCH[2]} == 0 ]]; then record goldens27_vs_lib PASS "27 golden cases, ${BASH_REMATCH[1]} field checks, 0 mismatches" logs/equiv_report.txt
    elif [[ $REQUIRE_FIXED == 1 ]]; then record goldens27_vs_lib INFO "${BASH_REMATCH[2]} golden mismatches (goldens lock v2.2.0/2.3.0 behaviour; fixes may change some, review the list)" logs/equiv_report.txt
    else record goldens27_vs_lib FAIL "${BASH_REMATCH[2]} mismatches of ${BASH_REMATCH[1]} field checks" logs/equiv_report.txt; fi
  else record goldens27_vs_lib FAIL "compare.py produced no golden summary" logs/compare.log; fi
  EQL="$(grep -E '^Equivalence:' "$LOGS/equiv_report.txt" || true)"
  if [[ $EQL =~ differing\ runs:\ ([0-9]+) ]]; then
    record equiv_vs_master_1ddd43e INFO "${BASH_REMATCH[1]} differing runs vs master 1ddd43e (0 = behaviourally identical; fixes are expected to differ)" logs/equiv_report.txt
  else record equiv_vs_master_1ddd43e FAIL "no equivalence summary" logs/compare.log; fi
  NB="$(grep -E '^Bonus' "$LOGS/equiv_report.txt" || true)"; [[ -n $NB ]] && record goldens_newer_vs_lib INFO "${NB#Bonus - }" logs/equiv_report.txt
else record harness_build FAIL "compile error (see build_harness_*.log)" logs/build_harness_lib.log; fi

# ---------------------------------------------------------------- 5. Python golden/spec suites (oracle self-consistency)
for t in test_golden test_spec; do
  if (cd "$CI_DIR/python" && $PY "$t.py") > "$LOGS/py_$t.log" 2>&1; then record "py_$t" PASS "$(grep -vE '^\s*$' "$LOGS/py_$t.log" | tail -1)" "logs/py_$t.log"
  else record "py_$t" FAIL "rc!=0" "logs/py_$t.log"; fi
done

# ---------------------------------------------------------------- 6. C++98 compatibility (informational)
if $CXX -std=c++98 -pedantic -fsyntax-only -I"$CI_DIR/stubs" -I"$LIB/src" "$LIB/src/si5351.cpp" > "$LOGS/cxx98.log" 2>&1; then
  record cxx98_syntax INFO "builds with -std=c++98 -pedantic; $(grep -c 'warning:' "$LOGS/cxx98.log" || true) pedantic warnings (master 1ddd43e: 73)" logs/cxx98.log
else record cxx98_syntax INFO "does NOT build with -std=c++98 (informational)" logs/cxx98.log; fi

# ---------------------------------------------------------------- 7. optional arduino-cli board compiles
if [[ -n $CORES ]]; then
  echo "== board compiles ($CORES)"
  if [[ -z $ARDUINO_CLI ]]; then
    if [[ -x "$HOME/bin/arduino-cli" ]]; then ARDUINO_CLI="$HOME/bin/arduino-cli"; else ARDUINO_CLI="$(command -v arduino-cli || true)"; fi
  fi
  if [[ -z $ARDUINO_CLI || ! -x $ARDUINO_CLI ]]; then
    record arduino_cli FAIL "arduino-cli not found (install to ~/bin, see README)"
  else
    # Version-manager shims (mise/asdf/pyenv) on PATH can break the python3 that some cores (esp8266) call
    # during the build; give arduino-cli a PATH without them when a real python3 is still reachable.
    AC_PATH="$(printf '%s' "$PATH" | tr ':' '\n' | grep -vE '/(mise|asdf|\.pyenv)/shims' | paste -sd: -)"
    if PATH="$AC_PATH" command -v python3 >/dev/null 2>&1 && [[ "$AC_PATH" != "$PATH" ]]; then
      echo "   note: running arduino-cli with version-manager shims removed from PATH"; ACENV=(env "PATH=$AC_PATH")
    else ACENV=(env); fi
    AC="$ARDUINO_CLI"; echo "ARDUINO_CLI=$AC ($($AC version 2>&1 | head -1))" >> "$OUT/meta.env"
    URLS="$(grep -E '^url\|' "$CI_DIR/boards.txt" | cut -d'|' -f2 | paste -sd, -)"
    $AC core update-index --additional-urls "$URLS" > "$LOGS/core_update_index.log" 2>&1 || true
    $AC core list --additional-urls "$URLS" > "$LOGS/core_list_before.log" 2>&1 || true
    SK="$OUT/sketches"; rm -rf "$SK"; mkdir -p "$SK"
    cp -r "$LIB/examples/si5351_example" "$LIB/examples/si5351_vcxo" "$SK/" 2>/dev/null || true
    # Ctor106 is stored as .ino.txt so arduino-lint does not flag a sketch outside examples/
    [[ $HAS_TWOWIRE == 1 ]] && { mkdir -p "$SK/Ctor106"; cp "$CI_DIR/sketches/Ctor106/Ctor106.ino.txt" "$SK/Ctor106/Ctor106.ino"; }
    while IFS='|' read -r key fqbn core; do
      [[ -z $key || $key == \#* || $key == url ]] && continue
      [[ $CORES != all && ",$CORES," != *",$key,"* ]] && continue
      if ! grep -q "^${core} " "$LOGS/core_list_before.log" && ! $AC core list --additional-urls "$URLS" 2>/dev/null | grep -q "^${core} "; then
        if [[ $INSTALL_CORES == 1 ]]; then
          echo "   installing core $core ..."
          if ! "${ACENV[@]}" $AC core install "$core" --additional-urls "$URLS" > "$LOGS/core_install_${core//:/_}.log" 2>&1; then
            record "core_$key" FAIL "core install $core failed" "logs/core_install_${core//:/_}.log"; continue; fi
        else record "core_$key" FAIL "core $core not installed (--no-install)"; continue; fi
      fi
      for s in "$SK"/*/; do
        s="${s%/}"; n="$(basename "$s")"; lg="logs/compile_${key}_${n}.log"
        if "${ACENV[@]}" $AC compile --fqbn "$fqbn" --library "$LIB" --warnings all --additional-urls "$URLS" --build-path "$BUILD/ac_${key}_${n}" "$s" > "$OUT/$lg" 2>&1; then
          sz="$( { grep -oE 'Sketch uses [0-9]+ bytes' "$OUT/$lg" || grep -oE 'IROM +[0-9]+' "$OUT/$lg" || echo '?'; } | grep -oE '[0-9]+|\?' | head -1)"
          lw="$( { grep -E "$LIB/src/.*warning" "$OUT/$lg" || true; } | sort -u | wc -l)"
          record "compile_${key}_${n}" PASS "$sz B flash, $lw library warnings" "$lg"
        elif [[ $n == si5351_vcxo ]] && grep -q 'get_correction' "$OUT/$lg" && [[ $REQUIRE_FIXED == 0 ]]; then
          record "compile_${key}_${n}" XFAIL "known v2.3.0 example bug (get_correction() needs an argument; fixed in v2.4.0)" "$lg"
        else record "compile_${key}_${n}" FAIL "compile error" "$lg"; fi
      done
    done < "$CI_DIR/boards.txt"
  fi
fi

# ---------------------------------------------------------------- 8. summary
FINISHED="$(date -u +%Y-%m-%dT%H:%M:%SZ)"; echo "FINISHED=$FINISHED" >> "$OUT/meta.env"
set +e; $PY "$CI_DIR/tools/summarize.py" "$OUT"; RC=$?; set -e
echo "== summary: $OUT/summary.md  $OUT/summary.json  (exit $RC)"
exit $RC
