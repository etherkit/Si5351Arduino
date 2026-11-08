#!/usr/bin/env bash
# Fail-closed Uno flash budget for the minimal sketches, including the
# SI5351_NO_CLK67 build of the runtime-clock sketch.
# Missing CLI, core, compiler, map, or a "Sketch uses" line is a failure.
# This measures program bytes. It is not a timing benchmark.
set -euo pipefail

CI_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT="$(cd "$CI_DIR/../.." && pwd)"
OUT="${FLASH_OUT:-/tmp/si5351-flash-budget}"
CLI="${ARDUINO_CLI:-}"
FQBN="arduino:avr:uno"
CORE_ID="arduino:avr"
CORE_VER="1.8.6"
GCC_REL="7.3.0-atmel3.6.1-arduino7"
CLI_VER="1.5.1"

if [[ -z "$CLI" ]]; then
  if [[ -x /tmp/arduino-cli-bin/arduino-cli ]]; then CLI=/tmp/arduino-cli-bin/arduino-cli
  elif [[ -x "$HOME/bin/arduino-cli" ]]; then CLI="$HOME/bin/arduino-cli"
  else CLI="$(command -v arduino-cli || true)"; fi
fi

fail() { echo "FAIL  $*" >&2; exit 1; }

[[ -n "$CLI" && -x "$CLI" ]] || fail "arduino-cli not found (set ARDUINO_CLI). Missing toolchain is a failure, not a skip."
VER="$("$CLI" version 2>&1 || true)"
echo "$VER" | grep -q "Version: ${CLI_VER}" || fail "arduino-cli is not ${CLI_VER}: $VER"

DATA="${ARDUINO_DIRECTORIES_DATA:-$HOME/.arduino15}"
CORE_LIST="$("$CLI" core list 2>&1 || true)"
echo "$CORE_LIST" | grep -Eq "^${CORE_ID}[[:space:]]+${CORE_VER}([^0-9]|$)" \
  || fail "core ${CORE_ID}@${CORE_VER} is not installed. core list: ${CORE_LIST}"

GCC="$DATA/packages/arduino/tools/avr-gcc/${GCC_REL}/bin/avr-gcc"
SIZE="$DATA/packages/arduino/tools/avr-gcc/${GCC_REL}/bin/avr-size"
[[ -x "$GCC" && -x "$SIZE" ]] || fail "pinned avr-gcc not at $GCC"
"$GCC" --version | head -1 | grep -q '7\.3\.0' || fail "avr-gcc --version is not 7.3.0: $("$GCC" --version | head -1)"

rm -rf "$OUT"
mkdir -p "$OUT/logs" "$OUT/elf"
echo "$VER" > "$OUT/logs/arduino-cli-version.txt"
echo "$CORE_LIST" > "$OUT/logs/core-list.txt"
"$GCC" --version > "$OUT/logs/avr-gcc-version.txt"
cp "$DATA/packages/arduino/hardware/avr/${CORE_VER}/platform.txt" "$OUT/logs/platform.txt"

