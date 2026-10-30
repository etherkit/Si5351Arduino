#!/usr/bin/env python3
"""LIBRARY regression lock: recompute every case from the library mirror and
require bit-identical match with golden_cases.json (the `spec` block is
ignored here; see test_spec.py)."""
from __future__ import annotations
import json, sys
from pathlib import Path

import si5351_math as L

ROOT = Path(__file__).resolve().parent

# APIs Coding PM's C++ harness missed — must remain present + asserted here.
REQUIRED_API_CASES = {
    "set_phase": ["S-phase-clk0-mask", "D-set_phase-clk6-writes-reserved-171", "A-set_phase-clk1-0x2A"],
    "init_xtal_load": ["S-xtal-load-6-8-10pF", "D-init-crystal-load-0pF-reserved", "A-init-crystal-load-8pF"],
    "set_int": ["D-set_int-clk6-sets-FBA_INT-on-fractional-plla", "A-set_int-clk0-ms-int",
                "A-set_correction-then-set_int-clk0"],
    "set_pll_input": ["D-set_pll_input-clkin_div-or-without-clear", "A-set_pll_input-clkin-div4-clean"],
    "set_pll_5mhz_ref": ["D-ref5MHz-pll_calc-stale-a160"],
    "set_ref_freq_27mhz": ["A-ref27MHz-set_pll-fixed"],  # sixth / adjacent API
}

MUTATION_GAP_CASES = [
    "M-clk7-pllb-assignment",
    "M-set-ms-byte2-rmw",
    "M-clk5-reset-plla",
]


def norm(o):
    return json.loads(json.dumps(o, sort_keys=True, default=str))


