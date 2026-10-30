#!/usr/bin/env python3
"""
Cross-check library-mirror cases (si5351_math.py) against the independent
AN619/datasheet oracle (spec_oracle.py).

For each case we produce a `spec` block:
  * spec-expected register bytes at AN619 addresses (from the library's chosen
    a/b/c, because the spec does not prescribe how b/c is chosen)
  * realized output frequency decoded from the library's registers (exact)
  * constraint checks with citations
  * status: "agree" | "deviation"  (+ which rules failed)
Known deviations are declared in KNOWN_DEVIATIONS / DEVIATION_CATALOG and are
treated as expected failures (xfail) by test_spec.py.
"""
from __future__ import annotations
from fractions import Fraction as F
from typing import Any, Dict, List, Optional

import spec_oracle as S

# ---------------------------------------------------------------------------
# Deviation catalogue (library behaviour locked by golden_cases; spec flags it)
# ---------------------------------------------------------------------------
DEVIATION_CATALOG: Dict[str, Dict[str, str]] = {
    "DEV-01-FOUT-MAX-225": {
        "title": "Library accepts CLK0-5 output up to 225 MHz; spec max is 200 MHz",
        "library": "SI5351_MULTISYNTH_MAX_FREQ = 225000000; set_freq clamps to 225 MHz, programs DIVBY4 with fVCO=4*Fout (e.g. 210 MHz -> VCO 840 MHz, 250 MHz request -> 225 MHz/VCO 900 MHz)",
        "spec": "DS Table 7 FCLK max 200 MHz; AN619 §2 MS output 500 kHz-200 MHz; AN619 §4.1.3 DIVBY4 range 150 < Fout <= 200 MHz",
        "rules": "OUT_FREQ_RANGE, MS_OUT_RANGE (+OUT_FREQ_ACCURACY when clamped)",
        "severity": "medium",
    },
    "DEV-02-FOUT-MIN-4K": {
        "title": "Library min CLK0-5 output is 4 kHz; datasheet min is 2.5 kHz",
        "library": "SI5351_CLKOUT_MIN_FREQ = 4000; 2.5 kHz request silently becomes 4 kHz (R=128, MS 512 kHz)",
        "spec": "DS Table 7 FCLK min 0.0025 MHz; AN619 §2 'down to 2.5 kHz' (note AN619 §4.2.2 says 8 kHz: docs disagree)",
        "rules": "OUT_FREQ_ACCURACY",
        "severity": "low (conservative)",
    },
    "DEV-03-CLK67-LOWFREQ-P1-TRUNC": {
        "title": "CLK6/7 low-frequency request writes MS6_P1 = 0 (uint8 truncation of 256)",
        "library": "set_freq(<18.454 kHz, CLK6/7) clamps to 4 kHz (CLKOUT_MIN, not CLKOUT67_MIN), select_r_div_ms67 picks R=1, multisynth67_calc clamps to 500 kHz, a=254 -> PLL 127 MHz < VCO_MIN -> a+=2 = 256 -> reg->p1=(uint8_t)256 = 0; set_pll(128 MHz) is clamped to 600 MHz inside pll_calc but pllb_freq stores 128 MHz",
        "spec": "AN619 Reg 90/91: MS6/7 ratio even integer 6..254; AN619 §2 VCO 600-900 MHz",
        "rules": "MS67_RATIO_VALID, VCO_REQUEST_RANGE, OUT_FREQ_ACCURACY",
        "severity": "high",
    },
    "DEV-04-MS-RATIO-6-TO-8": {
        "title": "Library permits output MS ratios in [6,8) other than 6 (odd 7, fractional 6.x, 7.x)",
        "library": "multisynth_calc(freq, pll_freq) only clamps a<6 / a>1800; set_freq_manual(100 MHz, 700 MHz) -> ratio 7; (90 MHz, 600 MHz) -> 6.666666",
        "spec": "AN619 §2.1.1 item 1 / §4.1.2: valid ratios 4, 6, 8, or 8+1/1048575 .. 2048",
        "rules": "MS_RATIO_VALID",
        "severity": "medium (set_freq_manual / mixed-PLL scenarios)",
    },
    "DEV-05-MS-STALE-A": {
        "title": "multisynth_calc keeps stale a after the a<6 clamp -> programs ratio 5",
        "library": "a = pll/freq = 5; freq = pll/6 but a is not recomputed; b = 0 -> P1 = 128 (ratio 5). set_freq_manual(140 MHz, 700 MHz)",
        "spec": "AN619 §2.1.1: ratio 5 invalid",
        "rules": "MS_RATIO_VALID",
        "severity": "medium",
    },
    "DEV-06-DIVBY4-PRECONDITION": {
        "title": "set_freq_manual forces DIVBY4 for Fout >= 150 MHz regardless of pll_freq",
        "library": "set_freq_manual(175 MHz, 800 MHz) writes DIVBY4 (P=0,0,1) -> actual output 200 MHz",
        "spec": "AN619 §4.1.3 'Set the appropriate feedback Multisynth to generate fVCO = Fout*4'",
        "rules": "OUT_FREQ_ACCURACY",
        "severity": "medium (documented 'track settings yourself' API)",
    },
    "DEV-07-VCXO-CENTER-112M5": {
        "title": "VCXO-linked output above 112.5 MHz (library example si5351_vcxo.ino uses 146 MHz)",
        "library": "No check; example tunes CLK0 = 146 MHz on VCXO PLLB 876 MHz",
        "spec": "DS §4.2 'Multisynth ... synthesizes the VCXO output's center frequency up to 112.5 MHz'",
        "rules": "VCXO_CENTER_MAX",
        "severity": "low (example/precondition)",
    },
    "DEV-08-VCXO-C-NOT-PRESERVED": {
        "title": "VCXO requirement c = 10^6 on MSNB is lost after later set_pll() calls",
        "library": "set_vcxo() does not update pllb_freq; set_correction()/set_pll_input() re-run set_pll(pllb_freq=800 MHz) -> MSNB a=32,b=0,c=1. set_freq_manual(..., 875 MHz) after set_vcxo(875 MHz) re-runs non-VCXO pll_calc -> b=0 -> c=1",
        "spec": "AN619 §3.2 & §7: 'Feedback B Multisynth divider ratio must be set such that ... c ... is fixed to 10^6'",
        "rules": "VCXO_C_FIXED, VCXO_PLL_PRESERVED",
        "severity": "medium",
    },
    "DEV-09-PHOFF-CLK67": {
        "title": "set_phase(CLK6/7) writes reserved registers 171/172",
        "library": "si5351_write(SI5351_CLK0_PHASE_OFFSET + clk, phase) with no clk bound",
        "spec": "AN619 §6 & Reg 165-170: phase offset only for CLK0-5; 171-176 Reserved",
        "rules": "PHOFF_ADDR",
        "severity": "low",
    },
    "DEV-10-XTAL-CL-0PF": {
        "title": "SI5351_CRYSTAL_LOAD_0PF writes reserved XTAL_CL = 00b",
        "library": "init(SI5351_CRYSTAL_LOAD_0PF, ...) -> reg183 = 0x12",
        "spec": "AN619 Reg 183: XTAL_CL 00 'Reserved. Do not select this option.'",
        "rules": "XTAL_CL_RESERVED",
        "severity": "low",
    },
    "DEV-11-SET_INT-CLK67-IS-FB_INT": {
        "title": "set_int(CLK6/7) toggles FBA_INT/FBB_INT (reg 22/23 bit 6), not an MS6/7 integer bit",
        "library": "set_int() writes bit 6 of SI5351_CLK0_CTRL+clk for any clk; on fractional PLLA this forces MSNA integer mode",
        "spec": "AN619 Reg 22/23 bit 6 = FBA_INT/FBB_INT; §3.2.1 only for even-integer feedback; §4.1.2.1 MS6/7 have no INT bit",
        "rules": "FB_INT_ONLY_EVEN_INT",
        "severity": "medium",
    },
    "DEV-12-CLKIN-DIV-RMW": {
        "title": "set_pll_input ORs CLKIN_DIV into reg 15 without clearing the old field",
        "library": "reg_val |= clkin_div (the &= ~SI5351_CLKIN_DIV_MASK line is commented out): prior /2 (01b) + new /4 (10b) -> 11b = /8",
        "spec": "AN619 Reg 15 CLKIN_DIV[1:0] is a 2-bit field (00 /1, 01 /2, 10 /4, 11 /8)",
        "rules": "CLKIN_DIV_FIELD",
        "severity": "medium (Si5351C/CLKIN users)",
    },
    "DEV-13-PLL-STALE-A": {
        "title": "pll_calc keeps stale a after a>90 clamp (non-25/27 MHz reference)",
        "library": "5 MHz reference: a = 160 > 90, freq clamped to 450 MHz but a not recomputed; P1 = 19968 (ratio 160)",
        "spec": "AN619 §3.2 feedback ratio 15..90; AN619 §3.1.2 PLL input 10-40 MHz; DS Table 6/8 XTAL 25-27 MHz",
        "rules": "FB_RATIO_RANGE, REF_RANGE",
        "severity": "low (out-of-spec reference)",
    },
    "DEV-14-CLK67-NOT-ENABLED": {
        "title": "set_freq() on CLK6/CLK7 never enables the output (reg 3 OEB bit stays 1)",
        "library": "CLK0-5 branch calls output_enable(clk,1) on first set; CLK6/7 branch has no output_enable(); reset() disabled all outputs",
        "spec": "AN619 Reg 3 CLKx_OEB: 1 = output disabled",
        "rules": "OUTPUT_ENABLED_AFTER_SET",
        "severity": "medium (functional; recommend hardware confirmation)",
    },
}

