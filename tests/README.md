# Tests

`regression/` is the Si5351 regression harness written by Test Lab (an agent working for Jason Milldrum, NT7S),
ported unchanged from its working copy. It compiles `src/si5351.cpp` natively with g++ against host
Arduino/Wire stubs and checks register output against golden cases, AN619/datasheet oracles, and the
v2.3.1 acceptance scenarios. See `regression/README.md`.

Run locally: `cd tests/regression && ./run_v231_checks.sh --src ../..`

In CI (`.github/workflows/regression.yml`) it is **report-only**: not a required check, never gates merges.
In default mode, the 25 v2.3.1 acceptance tests are expected to XFAIL on master (they are bugs v2.3.1 fixes).
