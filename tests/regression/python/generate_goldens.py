#!/usr/bin/env python3
"""Regenerate golden_cases.json (library lock + independent spec columns) and golden_cases.md."""
from __future__ import annotations
import copy, json
from pathlib import Path

import si5351_math as L
from spec_crosscheck import evaluate, DEVIATION_CATALOG, KNOWN_DEVIATIONS

ROOT = Path(__file__).resolve().parent

GOLDEN_CORRECTIONS = [{
    "cases": ["P0-clk0-DIVBY4-200MHz", "P0-clk0-DIVBY4-boundary-150MHz", "P0-manual-DIVBY4-175MHz-pll700",
              "P1-parity-gt100-set_freq-vs-manual-175MHz (embedded auto/manual)"],
    "field": "ms_a",
    "old": 6, "new": 4,
    "reason": ("multisynth_calc(freq, pll_freq) sees a = pll/freq = 4 < SI5351_MULTISYNTH_A_MIN, clamps freq = pll/6 "
               "but does NOT recompute a. The first-draft mirror recomputed a (=6). Registers were never affected "
               "because DIVBY4 overrides P1/P2/P3 to 0/0/1; only the reported intermediate changes. Mirror now "
               "matches library exactly (also fixes the same pattern in pll_calc)."),
}]


def library_cases():
    return L.build_golden_suite() + L.build_spec_coverage_cases()


def build():
    cases = library_cases()
    out_cases = []
    for c in cases:
        c = copy.deepcopy(c)
        c["spec"] = evaluate(c)
        out_cases.append(c)
    n_orig = len(L.build_golden_suite())
    data = {
        "source_repo": "https://github.com/etherkit/Si5351Arduino",
        "source_commit": "0559cbf82865e2957d46af72e70cc88207421434",
        "source_ref": "master",
        "library_version": "2.2.0",
        "spec_sources": {
            "AN619": "AN619 Rev 0.8 (2021-09-23) Manually Generating an Si5351 Register Map for 10-MSOP and 20-QFN Devices",
            "DS": "Si5351A/B/C-B datasheet Rev 1.3 (2021-08-27)",
        },
        "assumptions": {
            "xtal_hz_default": L.SI5351_XTAL_FREQ, "freq_mult": L.SI5351_FREQ_MULT,
            "pll_fixed_lib": L.SI5351_PLL_FIXED, "rfrac_denom": L.RFRAC_DENOM,
            "correction_default_ppb": 0, "freq_units": "API takes Hz * 100 (SI5351_FREQ_MULT)",
            "init_xo_freq_0_means": "SI5351_XTAL_FREQ (25 MHz)",
            "unknown_prior_register_bits": "assumed 0 (reg 44-style byte bit7, reg 92, reg 3 before reset)",
            "spec_bc_choice": "spec does not prescribe b/c selection; spec registers are encoded from the library's a/b/c",
            "accuracy_tolerance_ppb": 1000,
        },
        "golden_corrections": GOLDEN_CORRECTIONS,
        "deviation_catalog": DEVIATION_CATALOG,
        "known_deviation_cases": {k: {"devs": v["devs"], "rules": sorted(v["rules"])} for k, v in KNOWN_DEVIATIONS.items()},
        "original_case_count": n_orig,
        "new_case_count": len(out_cases) - n_orig,
        "case_count": len(out_cases),
        "cases": out_cases,
    }
    return data


def fmt_regs(d):
    if not d:
        return "—"
    return " ".join(f"{k}:0x{v:02X}" for k, v in d.items())


