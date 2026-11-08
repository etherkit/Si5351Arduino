# Si5351Arduino v2.3.1 pre-review checks (`run_v231_checks.sh`)

One self-contained, repeatable entry point for the v2.3.1 acceptance suite and the regression locks.
Everything it needs is vendored here: no `/workspace` paths and no network access except the optional
read-only git fetch and arduino-cli core installs. It never commits, pushes, or writes to the library repo.
Library sources are always pinned with `git archive` (or copied, for a plain tree) into `<out>/lib`.

## Requirements
bash, git, tar, g++ (or `CXX=clang++`) with C++17, python3 (3.8+, standard library only).
Optional: `arduino-cli` (≥1.0) for `--cores`; it is looked up as `$ARDUINO_CLI`, then `~/bin/arduino-cli`, then `PATH`.
User-level install: `curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | BINDIR=~/bin sh`

## Usage
```
./run_v231_checks.sh                                  # etherkit/Si5351Arduino master HEAD (read-only shallow fetch)
./run_v231_checks.sh --ref abf9b2a                    # any branch/tag/SHA from upstream (GitHub serves SHAs)
./run_v231_checks.sh --repo https://github.com/<fork>/Si5351Arduino.git --ref twowire-bus-select
./run_v231_checks.sh --src ~/src/Si5351Arduino --ref v2.3.1-fixes   # a local clone; ref archived, clone untouched
./run_v231_checks.sh --src ~/src/Si5351Arduino        # local clone at HEAD (committed state; uncommitted edits are flagged, not tested)
./run_v231_checks.sh --src /path/to/tree              # plain source tree (src/si5351.{cpp,h} or si5351.{cpp,h})
./run_v231_checks.sh --require-fixed ...              # v2.3.1 gate: all 25 acceptance tests must PASS
./run_v231_checks.sh --cores                          # + arduino-cli compiles on all 9 boards (installs missing cores into ~/.arduino15)
./run_v231_checks.sh --cores uno,esp32 --out /tmp/r1  # subset of boards, explicit output dir
```
Exit code: 0 = every check PASS/XFAIL/SKIP/INFO; 1 = at least one unexpected FAIL; 2 = usage/setup error.

## What runs
| Check | What | Gating |
|---|---|---|
| `build_acceptance`, `run_acceptance` | `v231/acceptance.cpp` + `scenarios_v231.inc` (per-scenario fork, so SIGFPE is recorded) compiled with the host compiler against the pinned source and the Wire stub | yes |
| `acceptance_A01..A25` | `v231/test_v231.py` AN619/datasheet oracle. **Default mode: a failing test is XFAIL** (master is expected to fail all 25). `--require-fixed`: a failing test is FAIL | per mode |
| `must_not_break` | 776 register/state/return/reg177 locks from `v231/must_not_break.json` | yes |
| `t106_twowire` | issue #106 TwoWire bus-selection test (`issue106/t106.cpp`, multi-bus stub). SKIP when the source has no `TwoWire*` constructor | yes |
| `rdiv_xtal` | CLK0-5 R-divider octaves and CLK6/7 `set_freq()` register 92 edges (`rdiv/t_rdiv.cpp`, issue #89), Register 183 XTAL_CL, and README numbers vs the header / `SI5351_FOUT_MAX` | yes |
| `reset_freq` | `reset_freq()` host test (`reset_freq/t_reset_freq.cpp`): re-enable on the next `set_freq()`, invalid index is a no-op, other clocks untouched. SKIP when the source has no `reset_freq()` | yes |
| `ctrl_status` | Exact register bytes and ordered I2C for every legal `drive_strength`, `set_clock_pwr` / `invert` / `source` / `disable` / `fanout` value, status-bit decode, NACK, and `requestFrom` with no payload (`ctrl/t_ctrl.cpp`) | yes |
| `plan_branches` | CLK6/7 `set_freq_manual` rejects, `multisynth67_calc` a±2 and the uncalled pll≠0 arm, `set_ms` CLK6/7 and DIVBY4 exit, `set_vcxo` ppm/`ref_ok`, `set_pll` clamp (`plan/t_plan.cpp`). Locks current v2.4.2 behavior, including the uint8 p1 truncation | yes |
| `goldens27_vs_lib` | C++ equivalence harness (`equiv/harness.cpp`) on the pinned source vs the 27 original golden cases | yes (INFO under `--require-fixed`) |
| `v242_provenance`, `equiv_vs_v242` | `sha256sum -c` of `reference/v242_f885fca/` (git show of f885fca, not a fetch of master), then a full harness transcript diff. 0 differing runs is PASS; any difference is FAIL. `reference/master_1ddd43e/` is no longer the behavior gate | yes |
| `goldens_newer_vs_lib` | Newer `lib_*` golden register maps | INFO |
| `py_test_golden`, `py_test_spec` | Python golden/spec suites: self-consistency of the AN619 oracle and goldens used above (library-independent) | yes |
| `cxx98_syntax` | `-std=c++98 -pedantic -fsyntax-only` warning count | INFO |
| `compile_<board>_<sketch>` (`--cores`) | arduino-cli `--warnings all` compile of `si5351_example`, `si5351_vcxo` and (TwoWire sources only) `sketches/Ctor106` on `boards.txt`. A vcxo `get_correction` error in default mode is XFAIL (known v2.3.0 example bug, fixed in 2.4.0) | yes |

## Notes
- Version-manager shims (mise, asdf, pyenv) on `PATH` can make cores that call `python3` during the build (esp8266, rp2040) fail with a "shim fallback recursion" error. When a real `python3` is still reachable without them, the script runs arduino-cli with those shim directories removed from `PATH`.
- Cores are installed per user into `~/.arduino15`. Nothing is installed system-wide and sudo is never used.

## Output (`--out`, default `./runs/run_<date>_<time>_<pid>`)
`summary.md`, `summary.json` (overall, pinned SHA, source, mode, UTC timestamps, host and toolchain, per-check status/detail/log),
`results.tsv`, `meta.env`, `PINNED_SHA`, `PINNED_SHA256SUMS`, `logs/` (build logs, JSONL transcripts, test logs, compile logs), `lib/` (pinned tree), `build/`.

## Layout
`v231/` acceptance runner, scenarios, oracle, locks, and reports · `stubs/` Arduino/Wire host stubs · `issue106/` t106 and its multi-bus stubs · `reset_freq/` `reset_freq()` regression ·
`equiv/` harness, goldens snapshot, compare.py · `reference/v242_f885fca/` pinned v2.4.2 source (the hard gate) · `reference/master_1ddd43e/` historical source, not the gate ·
`ctrl/` `plan/` branch locks · `flash/check_flash_budget.sh` Uno byte ceilings, including the `SI5351_NO_CLK67` compiler-flag and sketch-#define builds (separate CI workflow) · `python/` golden/spec suites and refs ·
`sketches/` Ctor106 · `boards.txt` board set and Boards Manager URLs · `tools/summarize.py`.
`v231/Makefile` is the original manual build, kept for reference only; the script does not use it.
