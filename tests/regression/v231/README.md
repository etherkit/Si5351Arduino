# v231: Si5351Arduino v2.3.1 acceptance tests

Local only (no push, no PR). Library under test: **master 1ddd43e (v2.3.0)**, pinned in `master_1ddd43e/` with `git show`.

| File | Purpose |
|---|---|
| `scenarios_v231.inc` | Call sequences in the Coding PM harness DSL (`init_/sf/sfm/svcxo/corr_/mssrc/pllin/reff`, plus `phase_/setint_/wr_`). 25 acceptance + 38 must-not-break/info scenarios |
| `acceptance.cpp` | Runner. Same stubs and JSONL record format as `/workspace/si5351-equiv/harness.cpp`, plus a per-scenario `fork()` so crashes are recorded (`"crash":"Floating point exception"`) |
| `Makefile` | `build/acc_master` (pinned 1ddd43e), `build/acc_v220` (0559cbf from `si5351-equiv/orig`); `make run` |
| `test_v231.py` | Python oracle. Decodes registers (AN619 P1/P2/P3, VCO, MS6/7, R, OEB), runs 25 acceptance tests and the must-not-break checks |
| `v231_acceptance.json` | Golden JSON: per test, the calls, the expected-correct values with AN619/datasheet citations, the 1ddd43e observed values and the debatable options |
| `must_not_break.json` | 1ddd43e register/state/return/reg177 locks for the MNB scenarios |
| `FAILS_ON_MASTER.md` · `MUST_NOT_BREAK.md` · `DEBATABLE.md` | Reports |
| `proto_fix/` | TEST-VALIDATION ONLY patch showing that all 25 tests can pass with the MNB locks green. Not a proposed fix |
| `snap_wip_*/` | Snapshot of the live `v2.3.1-fixes` working tree that another agent was editing during this run (informational) |
| `logs/` | Build logs, raw JSONL, and test logs for master, v2.2.0, proto and wip; harness goldens report |

## Run
```
make run                                                     # build both, run, test master
python3 test_v231.py logs/acc_master.jsonl --expect-master   # 1ddd43e: all 25 FAIL, MNB green -> exit 0
make build/acc_master MASTER_SRC=/workspace/Si5351Arduino/src && ./build/acc_master logs/acc_fix.jsonl
python3 test_v231.py logs/acc_fix.jsonl --require-fixed      # v2.3.1 gate: all 25 PASS + MNB green -> exit 0
```
(When building against the live tree, use a separate output name and avoid overwriting `build/acc_master`, or rebuild it afterwards from the pinned copy.)

## C++ harness integration
`scenarios_v231.inc` uses only the harness wrappers, plus three small ones defined in `acceptance.cpp` (`phase_`, `setint_`, `wr_`). To fold it into `si5351-equiv/harness.cpp`, copy those three wrappers and `#include "scenarios_v231.inc"` inside `build()`. Note that harness.cpp's `main` has no fork(), so A09/A10 would SIGFPE and kill the whole run on 1ddd43e. Keep them in this runner or guard them there. The harness `compare.py` equivalence diff then reports every acceptance scenario as a DIFF between master and the fix (expected).