DEVIATION_CATALOG["DEV-15-MANUAL-CLK67"] = {
    "title": "set_freq_manual() on CLK6/7 writes a truncated fractional-MS P1 to reg 90/91",
    "library": "set_freq_manual(87.4 MHz, 874 MHz, CLK7): multisynth_calc gives a=10,b=0,c=1 -> P1=128*10-512=768; set_ms writes (uint8_t)768 = 0x00 to reg 91",
    "spec": "AN619 §4.1.2 / Reg 91: 'For MS6 and MS7, set MSx_P1 directly (e.g., MSx_P1 = divide value)' -> 10 (0x0A)",
    "rules": "MS67_RATIO_VALID, OUT_FREQ_ACCURACY",
    "severity": "high (silent wrong output)",
}

# case id -> set of error-severity rules expected to FAIL (xfail)
KNOWN_DEVIATIONS: Dict[str, Dict[str, Any]] = {
    "P0-clk6-23MHz": {"devs": ["DEV-14-CLK67-NOT-ENABLED"], "rules": {"OUTPUT_ENABLED_AFTER_SET"}},
    "P0-clk7-874MHz-even-div-of-clk6-pll": {"devs": ["DEV-14-CLK67-NOT-ENABLED"], "rules": {"OUTPUT_ENABLED_AFTER_SET"}},
    "P0-vcxo-manual-146MHz-on-876pll": {"devs": ["DEV-07-VCXO-CENTER-112M5"], "rules": {"VCXO_CENTER_MAX"}},
    "S-clk6-R6div-100kHz": {"devs": ["DEV-14-CLK67-NOT-ENABLED"], "rules": {"OUTPUT_ENABLED_AFTER_SET"}},
    "S-clk7-R7div-50kHz": {"devs": ["DEV-14-CLK67-NOT-ENABLED"], "rules": {"OUTPUT_ENABLED_AFTER_SET"}},
    "S-clk6-150MHz-upper-clamp": {"devs": ["DEV-14-CLK67-NOT-ENABLED"], "rules": {"OUTPUT_ENABLED_AFTER_SET"}},
    "D-clk0-210MHz-above-spec-max": {"devs": ["DEV-01-FOUT-MAX-225"], "rules": {"OUT_FREQ_RANGE", "MS_OUT_RANGE"}},
    "D-clk0-250MHz-clamped-to-225": {"devs": ["DEV-01-FOUT-MAX-225"],
                                     "rules": {"OUT_FREQ_RANGE", "MS_OUT_RANGE", "OUT_FREQ_ACCURACY"}},
    "D-clk0-2p5kHz-clamped-to-4kHz": {"devs": ["DEV-02-FOUT-MIN-4K"], "rules": {"OUT_FREQ_ACCURACY"}},
    "D-clk6-10kHz-ms6-p1-zero": {"devs": ["DEV-03-CLK67-LOWFREQ-P1-TRUNC", "DEV-14-CLK67-NOT-ENABLED"],
                                 "rules": {"MS67_RATIO_VALID", "VCO_REQUEST_RANGE", "OUT_FREQ_ACCURACY",
                                           "OUTPUT_ENABLED_AFTER_SET"}},
    "D-manual-100MHz-pll700-ratio7": {"devs": ["DEV-04-MS-RATIO-6-TO-8"], "rules": {"MS_RATIO_VALID"}},
    "D-manual-90MHz-pll600-ratio6p67": {"devs": ["DEV-04-MS-RATIO-6-TO-8"], "rules": {"MS_RATIO_VALID"}},
    "D-manual-140MHz-pll700-stale-a5": {"devs": ["DEV-05-MS-STALE-A"], "rules": {"MS_RATIO_VALID"}},
    "D-manual-175MHz-pll800-divby4-precondition": {"devs": ["DEV-06-DIVBY4-PRECONDITION"],
                                                   "rules": {"OUT_FREQ_ACCURACY"}},
    "D-vcxo-then-set_correction-clobbers-pllb": {"devs": ["DEV-08-VCXO-C-NOT-PRESERVED"],
                                                 "rules": {"VCXO_C_FIXED", "VCXO_PLL_PRESERVED"}},
    "D-vcxo-875MHz-then-manual-rewrites-c1": {"devs": ["DEV-08-VCXO-C-NOT-PRESERVED"], "rules": {"VCXO_C_FIXED"}},
    "D-manual-clk7-87p4MHz-p1-truncated": {"devs": ["DEV-15-MANUAL-CLK67"],
                                           "rules": {"MS67_RATIO_VALID", "OUT_FREQ_ACCURACY"}},
    "D-set_phase-clk6-writes-reserved-171": {"devs": ["DEV-09-PHOFF-CLK67"], "rules": {"PHOFF_ADDR"}},
    "D-init-crystal-load-0pF-reserved": {"devs": ["DEV-10-XTAL-CL-0PF"], "rules": {"XTAL_CL_RESERVED"}},
    "D-set_int-clk6-sets-FBA_INT-on-fractional-plla": {"devs": ["DEV-11-SET_INT-CLK67-IS-FB_INT"],
                                                       "rules": {"FB_INT_ONLY_EVEN_INT"}},
    "D-set_pll_input-clkin_div-or-without-clear": {"devs": ["DEV-12-CLKIN-DIV-RMW"], "rules": {"CLKIN_DIV_FIELD"}},
    "D-ref5MHz-pll_calc-stale-a160": {"devs": ["DEV-13-PLL-STALE-A"], "rules": {"FB_RATIO_RANGE", "REF_RANGE"}},
}