def write_md(data):
    L_ = []
    a = L_.append
    a("# Golden cases — Si5351Arduino master vs AN619/datasheet")
    a("")
    a(f"**Library:** `{data['source_repo']}` @ `{data['source_commit']}` (v{data['library_version']})  ")
    a(f"**Spec:** {data['spec_sources']['AN619']}; {data['spec_sources']['DS']}")
    a("")
    a(f"**Cases:** {data['case_count']} total = {data['original_case_count']} original + {data['new_case_count']} new (S-/D-).")
    a("")
    a("Two columns throughout: **Library** = what current master computes/writes (the regression lock — never edited to match the spec). "
      "**Spec** = independent AN619/datasheet oracle (`spec_oracle.py`). A *deviation* means the library is locked to current "
      "behaviour but the spec flags it; `test_spec.py` treats those as expected failures (xfail).")
    a("")
    a("## Assumptions")
    for k, v in data["assumptions"].items():
        a(f"- **{k}:** `{v}`")
    a("")
    a("## Golden corrections vs first draft")
    for g in data["golden_corrections"]:
        a(f"- `{g['field']}` {g['old']} → {g['new']} in {', '.join(g['cases'])}. {g['reason']}")
    a("")
    a("## Summary")
    a("")
    a("| Case | Status | Register agreement | Failed spec rules | Known deviation | Advisories |")
    a("|---|---|---|---|---|---|")
    for c in data["cases"]:
        s = c["spec"]
        a(f"| `{c['id']}` | {s['status']} | {s['register_agreement']} | {', '.join(s['failed_rules']) or '—'} | "
          f"{', '.join(s['known_deviations']) or '—'} | {', '.join(s['advisories']) or '—'} |")
    a("")
    a("## Deviation catalogue")
    a("")
    for k, d in data["deviation_catalog"].items():
        a(f"### {k}: {d['title']}")
        a(f"- **Library (locked):** {d['library']}")
        a(f"- **Spec:** {d['spec']}")
        a(f"- **Rules:** {d['rules']} — **Severity:** {d['severity']}")
        a("")
    a("## Per-case detail")
    a("")
    for c in data["cases"]:
        s = c["spec"]
        a(f"### `{c['id']}`  — {s['status'].upper()}")
        a("")
        if c.get("covers"):
            a(f"Covers: {', '.join(c['covers'])}")
        if c.get("sequence"):
            a(f"Sequence: `{' ; '.join(c['sequence'])}`")
        if c.get("description"):
            a(c["description"])
        a("")
        a("| Item | Library (regression lock) | Spec (AN619/DS) |")
        a("|---|---|---|")
        if "input_freq_hz" in c:
            a(f"| Requested | {c['input_freq_hz']} Hz ({c.get('api')}, CLK{c.get('clk',0)}, {c.get('pll_assignment','')}) | "
              f"realized from regs: {s.get('realized_hz')} Hz |")
        if "pll_a" in c:
            a(f"| PLL a,b,c / P1,P2,P3 | {c['pll_a']},{c['pll_b']},{c['pll_c']} / {c['pll_p1']},{c['pll_p2']},{c['pll_p3']} | "
              f"fVCO={s.get('fvco_hz')} |")
        for libk, speck, label in (("lib_pll_regs", None, "PLL regs"), ("lib_plla_regs", None, "MSNA regs"),
                                   ("lib_pllb_regs", None, "MSNB regs")):
            if libk in c:
                a(f"| {label} | {fmt_regs(c[libk])} | {'agree' if s['register_agreement'] else 'see checks'} |")
        if "ms_p1" in c and c.get("clk_path") == "CLK0-5":
            a(f"| MS a,b,c / P1,P2,P3 | {c['ms_a']},{c['ms_b']},{c['ms_c']} / {c['ms_p1']},{c['ms_p2']},{c['ms_p3']} | "
              f"ratio={s.get('ms_ratio')} |")
            a(f"| R_DIV / DIVBY4 / MS_INT | {c['r_div_name']} / {c['div_by_4']} / {c['int_mode']} | |")
        if "lib_ms_regs" in c:
            a(f"| MS block | {fmt_regs(c['lib_ms_regs'])} | {fmt_regs(s.get('spec_ms_regs'))} |")
        if "lib_ms67_regs" in c:
            a(f"| MS6/7 P1 reg | {fmt_regs(c['lib_ms67_regs'])} (R {c.get('r_div_name')}) | "
              f"{fmt_regs(s.get('spec_ms67_regs')) if s.get('spec_ms67_regs') else 'invalid'}"
              f"{' ; intended ' + fmt_regs(s['spec_intended_ms67_regs']) if s.get('spec_intended_ms67_regs') else ''} |")
        if "lib_reg92" in c:
            a(f"| Reg 92 | 0x{c['lib_reg92']:02X} | 0x{s['spec_reg92']:02X} |")
        if "lib_clk_ctrl" in c:
            a(f"| CLK ctrl | {fmt_regs(c['lib_clk_ctrl'])} | {fmt_regs(s.get('spec_clk_ctrl')) if s.get('spec_clk_ctrl') else 'see checks'} |")
        if "lib_reg3_after" in c:
            a(f"| Reg 3 (OEB) after | 0x{c['lib_reg3_after']:02X} | bit{c.get('clk',0)} must be 0 to output |")
        if "lib_vcxo_regs" in c:
            a(f"| VCXO param / regs 162-164 | {c['vcxo_param']} / {fmt_regs(c['lib_vcxo_regs'])} | exact {s['spec_vcxo_exact']:.4f}, floor {s['spec_vcxo_floor']}, round {s['spec_vcxo_round']} / {fmt_regs(s['spec_vcxo_regs'])} |")
        if "lib_write" in c:
            a(f"| Register write | reg {c['lib_write']['reg']} = 0x{c['lib_write']['value']:02X} | see checks |")
        if "lib_writes" in c:
            a(f"| Register writes | {', '.join(f'{k}: 0x{v:02X}' for k, v in c['lib_writes'].items())} | see checks |")
        if "lib_reg177_writes" in c:
            a(f"| Reg 177 writes | {', '.join(f'0x{v:02X}' for v in c['lib_reg177_writes'])} | 0x20 (PLLA_RST), 0x80 (PLLB_RST) |")
        a("")
        bad = [k for k in s["checks"] if not k["ok"]]
        if bad:
            a("Failed / advisory checks:")
            for k in bad:
                a(f"- **{k['rule']}** ({k['severity']}) — {k['detail']} — _{k['cite']}_")
            a("")
    (ROOT / "golden_cases.md").write_text("\n".join(L_) + "\n")


def main():
    data = build()
    (ROOT / "golden_cases.json").write_text(json.dumps(data, indent=2, default=str))
    write_md(data)
    print(f"Wrote {data['case_count']} cases ({data['original_case_count']} original + {data['new_case_count']} new)")


if __name__ == "__main__":
    main()
