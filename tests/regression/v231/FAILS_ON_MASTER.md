# FAILS_ON_MASTER: v2.3.1 acceptance tests vs master 1ddd43e

Evidence comes from real runs. `acceptance.cpp` with `scenarios_v231.inc` was compiled against the **pinned 1ddd43e source** (`master_1ddd43e/`, extracted with `git show 1ddd43e:src/si5351.{cpp,h}`; SHA-256 in `master_1ddd43e/SHA256SUMS`). The live `/workspace/Si5351Arduino` working tree was not used, because another agent is committing on `v2.3.1-fixes`. The stubs are the Coding PM harness stubs (`/workspace/si5351-equiv/stubs`). Each scenario runs in a forked child on both register-file patterns (p0 zero-filled, p1 pattern-filled).

Run log: `logs/test_v231_master.log` (`python3 test_v231.py logs/acc_master.jsonl --expect-master`, exit 0). Raw output: `logs/acc_master.jsonl`.

**Result: 25/25 acceptance tests FAIL on 1ddd43e, on both patterns. Must-not-break: 776/776 checks pass.**

Observed values are from p0. p1 is identical except where noted.

| Test | Item | Call sequence | Expected (correct) | Observed on 1ddd43e |
|---|---|---|---|---|
| **A01** FAIL | 1 DEV-03 | `init0(); sf(10000 * M, 6);` | every reg90 write by set_freq is an even integer 6..254: even 6..254 (AN619 Reg 90, AN619.txt:2696); stored pllb_freq within VCO 600..900 MHz: 600..900 MHz (AN619.txt:37); either rejected (ret 1, nothing written) or clamped to >= CLKOUT67_MIN 18454 Hz: reject or clamp>=18454 Hz (DEBATABLE D1) | ['0x00']; 128 MHz; ret=0 clk_freq[6]=4000 Hz realized=None Hz |
| **A02** FAIL | 4 CLK67 clamp | `init0(); sf(15000 * M, 6);` | every reg90 write by set_freq is an even integer 6..254: even 6..254 (AN619 Reg 90, AN619.txt:2696); stored pllb_freq within VCO 600..900 MHz: 600..900 MHz (AN619.txt:37); either rejected (ret 1, nothing written) or clamped to >= CLKOUT67_MIN 18454 Hz: reject or clamp>=18454 Hz (DEBATABLE D1) | ['0x00']; 128 MHz; ret=0 clk_freq[6]=4000 Hz realized=None Hz |
| **A03** FAIL | 1 DEV-15 | `init0(); sfm(87400000 * M, 874000000 * M, 7);` | reg91 (MS7_P1): 0x0A (AN619.txt:247 'set MSx_P1 directly (= divide value)'; 874/87.4 = 10); realized CLK7: 87.4 MHz | 0x00; None MHz |
| **A04** FAIL | 1 DEV-15 | `init0(); sfm(87400000 * M, 874000000 * M, 6);` | reg90 (MS6_P1): 0x0A (AN619.txt:247 'set MSx_P1 directly (= divide value)'; 874/87.4 = 10); realized CLK6: 87.4 MHz | 0x00; None MHz |
| **A05** FAIL | 2 DEV-14 | `init0(); sf(23000000 * M, 6);` | reg3 CLK6_OEB after set_freq: 0 = enabled (AN619 Reg 3; parity with CLK0-5 first-set enable, si5351.cpp set_freq CLK0-5 branch) | 1 |
| **A06** FAIL | 2 DEV-14 | `init0(); sf(23000000 * M, 6); sf(87400000 * M, 7);` | reg3 CLK6_OEB after set_freq: 0 = enabled (AN619 Reg 3; parity with CLK0-5 first-set enable, si5351.cpp set_freq CLK0-5 branch); reg3 CLK7_OEB after set_freq: 0 = enabled (AN619 Reg 3; parity with CLK0-5 first-set enable, si5351.cpp set_freq CLK0-5 branch) | 1; 1 |
| **A07** FAIL | 2 DEV-14 | `init0(); sf(23000000 * M, 7);` | reg3 CLK7_OEB after set_freq: 0 = enabled (AN619 Reg 3; parity with CLK0-5 first-set enable, si5351.cpp set_freq CLK0-5 branch) | 1 |
| **A08** FAIL | 4 uninit | `init0(); sf(23000000 * M, 6); sf(1000000 * M, 7);` | set_freq(1 MHz, CLK7) return: 1: 874 MHz/1 MHz = 874 = 2x437, no even MS7 6..254 x R=2^k exists (AN619.txt:74, Reg 92 R7); no write to reg 91 (uninitialised ms_reg.p1): no write; clk_freq[7] unchanged: 0 | 0; ['0xFE']; 1000000.0 |
| **A09** FAIL | 4 div0 | `init0(); sf(23000000 * M, 6); sf(0, 7);` | set_freq(0, CLK7) with peer set does not crash: no crash (no '% 0') | Floating point exception |
| **A10** FAIL | 4 div0 | `init0(); sf(23000000 * M, 7); sf(0, 6);` | set_freq(0, CLK6) with peer set does not crash: no crash (no '% 0') | Floating point exception |
| **A11** FAIL | 4 fmax | `init0(); sf(210000000 * M, 0);` | realized CLK0 <= 200 MHz (or rejected: ret 1, MS0 not written): <= 200 MHz (DS p1 / AN619.txt:38; DEBATABLE D3 reject vs clamp); stored clk_freq[0] <= 200 MHz: <= 200 MHz | ret=0 realized=210.0 MHz; 210 MHz |
| **A12** FAIL | 4 fmax | `init0(); sf(225000000 * M, 0);` | realized CLK0 <= 200 MHz (or rejected: ret 1, MS0 not written): <= 200 MHz (DS p1 / AN619.txt:38; DEBATABLE D3 reject vs clamp); stored clk_freq[0] <= 200 MHz: <= 200 MHz | ret=0 realized=225.0 MHz; 225 MHz |
| **A13** FAIL | 4 fmax | `init0(); sfm(225000000 * M, 900000000 * M, 0);` | realized CLK0 <= 200 MHz (or rejected: ret 1, MS0 not written): <= 200 MHz (DS p1 / AN619.txt:38; DEBATABLE D3 reject vs clamp); stored clk_freq[0] <= 200 MHz: <= 200 MHz | ret=0 realized=225.0 MHz; 225 MHz |
| **A14** FAIL | 4 ms ratio | `init0(); sfm(140000000 * M, 700000000 * M, 0);` | MS0 ratio valid (or call rejected without writing MS0): 4, 6, 8 or 8+1/1048575..2048 (AN619.txt:58,235; DEBATABLE D4) | ret=0 ratio=5 |
| **A15** FAIL | 4 ms ratio | `init0(); sfm(90000000 * M, 600000000 * M, 0);` | MS0 ratio valid (or call rejected without writing MS0): 4, 6, 8 or 8+1/1048575..2048 (AN619.txt:58,235; DEBATABLE D4) | ret=0 ratio=6.66667 |
| **A16** FAIL | 4 ms ratio | `init0(); sfm(100000000 * M, 700000000 * M, 0);` | MS0 ratio valid (or call rejected without writing MS0): 4, 6, 8 or 8+1/1048575..2048 (AN619.txt:58,235; DEBATABLE D4) | ret=0 ratio=7 |
| **A17** FAIL | 4 phase | `init0(); phase_(6, 10);` | no write to reserved reg 171: none (AN619.txt:582 'Reg 171-176 Reserved') | [10] |
| **A18** FAIL | 4 phase | `init0(); phase_(7, 10);` | no write to reserved reg 172: none (AN619.txt:582 'Reg 171-176 Reserved') | [10] |
| **A19** FAIL | 4 set_int | `init0(); corr_(100000, 0); setint_(6, 1);` | reg22 bit6 (FBA_INT) unchanged by set_int(CLK6): bit6 unchanged: FBA_INT only for even-integer feedback (AN619.txt:138, :1234); MS6/7 have no INT bit | 0x6C (before 0x2C) |
| **A20** FAIL | 4 set_int | `init0(); corr_(100000, 0); setint_(7, 1);` | reg23 bit6 (FBB_INT) unchanged by set_int(CLK7): bit6 unchanged: FBB_INT only for even-integer feedback (AN619.txt:138, :1234); MS6/7 have no INT bit | 0x6C (before 0x2C) |
| **A21** FAIL | 4 clkin div | `init0(); reff(50000000, 1); pllin(0, 1); reff(80000000, 1); pllin(0, 1);` | reg15 CLKIN_DIV[1:0] after 80 MHz CLKIN: 10b = /4 (AN619.txt:834-840; 80 MHz needs /4 for 10-40 MHz, AN619.txt:94) | 11b (reg15=0xC4) (p1: 11b (reg15=0xD7)) |
| **A22** FAIL | 4 0pF | `init_(SI5351_CRYSTAL_LOAD_0PF, 0, 0);` | reg183: XTAL_CL != 00b, bits5:0 = 010010b (AN619.txt:3134-3141; DEBATABLE D5 which value) | 0x12 (XTAL_CL=00b) |
| **A23** FAIL | 4 vcxo | `init0(); svcxo(876000000 * M, 40); corr_(0, 0);` | MSNB c (P3): 1000000 (AN619.txt:418 'c ... fixed to 10^6'); PLLB VCO: 876 MHz (set_vcxo value preserved); PLLB regs 34..41 == set_vcxo() result: [66, 64, 0, 15, 133, 241, 212, 192] | 1; 800.0 MHz; [0, 1, 0, 14, 0, 0, 0, 0] |
| **A24** FAIL | 4 vcxo | `init0(); svcxo(876000000 * M, 40); pllin(0, 0);` | MSNB c (P3): 1000000 (AN619.txt:418 'c ... fixed to 10^6'); PLLB VCO: 876 MHz (set_vcxo value preserved); PLLB regs 34..41 == set_vcxo() result: [66, 64, 0, 15, 133, 241, 212, 192] | 1; 800.0 MHz; [0, 1, 0, 14, 0, 0, 0, 0] |
| **A25** FAIL | 4 DEV-13 | `init_(L8, 5000000, 0);` | every PLLA/PLLB feedback block written has a+b/c in 15..90: 15..90 (AN619.txt:116; DEBATABLE D6 for the 5 MHz policy) | [(26, 120.0), (26, 120.0), (26, 160.0), (34, 120.0), (34, 120.0), (34, 160.0)] |