def _regs(d: Dict[int, int]) -> Dict[str, int]:
    return {str(k): v for k, v in d.items()}


def _fref(case: Dict[str, Any]) -> F:
    x = F(case.get("xtal_hz", 25_000_000))
    return x * (1 + F(case.get("correction_ppb", 0), 10**9))


def _cmp(rule: str, lib: Any, spec: Any, cite: str) -> Dict[str, Any]:
    return S.chk(rule, lib == spec, cite, f"library={lib} spec={spec}")


def _eval_feedback(case, checks, pll_key="PLLA", regs_key="lib_pll_regs") -> Optional[F]:
    if "pll_p1" not in case:
        return None
    p1, p2, p3 = case["pll_p1"], case["pll_p2"], case["pll_p3"]
    checks.append(_cmp("FB_ENCODING", [p1, p2, p3], list(S.encode_abc(case["pll_a"], case["pll_b"], case["pll_c"])),
                       "AN619 §3.2 P1/P2/P3 equations"))
    if regs_key in case:
        checks.append(_cmp("REG_PLL_BLOCK", case[regs_key], _regs(S.feedback_block(pll_key, p1, p2, p3)),
                           "AN619 §8.1 / Reg 26-33 (MSNA), 34-41 (MSNB)"))
    fc, ratio, vco = S.check_feedback(p1, p2, p3, _fref(case))
    checks.extend(fc)
    return vco