declare -A BUDGET
while read -r name bytes; do
  [[ -z "${name:-}" || "$name" == \#* ]] && continue
  [[ "$bytes" =~ ^[0-9]+$ ]] || fail "bad budget line: $name $bytes"
  BUDGET[$name]=$bytes
done < "$CI_DIR/flash/budgets.txt"
[[ ${#BUDGET[@]} -ge 1 ]] || fail "no budgets in budgets.txt"

# gate_log <logfile> <budget>  -> prints the byte count, exit 1 if over or unparsed
gate_log() {
  local log="$1" budget="$2" n
  n="$(grep -oE 'Sketch uses [0-9]+ bytes' "$log" | grep -oE '[0-9]+' | head -1 || true)"
  [[ -n "$n" ]] || return 2
  if [[ "$n" -gt "$budget" ]]; then
    echo "$n"
    return 1
  fi
  echo "$n"
  return 0
}

build_one() {
  local name="$1"
  local extra="${2:-}"
  local src="$CI_DIR/flash/sketches/${name}.ino.txt"
  [[ -f "$src" ]] || fail "missing sketch $src"
  [[ -n "${BUDGET[$name]:-}" ]] || fail "no budget for $name (refusing to skip)"
  local sk="$OUT/sketch/$name/$name"
  mkdir -p "$sk" "$OUT/build/$name"
  cp "$src" "$sk/${name}.ino"
  local log="$OUT/logs/${name}.log"
  local -a cmd=("$CLI" compile --fqbn "$FQBN" --library "$ROOT"
      --build-path "$OUT/build/$name"
      --build-property "compiler.c.elf.extra_flags=-Wl,-Map,$OUT/build/$name/flash.map")
  # Optional second property, used to pass -DSI5351_NO_CLK67 into si5351.cpp.
  # A #define in the .ino does not reach the library translation unit.
  if [[ -n "$extra" ]]; then
    cmd+=(--build-property "$extra")
  fi
  if ! "${cmd[@]}" "$sk" > "$log" 2>&1; then
    fail "$name did not compile (see $log)"
  fi
  local elf="$OUT/build/$name/${name}.ino.elf"
  local map="$OUT/build/$name/flash.map"
  [[ -f "$elf" && -f "$map" ]] || fail "$name produced no ELF or map"
  cp "$elf" "$OUT/elf/${name}.elf"
  cp "$map" "$OUT/elf/${name}.map"

  local sketch_bytes text data bss
  sketch_bytes="$(grep -oE 'Sketch uses [0-9]+ bytes' "$log" | grep -oE '[0-9]+' | head -1 || true)"
  [[ -n "$sketch_bytes" ]] || fail "$name: could not parse 'Sketch uses N bytes'"
  # avr-size default columns: text data bss
  read -r text data bss < <("$SIZE" "$elf" | awk 'NR==2 {print $1, $2, $3}')
  [[ -n "${text:-}" && -n "${data:-}" && -n "${bss:-}" ]] || fail "$name: avr-size parse failed"
  local sum=$((text + data))
  [[ "$sum" -eq "$sketch_bytes" ]] || fail "$name: Sketch uses $sketch_bytes but avr-size text+data is $sum"
  if [[ "$sketch_bytes" -gt "${BUDGET[$name]}" ]]; then
    fail "$name uses $sketch_bytes bytes, budget is ${BUDGET[$name]} (not raised)"
  fi
  echo "PASS  $name $sketch_bytes bytes <= budget ${BUDGET[$name]} (text=$text data=$data bss=$bss)"
  echo "$name $sketch_bytes ${BUDGET[$name]} $text $data $bss" >> "$OUT/measured.tsv"
}

build_one min_const
build_one min_runtime
build_one min_runtime_no_clk67 "compiler.cpp.extra_flags=-DSI5351_NO_CLK67"
# Sketch-side opt-out: the #define is in the .ino, no compiler flag.
build_one min_runtime_sketch_no_clk67

# Negative control: the same parser must reject an over-budget line and a
# real log scored against a 1-byte ceiling. Success of this script requires
# both probes to fail the gate.
probe() {
  local label="$1" log="$2" budget="$3" rc=0
  gate_log "$log" "$budget" >/dev/null || rc=$?
  [[ "$rc" -eq 1 ]] || fail "negative probe '$label' did not fail over budget (rc=$rc)"
  echo "PASS  negative probe rejected $label"
}
printf 'Sketch uses 999999 bytes of program storage space.\n' > "$OUT/logs/synthetic-over.log"
probe "synthetic 999999 vs budget 1" "$OUT/logs/synthetic-over.log" 1
probe "min_const log vs budget 1" "$OUT/logs/min_const.log" 1
# A missing size line is also a failure (rc 2), not a skip.
printf 'compilation finished\n' > "$OUT/logs/no-size.log"
rc=0
gate_log "$OUT/logs/no-size.log" 999999 >/dev/null || rc=$?
[[ "$rc" -eq 2 ]] || fail "unparsed size line was not a hard failure (rc=$rc)"
echo "PASS  unparsed size line is a failure"
echo "ALL PASS"
exit 0
