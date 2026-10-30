# MUST_NOT_BREAK: behaviours the v2.3.1 fix must preserve

The checks are in `test_v231.py` (`run_mnb`), with locks in `must_not_break.json`. They run on the same JSONL as the acceptance tests. **Current status on 1ddd43e: 36 scenarios × 2 register patterns, 776 checks, 0 failures** (`logs/test_v231_master.log`).

Each scenario is checked in two layers:
1. **Golden layer** (pattern 0): every `lib_*` register field from the existing 67-case suite (`../golden_cases.json`) for the mapped golden id.
2. **1ddd43e lock** (both patterns): the full locked register set is checked: reg 3, 15, 16-23 (CLK ctrl), 26-41 (PLLA/PLLB), 42-89 (MS0-5), 90-92, 162-164 (VCXO), 165-170 (phase), 183. The lock also covers return codes, `plla_freq`/`pllb_freq`/`clk_freq[]`/`pll_assignment[]`, and the exact sequence of PLL-reset (reg 177) writes.

Separate evidence that 1ddd43e still matches the existing goldens: the Coding PM harness (`/workspace/si5351-equiv/harness.cpp`) was rebuilt against the pinned 1ddd43e source and checked with `compare.py`. Result: 27 original goldens (292 field checks) and 23 newer lib_* goldens (343 register checks), **0 mismatches** (`logs/goldens_vs_master_report.txt`). The 17 goldens that harness lacks (F-, M-, A-, phase/xtal) are covered below.

## Locked scenarios
| Group | Scenario | Golden id | What is locked |
|---|---|---|---|
| FREQ_CHECKS #1 | clk0-10MHz | P0-clk0-common-2-10MHz | MS0 80/0/1, P1 9728, PLLA 800 MHz untouched |
| FREQ_CHECKS #2 | clk0-14p07MHz | P0-clk0-fractional-MS-14p07MHz | fractional MS P 6765/896192/1e6 |
| FREQ_CHECKS #4 | clk0-100MHz | F-clk0-100MHz-share-max-boundary | ≤100 MHz path keeps PLL, P 512/0/1 |
| FREQ_CHECKS #5 | clk0-112p5MHz | S-boundary-112p5MHz-ratio8 | >100 MHz PLL recalc → 900 MHz |
| DIVBY4 | clk0-150MHz-divby4 / clk0-200 / clk3-200 | P0-…150MHz, P0-…200MHz, S-divby4-block-clk3 | reg44 0x0C, MS_INT, P 0/0/1, PLL 600/800 MHz |
| R div | clk0-4kHz, clk2-5kHz, clk0-512kHz | F-clk0-4kHz, S-rdiv-placement-clk2, P0-rdiv-1-512kHz | R=128 placement, R=1 boundary |
| CLK6/7 | clk6-23MHz, clk7-87p4-after-clk6, clk6-100kHz-R6, clk7-50kHz-R7, clk6-150MHz-clamp | P0-clk6-23MHz, P0-clk7-874…, S-clk6-R6div, S-clk7-R7div, S-clk6-150MHz | PLLB 874 MHz (34/960000/1e6), reg90=38, reg91=10, reg92 R fields, 150 MHz clamp → MS6=6 |
| CLK7 rejects | clk7-reject-12p345678, clk7-reject-odd-ratio | P0-clk7-reject-noninteger | `set_freq` returns 1 and leaves reg91 untouched (non-integer / odd ratio) |
| manual | manual-clk1-175-700-divby4, manual-clk0-14-800-parity | P0-manual-DIVBY4-175MHz-pll700, P1-parity-14MHz | legal manual calls keep their registers and return 0 |
| PLL assignment | pll-assign-after-reset, clk7-pll-assignment-lock, clk1-pllb-14MHz | M-clk7-pllb-assignment, M-clk5-reset-plla, P0-clk1-PLLB | after reset CLK0-5→PLLA (reg16-21=0x0C), CLK6/7→PLLB (0x2C); CLK7 first-set uses PLLB |
| RMW | set_ms-byte2-rmw-5kHz | M-set-ms-byte2-rmw | reg44 = 0xF2 with bit7 preset (byte-2 read-modify-write) |
| 2.3.0 #65 | divby4-exit-reset-155-96, divby4-exit-reset-156-80 | (new in 2.3.0; locked from 1ddd43e) | PLL reset on DIVBY4 exit, ratio<8 re-plan back to 800 MHz, reg177 write sequence |
| 2.3.0 move | low-clock-moves-155-clk0-99-clk2 | (new in 2.3.0) | CLK2 moves to PLLB (reg18 0x2C, pll_assignment[2]=1), PLLA stays at 620 MHz |
| VCXO | vcxo-876-40ppm | P0-vcxo-876MHz-40ppm | PLLB regs [66,64,0,15,133,241,212,192] and VCXO param regs 162-164 |
| phase/int | phase-clk0-mask, phase-clk5, setint-clk0 | S-phase-clk0-mask, A-set_int-clk0-ms-int | CLK0-5 phase regs 165-170 still written; set_int(CLK0) → reg16 0x4C |
| xtal | xtal-6pF / 8pF / 10pF | S-xtal-load-6-8-10pF, A-init-crystal-load-8pF | reg183 = 0x52 / 0x92 / 0xD2 |
| CLKIN | clkin-div4-clean | A-set_pll_input-clkin-div4-clean | reg15 = 0x84 from a clean start |
| refs/corr | ref-27MHz, corr-100000-14MHz | A-ref27MHz-set_pll-fixed, P0-corr-100000-14MHz | 27 MHz ref PLL regs, +100 ppm correction PLL/MS regs |

## Changes the fix is ALLOWED to make (explicitly unlocked)
- `reg3` bit 6/7 (CLK6/7 OEB) in the CLK6/7 scenarios. This is the DEV-14 fix; the other reg3 bits stay locked.
- `state.pllb_freq` in vcxo-876-40ppm. This is a DEV-08 fix component: 1ddd43e leaves `pllb_freq` at 800 MHz although PLLB runs at 876 MHz. The registers stay locked.

## NOT locked (on purpose)
- `MNB:both-plls-held-return-code` is recorded only. This is item 3 (deferred): return codes for both-PLLs-held / other-PLL-too-low / manual-low-PLL must not be re-baselined by this work.
- `MNB:clk0-0Hz` is recorded only. On 1ddd43e, `set_freq(0)` on CLK0-5 writes an MS for 500 kHz and enables the output. See debatable D2.
- Some goldens are expected to change with the fix, so they are not MNB: `D-clk0-210MHz-above-spec-max`, `D-clk0-250MHz-clamped-to-225`, `D-clk6-10kHz-ms6-p1-zero`, `D-manual-*`, `D-vcxo-*`, `D-set_phase-clk6-*`, `D-init-crystal-load-0pF-*`, `D-set_int-clk6-*`, `D-set_pll_input-*` and `D-ref5MHz-*`. The DEV-14 reg3 bits in P0-clk6/7 and S-clk6/7 are also expected to change. **Heads-up:** `../test_golden.py` locks v2.2.0 library behaviour bit-for-bit, so it WILL report these as diffs after the fix. That is intended; those cases are xfail in `../test_spec.py` and should flip to xpass.

## Sensitivity
On v2.2.0 (0559cbf) the same checks report 68 failures, all in the 2.3.0 scenarios (`logs/test_v231_v220.log`). The 1ddd43e locks detect a regression to pre-2.3.0 behaviour.