def evaluate(case: Dict[str, Any]) -> Dict[str, Any]:
    checks: List[Dict[str, Any]] = []
    out: Dict[str, Any] = {}
    api, path, clk = case.get("api"), case.get("clk_path"), case.get("clk", 0)

    if api == "parity_check":
        if "auto" in case:
            a = evaluate(dict(case["auto"], clk=0))
            m = evaluate(dict(case["manual"], clk=0))
            out["auto_status"], out["manual_status"] = a["status"], m["status"]
            checks.append(S.chk("PARITY_SPEC_REGS_EQUAL", a.get("spec_ms_regs") == m.get("spec_ms_regs"),
                                "AN619 §4.1.2/§4.1.3", "spec MS registers for auto vs manual"))
        else:
            checks.append(S.chk("PARITY_MS_MATCH", case["ms_match"], "library parity (no spec rule)", ""))
        return _finish(case, out, checks)

    if api == "set_ms_rmw":
        # Spec packing with prior bit7 must match library final byte
        R = 1 << case["r_div"]
        divby4 = bool(case["div_by_4"])
        p = (case["ms_p1"], case["ms_p2"], case["ms_p3"])
        # Spec ms_block assumes prior unused bits = 0 in the R_DIV/DIVBY4/P1hi field construction;
        # AN619 marks bit7 unused — preserving a 1 there is library RMW behaviour, not a spec requirement.
        # Compare bits 6:0 against spec; bit7 separately as RMW lock.
        spec_ms = S.ms_block(case["clk"], *p, r=R, divby4=divby4)
        out["spec_ms_regs"] = {str(k): v for k, v in spec_ms.items()}
        lib = case["lib_ms_regs"]
        # bytes other than +2 must match exactly
        for off in (0, 1, 3, 4, 5, 6, 7):
            addr = str(42 + 8 * case["clk"] + off)
            checks.append(_cmp(f"REG_MS_B{off}", lib[addr], out["spec_ms_regs"][addr],
                               "AN619 MS0 parameter block"))
        addr2 = str(42 + 8 * case["clk"] + 2)
        spec_byte2 = out["spec_ms_regs"][addr2]  # bit7=0
        lib_byte2 = lib[addr2]
        checks.append(S.chk("REG_MS_BYTE2_LOW7", (lib_byte2 & 0x7F) == (spec_byte2 & 0x7F),
                            "AN619 Reg 44 bits 6:0 (R_DIV|DIVBY4|P1[17:16])",
                            f"library=0x{lib_byte2:02X} spec_low7=0x{spec_byte2:02X}"))
        checks.append(S.chk("RMW_BIT7_PRESERVED", (lib_byte2 & 0x80) == (case["prior_byte2"] & 0x80),
                            "Library set_ms/ms_div RMW preserves unused bit7 (AN619 Reg 44 bit7 Unused)",
                            f"prior=0x{case['prior_byte2']:02X} final=0x{lib_byte2:02X}"))
        # Mutation detectors must differ from the lock
        md = case["rmw_transcript"]["mutation_detectors"]
        checks.append(S.chk("MUTATION_NOT_NAIVE_MS_DIV", lib_byte2 != md["naive_ms_div_overwrite"],
                            "Coding PM mutation: naive ms_div overwrite",
                            f"final=0x{lib_byte2:02X} naive=0x{md['naive_ms_div_overwrite']:02X}"))
        checks.append(S.chk("MUTATION_NOT_NAIVE_SET_MS", lib_byte2 != md["naive_set_ms_then_correct_ms_div"],
                            "Coding PM mutation: naive set_ms (no RMW) then correct ms_div",
                            f"final=0x{lib_byte2:02X} naive=0x{md['naive_set_ms_then_correct_ms_div']:02X}"))
        # Transcript final agrees
        checks.append(_cmp("RMW_TRANSCRIPT_FINAL", case["rmw_transcript"]["final"], lib_byte2,
                           "set_ms+ms_div RMW transcript"))
        # Also check feedback encoding for completeness
        _eval_feedback(case, checks, "PLLA")
        return _finish(case, out, checks)

    if api == "reset_pll_assignment":
        # Lock default PLL map after reset(); Spec allows either PLL (AN619 §4.1.1)
        table = case["pll_assignment_table"]
        checks.append(S.chk("CLK5_RESET_PLLA", table["5"] == "PLLA",
                            "Library reset() clk5 → PLLA (AN619 §4.1.1 allows either)",
                            f"pll_assignment[5]={table['5']}"))
        checks.append(S.chk("CLK7_RESET_PLLB", table["7"] == "PLLB",
                            "Library reset() clk7 → PLLB (AN619 §4.1.1 allows either)",
                            f"pll_assignment[7]={table['7']}"))
        # Encoding of the MSx_SRC bit must match AN619
        for i in range(8):
            want = S.clk_ctrl(i, src_pllb=int(table[str(i)] == "PLLB"))
            got = case["lib_clk_ctrl"][str(16 + i)]
            checks.append(_cmp(f"REG_CLK_CTRL_{i}", got, want, f"AN619 Reg {16+i} MSx_SRC"))
        # Focused lock for this case's clk
        focus = case["clk"]
        checks.append(_cmp("REG_FOCUS_CTRL", case["lib_write"]["value"],
                           case["lib_clk_ctrl"][str(16 + focus)],
                           f"AN619 Reg {16+focus}"))
        # Document that Spec does not mandate the library's choice
        checks.append(S.chk("PLL_ASSIGN_SPEC_PERMITS_EITHER", True, "AN619 §4.1.1",
                            case.get("spec_note", "")))
        # Soft check: mutation_fail_if values must NOT equal the lock
        mfi = case.get("mutation_fail_if", {})
        if focus == 7:
            checks.append(S.chk("MUTATION_CLK7_NOT_PLLA",
                                case["lib_write"]["value"] != mfi.get("clk7_ctrl_equals")
                                and table["7"] != mfi.get("pll_assignment_7"),
                                "mutation detector (Coding PM)",
                                f"locked reg23=0x{case['lib_write']['value']:02X} assignment={table['7']}"))
        if focus == 5:
            checks.append(S.chk("MUTATION_CLK5_NOT_PLLB",
                                case["lib_write"]["value"] != mfi.get("clk5_ctrl_equals")
                                and table["5"] != mfi.get("pll_assignment_5"),
                                "mutation detector (Coding PM)",
                                f"locked reg21=0x{case['lib_write']['value']:02X} assignment={table['5']}"))
        return _finish(case, out, checks)

    if path == "CLK0-5" and "ms_p1" in case:
        pll = case.get("pll_assignment", "PLLA")
        vco = _eval_feedback(case, checks, "PLLB" if pll == "PLLB" else "PLLA")
        divby4 = bool(case["div_by_4"])
        R = 1 << case["r_div"]
        p = (case["ms_p1"], case["ms_p2"], case["ms_p3"])
        if not divby4:
            checks.append(_cmp("MS_ENCODING", list(p), list(S.encode_abc(case["ms_a"], case["ms_b"], case["ms_c"])),
                               "AN619 §4.1.2 MSx_P1/P2/P3 equations"))
            spec_p = S.encode_abc(case["ms_a"], case["ms_b"], case["ms_c"])
        else:
            spec_p = S.divby4_params()
        spec_ms = _regs(S.ms_block(clk, *spec_p, r=R, divby4=divby4))
        out["spec_ms_regs"] = spec_ms
        if "lib_ms_regs" in case:
            checks.append(_cmp("REG_MS_BLOCK", case["lib_ms_regs"], spec_ms,
                               "AN619 §8.1 / Reg 42-89 (R_DIV 6:4, DIVBY4 3:2, P1[17:16] 1:0 of byte +2)"))
        if "lib_clk_ctrl" in case:
            spec_ctrl = {str(16 + clk): S.clk_ctrl(clk, int_bit=case["int_mode"], src_pllb=int(pll == "PLLB"))}
            out["spec_clk_ctrl"] = spec_ctrl
            checks.append(_cmp("REG_CLK_CTRL", case["lib_clk_ctrl"], spec_ctrl, "AN619 Reg 16-21"))
        if "lib_reg3_after" in case:
            checks.append(S.chk("OUTPUT_ENABLED_AFTER_SET", ((case["lib_reg3_after"] >> clk) & 1) == 0,
                                "AN619 Reg 3 CLKx_OEB (0 = enabled)", f"reg3=0x{case['lib_reg3_after']:02X}"))
        f_ms = (vco / 4 if divby4 else vco / S.decode_p(*p)) if vco is not None else None
        mc, ratio = S.check_ms05(*p, divby4=divby4, ms_int=bool(case["int_mode"]), f_ms_hz=f_ms)
        checks.extend(mc)
        realized = f_ms / R if f_ms is not None else None
        req = F(case["input_freq_lib"], 100)
        checks.extend(S.check_out_freq(req, realized))
        if case.get("vcxo_context"):
            checks.append(S.chk("VCXO_CENTER_MAX", req <= S.VCXO_CENTER_MAX_HZ, "DS §4.2 (VCXO center <= 112.5 MHz)",
                                f"Fout={float(req)} Hz"))
            checks.append(S.chk("VCXO_C_FIXED", case["pll_p3"] == S.VCXO_C, "AN619 §3.2/§7 (MSNB c = 10^6)",
                                f"MSNB P3=c={case['pll_p3']}"))
        out.update(realized_hz=float(realized) if realized is not None else None,
                   ms_ratio=str(ratio), fvco_hz=float(vco) if vco is not None else None)
        return _finish(case, out, checks)

    if path == "CLK6/7":
        req = F(case["input_freq_lib"], 100)
        if case.get("rejected"):
            pllb = F(case.get("pll_freq_lib", 0), 100) if case.get("pll_freq_lib") else None
            out["spec_expectation"] = "reject: requested freq is not an even-integer division of PLLB"
            checks.append(S.chk("MS67_REJECT_CONSISTENT", True, "AN619 §2.2 Note (even integers 6..254 only)",
                                f"library rejected ({case.get('reason')})"))
            return _finish(case, out, checks)
        vco = _eval_feedback(case, checks, "PLLB")
        a = list(case["lib_ms67_regs"].values())[0]   # byte the library actually writes to reg 90/91
        R = 1 << case["r_div"]
        req_vco_i = F(case["pll_freq_lib"], 100)
        intended = req_vco_i / (req * R)
        if intended.denominator == 1 and intended.numerator % 2 == 0 and S.MS67_MIN <= intended.numerator <= S.MS67_MAX:
            out["spec_intended_ms67_regs"] = _regs(S.ms67_reg(clk, intended.numerator))
        checks.extend(S.check_ms67(a))
        req_vco = F(case["pll_freq_lib"], 100)
        checks.append(S.chk("VCO_REQUEST_RANGE", S.VCO_MIN_HZ <= req_vco <= S.VCO_MAX_HZ,
                            "AN619 §2 (600-900 MHz); library stores requested value in pllb_freq",
                            f"requested PLLB={float(req_vco)} Hz"))
        valid = a % 2 == 0 and S.MS67_MIN <= a <= S.MS67_MAX
        if valid:
            spec67 = _regs(S.ms67_reg(clk, a))
            out["spec_ms67_regs"] = spec67
            checks.append(_cmp("REG_MS67", case["lib_ms67_regs"], spec67, "AN619 Reg 90/91"))
        spec92 = S.reg92(r6=R) if clk == 6 else S.reg92(r7=R)
        out["spec_reg92"] = spec92
        checks.append(_cmp("REG_92", case["lib_reg92"], spec92, "AN619 Reg 92 (R7_DIV 6:4, R6_DIV 2:0)"))
        spec_ctrl = {str(16 + clk): S.clk_ctrl(clk, int_bit=0, src_pllb=1)}
        checks.append(_cmp("REG_CLK_CTRL", case["lib_clk_ctrl"], spec_ctrl, "AN619 Reg 22/23"))
        checks.append(S.chk("OUTPUT_ENABLED_AFTER_SET", ((case["lib_reg3_after"] >> clk) & 1) == 0,
                            "AN619 Reg 3 CLKx_OEB (0 = enabled)", f"reg3=0x{case['lib_reg3_after']:02X}"))
        realized = vco / a / R if (valid and vco is not None) else None
        checks.extend(S.check_out_freq(req, realized))
        if realized is None:
            checks.append(S.chk("OUT_FREQ_ACCURACY", False, "AN619 Reg 90/91", f"undefined output: MS{clk}_P1={a}"))
        out.update(realized_hz=float(realized) if realized else None, fvco_hz=float(vco) if vco else None)
        return _finish(case, out, checks)

    if api == "set_vcxo":
        _eval_feedback(case, checks, "PLLB")
        v = S.vcxo_param(case["pll_a"], case["pll_b"], case["ppm"])
        out["spec_vcxo_exact"] = float(v["exact"])
        out["spec_vcxo_floor"] = v["floor"]
        out["spec_vcxo_round"] = v["round"]
        out["spec_vcxo_regs"] = _regs(v["regs_floor"])
        checks.append(S.chk("VCXO_C_FIXED", case["pll_c"] == S.VCXO_C and case["pll_p3"] == S.VCXO_C,
                            "AN619 §3.2/§7 (MSNB c = 10^6)", f"c={case['pll_c']} P3={case['pll_p3']}"))
        checks.append(S.chk("VCXO_APR_RANGE", S.VCXO_APR_MIN <= case["ppm"] <= S.VCXO_APR_MAX,
                            "DS Table 5 (±30..±240 ppm)", f"applied ppm={case['ppm']} requested={case.get('requested_ppm', case['ppm'])}"))
        checks.append(_cmp("VCXO_PARAM_FLOOR", case["vcxo_param"], v["floor"], "AN619 §7 (floor of exact value)"))
        checks.append(_cmp("REG_VCXO", case["lib_vcxo_regs"], out["spec_vcxo_regs"],
                           "AN619 Reg 162-164 (164 bits 7:6 'Only write 00b')"))
        if v["floor"] != v["round"]:
            checks.append(S.chk("VCXO_PARAM_ROUNDING", False, "AN619 §7 (no rounding rule stated)",
                                f"exact={float(v['exact']):.4f}: library floors to {v['floor']}; round-half-up would give {v['round']}",
                                "advisory"))
        return _finish(case, out, checks)

    if api == "vcxo_sequence":
        _eval_feedback(case, checks, "PLLB")
        before = case["pllb_after_set_vcxo"]
        checks.append(S.chk("VCXO_C_FIXED", case["pll_p3"] == S.VCXO_C, "AN619 §3.2/§7 (MSNB c = 10^6)",
                            f"MSNB P3 after sequence={case['pll_p3']}"))
        r0 = S.decode_p(before["p1"], before["p2"], before["p3"])
        r1 = S.decode_p(case["pll_p1"], case["pll_p2"], case["pll_p3"])
        checks.append(S.chk("VCXO_PLL_PRESERVED", r0 == r1, "AN619 §7 (PLLB is the VCXO)",
                            f"MSNB ratio after set_vcxo={float(r0)} after sequence={float(r1)}"))
        return _finish(case, out, checks)

    if api == "reset_plls":
        for pll, key in (("PLLA", "lib_plla_regs"), ("PLLB", "lib_pllb_regs")):
            checks.append(_cmp(f"REG_PLL_BLOCK_{pll}", case[key],
                               _regs(S.feedback_block(pll, case["pll_p1"], case["pll_p2"], case["pll_p3"])),
                               "AN619 Reg 26-33 / 34-41"))
        fc, _, _ = S.check_feedback(case["pll_p1"], case["pll_p2"], case["pll_p3"], _fref(case))
        checks.extend(fc)
        checks.append(_cmp("REG_177", case["lib_reg177_writes"],
                           [S.pll_reset_byte(True, False), S.pll_reset_byte(False, True)],
                           "AN619 Reg 177 (PLLB_RST bit7, PLLA_RST bit5)"))
        spec_ctrl = {str(16 + i): S.clk_ctrl(i, src_pllb=int(i >= 6)) for i in range(8)}
        checks.append(_cmp("REG_CLK_CTRL", case["lib_clk_ctrl"], spec_ctrl,
                           "AN619 Reg 16-23 (PDN=0, INT=0, SRC per PLL, CLK_SRC=11 own MS, 2 mA)"))
        return _finish(case, out, checks)

    if api == "set_pll":
        _eval_feedback(case, checks, case.get("pll_assignment", "PLLA"))
        xt = case.get("xtal_hz", 25_000_000)
        checks.append(S.chk("REF_RANGE", S.XTAL_MIN_HZ <= xt <= S.XTAL_MAX_HZ or S.PLL_IN_MIN_HZ <= xt <= S.PLL_IN_MAX_HZ,
                            "DS Table 6/8 XTAL 25-27 MHz; AN619 §3.1.2 PLL input 10-40 MHz", f"ref={xt} Hz"))
        return _finish(case, out, checks)

    if api == "set_phase":
        w = case["lib_write"]
        ok_addr = 0 <= clk <= 5
        checks.append(S.chk("PHOFF_ADDR", ok_addr and w["reg"] == S.REG_PHOFF_BASE + clk,
                            "AN619 §6, Reg 165-170 (CLK0-5 only; 171-176 Reserved)", f"library writes reg {w['reg']}"))
        checks.append(S.chk("PHOFF_BIT7_ZERO", (w["value"] & 0x80) == 0, "AN619 Reg 165-170 bit 7 'Only write 0'",
                            f"value=0x{w['value']:02X}"))
        if ok_addr:
            checks.append(_cmp("REG_PHOFF", [w["reg"], w["value"]], list(S.phase_reg(clk, case["requested_phase"])),
                               "AN619 Reg 165-170"))
        checks.append(S.chk("PHOFF_PRECONDITION", False, "AN619 §6 / §4.1.2.1",
                            "phase offset needs MSx_INT=0 and fractional MS > 8; library does not enforce", "advisory"))
        return _finish(case, out, checks)

    if api == "init_xtal_load":
        codes = {"0pF": 0, "6pF": 1, "8pF": 2, "10pF": 3}
        for k, v in case["lib_writes"].items():
            checks.append(_cmp(f"REG_183_{k}", v, S.xtal_cl_byte(codes[k]), "AN619 Reg 183 (bits 5:0 = 010010b)"))
            checks.append(S.chk("XTAL_CL_RESERVED", (v >> 6) != 0, "AN619 Reg 183 (XTAL_CL=00 reserved)",
                                f"{k}: reg183=0x{v:02X}"))
        return _finish(case, out, checks)

    if api == "set_int":
        w = case["lib_write"]
        int_bit = (w["value"] >> 6) & 1
        if case.get("ms_int_target"):
            # CLK0-5: bit6 is MSx_INT — library behaviour matches AN619 Reg 16-21
            checks.append(_cmp("REG_MS_INT", w["value"],
                               S.clk_ctrl(clk, int_bit=1, src_pllb=int(case.get("pll_assignment") == "PLLB")),
                               f"AN619 Reg {16+clk} bit6 = MSx_INT"))
            checks.append(S.chk("MS_INT_BIT_SET", int_bit == 1, "AN619 §4.1.2.1 / Reg 16-21 bit6",
                                f"reg{w['reg']}=0x{w['value']:02X}"))
            if case.get("plla") and case["plla"] is not None:
                # optional: also confirm PLLA state when set_correction preceded set_int
                _ = S.decode_p(case["plla"]["p1"], case["plla"]["p2"], case["plla"]["p3"])
        else:
            # CLK6/7: bit6 is FBA_INT / FBB_INT (DEV-11 when set on fractional feedback)
            pa = case["plla"]
            r = S.decode_p(pa["p1"], pa["p2"], pa["p3"])
            even_int = r.denominator == 1 and r.numerator % 2 == 0
            checks.append(S.chk("FB_INT_ONLY_EVEN_INT", (not int_bit) or even_int,
                                "AN619 §3.2.1 / Reg 22 bit6 FBA_INT (only for even-integer a+b/c)",
                                f"reg{w['reg']}=0x{w['value']:02X} -> FBA_INT={int_bit}; MSNA ratio={float(r):.9f}"))
        return _finish(case, out, checks)

    if api == "set_pll_input":
        w = case["lib_write"]
        field = (w["value"] >> 6) & 3
        want = S.R_VALUES.index(case["intended_clkin_div"])  # /1,/2,/4,/8 -> 0..3
        checks.append(S.chk("CLKIN_DIV_FIELD", field == want, "AN619 Reg 15 CLKIN_DIV[1:0]",
                            f"reg15=0x{w['value']:02X} -> CLKIN_DIV={field:02b}b (/{1 << field}); intended /{case['intended_clkin_div']}"))
        return _finish(case, out, checks)

    checks.append(S.chk("UNHANDLED", False, "", f"no evaluator for api={api}"))
    return _finish(case, out, checks)


def _finish(case, out, checks):
    errors = sorted({c["rule"] for c in checks if not c["ok"] and c["severity"] == "error"})
    advisories = sorted({c["rule"] for c in checks if not c["ok"] and c["severity"] == "advisory"})
    reg_rules = [c for c in checks if c["rule"].startswith("REG_") or c["rule"].endswith("_ENCODING")]
    out["register_agreement"] = all(c["ok"] for c in reg_rules) if reg_rules else None
    out["failed_rules"] = errors
    out["advisories"] = advisories
    out["status"] = "deviation" if errors else "agree"
    kd = KNOWN_DEVIATIONS.get(case.get("id", ""))
    out["known_deviations"] = kd["devs"] if kd else []
    out["checks"] = checks
    return out