def main() -> int:
    stored = json.loads((ROOT / "golden_cases.json").read_text())
    fresh = {c["id"]: c for c in L.build_golden_suite() + L.build_spec_coverage_cases()}
    st = {c["id"]: {k: v for k, v in c.items() if k != "spec"} for c in stored["cases"]}
    errors = []
    if stored["assumptions"]["xtal_hz_default"] != L.SI5351_XTAL_FREQ:
        errors.append("xtal assumption drift")
    if set(st) != set(fresh):
        errors.append(f"case id mismatch: missing={sorted(set(st) - set(fresh))} extra={sorted(set(fresh) - set(st))}")
    for cid in sorted(set(st) & set(fresh)):
        if norm(st[cid]) != norm(fresh[cid]):
            errors.append(f"library mismatch in {cid}")

    f = fresh
    # ---- classic anchors ----
    c = f["P0-clk0-common-1-14MHz"]
    assert (c["ms_a"], c["ms_b"], c["ms_c"], c["ms_p1"], c["ms_p2"], c["ms_p3"]) == (57, 142857, 1_000_000, 6802, 285696, 1_000_000)
    assert f["P0-clk6-23MHz"]["pll_freq_hz"] == 874_000_000 and f["P0-clk6-23MHz"]["ms_a"] == 38
    assert f["P0-vcxo-876MHz-40ppm"]["vcxo_bytes"] == [1, 209, 2]
    assert f["P0-clk0-DIVBY4-200MHz"]["ms_a"] == 4
    assert f["S-divby4-block-clk3-200MHz"]["lib_ms_regs"]["68"] == 0x0C
    assert f["S-divby4-block-clk3-200MHz"]["lib_clk_ctrl"]["19"] == 0x4C
    assert f["S-rdiv-placement-clk2-5kHz"]["lib_ms_regs"]["60"] == 0x72
    assert f["S-clk6-R6div-100kHz"]["lib_reg92"] == 0x05 and f["S-clk7-R7div-50kHz"]["lib_reg92"] == 0x60
    assert f["D-clk6-10kHz-ms6-p1-zero"]["lib_ms67_regs"] == {"90": 0}
    assert f["D-manual-clk7-87p4MHz-p1-truncated"]["lib_ms67_regs"] == {"91": 0}
    assert f["P1-parity-set_freq-vs-manual-14MHz-pll800"]["ms_match"] is True
    p2 = f["P1-parity-gt100-set_freq-vs-manual-175MHz"]
    assert p2["ms_match"] is True and p2["pll_match"] is True

    # ---- Mutation gaps (Coding PM) ----
    for cid in MUTATION_GAP_CASES:
        assert cid in f, f"missing mutation-gap case {cid}"

    # M1: CLK7 locked to PLLB (reg 23 = 0x2C); must NOT equal PLLA encoding 0x0C
    m7 = f["M-clk7-pllb-assignment"]
    assert m7["pll_assignment"] == "PLLB"
    assert m7["pll_assignment_table"]["7"] == "PLLB"
    assert m7["lib_write"] == {"reg": 23, "value": 0x2C}
    assert m7["lib_write"]["value"] != m7["mutation_fail_if"]["clk7_ctrl_equals"]  # 0x0C
    assert m7["pll_assignment_table"]["7"] != m7["mutation_fail_if"]["pll_assignment_7"]

    # M2: set_ms + ms_div RMW — final byte 0xF2; naive mutations 0x70 / 0x72 must differ
    mrmw = f["M-set-ms-byte2-rmw"]
    assert mrmw["prior_byte2"] == 0xFF
    assert mrmw["lib_ms_regs"]["44"] == 0xF2
    assert (mrmw["lib_ms_regs"]["44"] & 0x80) == 0x80  # bit7 preserved
    assert (mrmw["lib_ms_regs"]["44"] & 0x03) == 0x02  # P1[17:16]
    assert (mrmw["lib_ms_regs"]["44"] >> 4) & 0x07 == 7  # R_DIV=128
    md = mrmw["rmw_transcript"]["mutation_detectors"]
    assert md["naive_ms_div_overwrite"] == 0x70
    assert md["naive_set_ms_then_correct_ms_div"] == 0x72
    assert mrmw["lib_ms_regs"]["44"] not in (0x70, 0x72)
    assert mrmw["rmw_transcript"]["final"] == 0xF2
    assert "si5351_read" in mrmw["rmw_transcript"]["steps"][0]["op"]

    # M3: CLK5 locked to PLLA (reg 21 = 0x0C); must NOT equal PLLB encoding 0x2C
    m5 = f["M-clk5-reset-plla"]
    assert m5["pll_assignment"] == "PLLA"
    assert m5["pll_assignment_table"]["5"] == "PLLA"
    assert m5["lib_write"] == {"reg": 21, "value": 0x0C}
    assert m5["lib_write"]["value"] != m5["mutation_fail_if"]["clk5_ctrl_equals"]  # 0x2C
    assert m5["pll_assignment_table"]["5"] != m5["mutation_fail_if"]["pll_assignment_5"]

    # ---- Six APIs Coding PM harness missed (explicit automation) ----
    for api, ids in REQUIRED_API_CASES.items():
        for cid in ids:
            assert cid in f, f"API {api}: missing case {cid}"

    # set_phase
    assert f["S-phase-clk0-mask"]["lib_write"] == {"reg": 165, "value": 0x7F}
    assert f["D-set_phase-clk6-writes-reserved-171"]["lib_write"] == {"reg": 171, "value": 10}  # DEV-09
    assert f["A-set_phase-clk1-0x2A"]["lib_write"] == {"reg": 166, "value": 0x2A}

    # crystal load
    assert f["S-xtal-load-6-8-10pF"]["lib_writes"] == {"6pF": 0x52, "8pF": 0x92, "10pF": 0xD2}
    assert f["D-init-crystal-load-0pF-reserved"]["lib_writes"] == {"0pF": 0x12}  # DEV-10
    assert f["A-init-crystal-load-8pF"]["lib_writes"] == {"8pF": 0x92}

    # set_int
    assert f["D-set_int-clk6-sets-FBA_INT-on-fractional-plla"]["lib_write"] == {"reg": 22, "value": 0x6C}  # DEV-11
    assert f["A-set_int-clk0-ms-int"]["lib_write"] == {"reg": 16, "value": 0x4C}
    assert f["A-set_correction-then-set_int-clk0"]["lib_write"] == {"reg": 16, "value": 0x4C}

    # set_pll_input
    assert f["D-set_pll_input-clkin_div-or-without-clear"]["lib_write"]["value"] == 0xC4  # DEV-12
    assert f["A-set_pll_input-clkin-div4-clean"]["lib_write"] == {"reg": 15, "value": 0x84}

    # 5 MHz-ref set_pll (DEV-13)
    r5 = f["D-ref5MHz-pll_calc-stale-a160"]
    assert r5["xtal_hz"] == 5_000_000 and r5["pll_a"] == 160
    assert r5["lib_pll_regs"]["29"] == 0x4E and r5["lib_pll_regs"]["30"] == 0x00

    # sixth / adjacent: 27 MHz in-spec ref via set_ref_freq + set_pll
    r27 = f["A-ref27MHz-set_pll-fixed"]
    assert r27["xtal_hz"] == 27_000_000
    assert r27["pll_a"] == 29  # floor(800e6/27e6)=29
    assert "lib_pll_regs" in r27

    if errors:
        print("FAIL:")
        for e in errors:
            print(" -", e)
        return 1
    print(f"OK: {len(fresh)} library golden cases bit-identical "
          f"({stored['original_case_count']} original + {stored['new_case_count']} new)")
    print(f"    mutation gaps locked: {', '.join(MUTATION_GAP_CASES)}")
    print(f"    APIs automated: {', '.join(REQUIRED_API_CASES)}")
    print(f"    XTAL={L.SI5351_XTAL_FREQ} PLL_FIXED={L.SI5351_PLL_FIXED} MULT={L.SI5351_FREQ_MULT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