Notes
- A08: the build uses `-ftrivial-auto-var-init=pattern`, so the uninitialised `ms_reg.p1` reads as 0xFE (it looks like a legal 254). On an AVR it is stack garbage. The test checks for *no write* to reg 91 and `ret == 1`, so the result does not depend on the garbage value. R7 is also written as /4 (reg92 = 0x20).
- A09/A10: the child process dies with SIGFPE (`pllb_freq % 0`). On an AVR the result of the division is undefined (no trap).
- A25: the blocks with ratio 120 come from `init()` → `set_correction()` → `set_pll(plla_freq = 0)` before `reset()` (VCO clamped to 600 MHz / 5 MHz = 120). Ratio 160 is from `reset()` → `set_pll(800 MHz)`.
- A13 shows that clamping `freq` alone is not enough: `set_freq_manual(225 MHz, 900 MHz)` forces DIVBY4, so the output is PLL/4 = 225 MHz whatever the clamped `freq` is (overlaps DEV-06).

## The tests can pass (validation)
`proto_fix/` is a TEST-VALIDATION-ONLY patch of the pinned source (`proto_fix/proto_patch.py`, `proto_fix/proto.diff`). It picks one arbitrary option for each debatable item and is not a proposed fix. Result: **25/25 acceptance pass, 776/776 must-not-break pass** (`logs/test_v231_proto.log`, `--require-fixed`, exit 0). So no test is unsatisfiable and none contradicts the must-not-break locks.

## Informational: live `v2.3.1-fixes` working tree (snapshot)
`snap_wip_*/` is a copy of `/workspace/Si5351Arduino/src` (HEAD 8f3fef6 plus uncommitted edits) taken during this run. Result: 21/25 pass. **A13, A15, A16 and A25 still fail.** Must-not-break: 0 failures (`logs/test_v231_wip.log`). This is a moving target; re-run before relying on it.

## Reference: v2.2.0 (0559cbf)
`logs/test_v231_v220.log`: 0/25 acceptance pass. Must-not-break has 68 failures, all in the three 2.3.0 behaviours (#65 DIVBY4-exit reset, ratio<8 re-plan, low clock moves to the other PLL). This is expected and shows that those locks detect a regression to 2.2.0 behaviour.

