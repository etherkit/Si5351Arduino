#!/usr/bin/env python3
"""SPEC conformance: evaluate every library case against the independent
AN619/datasheet oracle.

Outcomes per case:
  PASS   no error-severity rule failed and none expected
  XFAIL  exactly the expected (known-deviation) rules failed  -> library behaviour
         is locked, spec deviation is documented in DEVIATION_CATALOG
  XPASS  a known deviation no longer reproduces -> FAIL (strict): the library
         changed behaviour; update goldens + catalogue deliberately
  FAIL   unexpected spec failure, register-encoding disagreement, or the stored
         spec block drifted
Exit code 0 iff no FAIL/XPASS.
"""
from __future__ import annotations
import json, sys
from pathlib import Path

import si5351_math as L
import spec_oracle as S
from spec_crosscheck import evaluate, KNOWN_DEVIATIONS, DEVIATION_CATALOG

ROOT = Path(__file__).resolve().parent


def norm(o):
    return json.loads(json.dumps(o, sort_keys=True, default=str))


def unit_checks():
    """Spot checks of the oracle itself against numbers stated in AN619."""
    assert S.encode_abc(4, 0, 1) == (0, 0, 1)                       # ratio 4 -> P1=0 (DIVBY4 encoding consistent)
    assert S.decode_p(*S.encode_abc(57, 142857, 1_000_000)) == 57 + S.F(142857, 1_000_000)
    assert S.feedback_block("PLLB", 1, 2, 0xABCDE)[39] == 0xA0       # P3[19:16] high nibble of reg 39
    assert S.feedback_block("PLLA", 0x3FFFF, 0xFFFFF, 1)[28] == 0x03 and S.feedback_block("PLLA", 0, 0xF0000, 1)[31] == 0x0F
    assert S.ms_block(0, 0, 0, 1, r=128, divby4=True)[44] == 0x7C
    assert S.reg92(r6=128, r7=128) == 0x77
    assert S.clk_ctrl(0) == 0x0C and S.clk_ctrl(6, src_pllb=1) == 0x2C and S.clk_ctrl(0, int_bit=1) == 0x4C
    assert S.pll_reset_byte(True, True) == 0xA0
    assert S.xtal_cl_byte(3) == 0xD2
    assert S.phase_reg(5, 0xFF) == (170, 0x7F)
    assert S.vcxo_param(32, 0, 100)["floor"] == 421888                # 1.03*128*32*100 exactly
    assert S.phase_word(S.F(1, 10**9), S.F(800_000_000)) == 3         # 1 ns at 800 MHz = 3.2 LSB -> 3 (DS PSTEP ~333 ps)


def main() -> int:
    unit_checks()
    stored = {c["id"]: c for c in json.loads((ROOT / "golden_cases.json").read_text())["cases"]}
    cases = L.build_golden_suite() + L.build_spec_coverage_cases()
    counts = {"PASS": 0, "XFAIL": 0, "XPASS": 0, "FAIL": 0}
    lines = []
    for c in cases:
        s = evaluate(c)
        exp = KNOWN_DEVIATIONS.get(c["id"], {}).get("rules", set())
        got = set(s["failed_rules"])
        problems = []
        if s["register_agreement"] is False:
            problems.append("register encoding disagrees with AN619")
        if c["id"] in stored and norm(stored[c["id"]].get("spec")) != norm(s):
            problems.append("stored spec block drifted (regenerate deliberately)")
        if got - exp:
            problems.append(f"unexpected spec failures {sorted(got - exp)}")
        if exp - got:
            outcome = "XPASS"
            problems.append(f"expected deviation no longer reproduces: {sorted(exp - got)}")
        elif problems:
            outcome = "FAIL"
        elif exp:
            outcome = "XFAIL"
        else:
            outcome = "PASS"
        counts[outcome] += 1
        devs = ",".join(KNOWN_DEVIATIONS.get(c["id"], {}).get("devs", []))
        lines.append(f"{outcome:5s} {c['id']}" + (f"  [{devs}]" if devs else "") +
                     (f"  !! {'; '.join(problems)}" if problems else ""))
    print("\n".join(lines))
    print()
    print(f"spec oracle unit checks: OK")
    print(f"summary: {counts['PASS']} passed, {counts['XFAIL']} xfailed (known deviations), "
          f"{counts['XPASS']} xpassed, {counts['FAIL']} failed  — {len(cases)} cases, "
          f"{len(DEVIATION_CATALOG)} catalogued deviations")
    return 0 if counts["FAIL"] == 0 and counts["XPASS"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
