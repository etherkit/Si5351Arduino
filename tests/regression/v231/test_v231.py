#!/usr/bin/env python3
"""Si5351Arduino v2.3.1 acceptance tests + must-not-break locks.

Input: JSONL from the C++ runner (acceptance.cpp + scenarios_v231.inc), built against
the real library source, e.g. master 1ddd43e  ->  logs/acc_master.jsonl.

  python3 test_v231.py logs/acc_master.jsonl                 # report; exit 0 iff all MUST-NOT-BREAK pass
  python3 test_v231.py logs/acc_master.jsonl --expect-master # also require every ACCEPTANCE test to FAIL (1ddd43e state)
  python3 test_v231.py logs/acc_fixed.jsonl --require-fixed  # require every ACCEPTANCE test to PASS (the v2.3.1 gate)
  python3 test_v231.py logs/acc_master.jsonl --write-golden  # (re)write v231_acceptance.json + must_not_break.json

Expected values are what AN619 / the datasheet require (citations to ref/AN619.txt and
ref/datasheet.txt line numbers). Where the correct policy is debatable, the test asserts only
the policy-neutral spec invariants and accepts any of the listed options.
"""
import json, re, sys
from fractions import Fraction
from pathlib import Path

HERE = Path(__file__).resolve().parent
GOLDEN67 = HERE.parent / "golden_cases.json"
XTAL = 25_000_000
CLKOUT67_MIN = (600_000_000 // 254) // 128          # 18454 Hz, library's own SI5351_CLKOUT67_MIN_FREQ

# ---------------------------------------------------------------- decoding helpers
def calls(run):
    """Split the transcript into [(call_text, [events])]."""
    out = [("<pre>", [])]
    for e in run["tx"]:
        if e.startswith("CALL "): out.append((e[5:], []))
        else: out[-1][1].append(e)
    return out

def writes(events):
    """[(reg, value)] for every byte written, bulk writes expanded."""
    w = []
    for e in events:
        m = re.match(r"W a=60 r=(\d+) n=\d+ \[([0-9A-F ]+)\]", e)
        if m:
            base = int(m.group(1))
            for i, h in enumerate(m.group(2).split()): w.append(((base + i) & 0xFF, int(h, 16)))
    return w

def all_writes(run): return writes(run["tx"])
def last_call_writes(run, prefix):
    cs = [c for c in calls(run) if c[0].startswith(prefix)]
    return writes(cs[-1][1]) if cs else []
def last_call_ret(run, name):
    r = [x for x in run["rets"] if x.startswith(name + "=")]
    return int(r[-1].split("=")[1]) if r else None

def block_writes(run, base):
    """8-byte parameter blocks written as one bulk write at `base`."""
    out = []
    for e in run["tx"]:
        m = re.match(r"W a=60 r=(\d+) n=8 \[([0-9A-F ]+)\]", e)
        if m and int(m.group(1)) == base: out.append([int(h, 16) for h in m.group(2).split()])
    return out

def p123(b):
    p1 = ((b[2] & 3) << 16) | (b[3] << 8) | b[4]
    p2 = ((b[5] & 0x0F) << 16) | (b[6] << 8) | b[7]
    p3 = ((b[5] >> 4) << 16) | (b[0] << 8) | b[1]
    return p1, p2, p3

def ratio(b):
    """a + b/c from an AN619 P1/P2/P3 block (AN619 sec 3.2 / 4.1.2 encodings)."""
    p1, p2, p3 = p123(b)
    if p3 == 0: return None
    return (Fraction(p1 + 512) + Fraction(p2, p3)) / 128

def vco(regs, base):
    r = ratio(regs[base:base + 8])
    return None if r is None else XTAL * r

def ms67_out(regs, clk):
    """CLKx out for CLK6/7 = fVCO_B / (MSx_P1 * R)  (AN619 Reg 90/91/92)."""
    p1 = regs[90 + clk - 6]
    rsh = (regs[92] & 7) if clk == 6 else ((regs[92] >> 4) & 7)
    v = vco(regs, 34)
    return None if (p1 == 0 or v is None) else v / (p1 * (1 << rsh))

def ms05_ratio(regs, clk):
    b = regs[42 + 8 * clk: 50 + 8 * clk]
    if (b[2] >> 2) & 3 == 3: return Fraction(4)        # MSx_DIVBY4 = 11b
    return ratio(b)

def ms05_out(regs, clk):
    src = 34 if regs[16 + clk] & 0x20 else 26
    v, r = vco(regs, src), ms05_ratio(regs, clk)
    rsh = (regs[44 + 8 * clk] >> 4) & 7
    return None if (v is None or not r) else v / (r * (1 << rsh))

def valid_ms05_ratio(r):  # AN619 sec 2.1.1 item 1 (AN619.txt:58), sec 4.1.2 (AN619.txt:235)
    return r in (4, 6, 8) or (Fraction(8) < r <= 2048 and r >= 8 + Fraction(1, 1048575))

def valid_ms67(p1):       # AN619.txt:74, :247, Reg 90/91 (AN619.txt:2696-2715)
    return 6 <= p1 <= 254 and p1 % 2 == 0

def mhz(x): return None if x is None else round(float(x) / 1e6, 6)

# ---------------------------------------------------------------- acceptance tests
# each check(run_by_pattern) -> list of (ok, what, observed, expected)
def t_dev03(rp):
    res = []
    for pat, run in rp.items():
        if run["crash"]: res.append((False, f"[p{pat}] no crash", run["crash"], "no crash")); continue
        bad = [v for (r, v) in all_writes(run) if r == 90 and not valid_ms67(v)
               and any(c[0].startswith("set_freq(") for c in calls(run))]
        sf_w = [v for (r, v) in last_call_writes(run, "set_freq(") if r == 90]
        res.append((all(valid_ms67(v) for v in sf_w), f"[p{pat}] every reg90 write by set_freq is an even integer 6..254",
                     [f"0x{v:02X}" for v in sf_w], "even 6..254 (AN619 Reg 90, AN619.txt:2696)"))
        pb = run["state"]["pllb_freq"] / 100
        res.append((600e6 <= pb <= 900e6, f"[p{pat}] stored pllb_freq within VCO 600..900 MHz", f"{pb/1e6:g} MHz", "600..900 MHz (AN619.txt:37)"))
        ret = last_call_ret(run, "set_freq"); f6 = run["state"]["clk_freq"][6] / 100
        out = ms67_out(run["regs"], 6)
        rejected = ret == 1 and f6 == 0 and not sf_w
        clamped = ret == 0 and out is not None and float(out) >= CLKOUT67_MIN - 0.5 and f6 >= CLKOUT67_MIN
        res.append((rejected or clamped, f"[p{pat}] either rejected (ret 1, nothing written) or clamped to >= CLKOUT67_MIN {CLKOUT67_MIN} Hz",
                     f"ret={ret} clk_freq[6]={f6:g} Hz realized={None if out is None else round(float(out),3)} Hz", "reject or clamp>=18454 Hz (DEBATABLE D1)"))
    return res

def t_dev15(clk):
    def f(rp):
        res = []
        for pat, run in rp.items():
            v = run["regs"][90 + clk - 6]
            res.append((v == 0x0A, f"[p{pat}] reg{90+clk-6} (MS{clk}_P1)", f"0x{v:02X}", "0x0A (AN619.txt:247 'set MSx_P1 directly (= divide value)'; 874/87.4 = 10)"))
            out = ms67_out(run["regs"], clk)
            res.append((out is not None and abs(float(out) - 87.4e6) < 1, f"[p{pat}] realized CLK{clk}", f"{mhz(out)} MHz", "87.4 MHz"))
        return res
    return f

def t_dev14(clks):
    def f(rp):
        res = []
        for pat, run in rp.items():
            r3 = run["regs"][3]
            for c in clks:
                res.append(((r3 >> c) & 1 == 0, f"[p{pat}] reg3 CLK{c}_OEB after set_freq", (r3 >> c) & 1,
                            "0 = enabled (AN619 Reg 3; parity with CLK0-5 first-set enable, si5351.cpp set_freq CLK0-5 branch)"))
        return res
    return f

def t_uninit(rp):
    res = []
    for pat, run in rp.items():
        ret = last_call_ret(run, "set_freq")
        w91 = [v for (r, v) in last_call_writes(run, "set_freq(100000000,CLK7)") if r == 91]
        res.append((ret == 1, f"[p{pat}] set_freq(1 MHz, CLK7) return", ret, "1: 874 MHz/1 MHz = 874 = 2x437, no even MS7 6..254 x R=2^k exists (AN619.txt:74, Reg 92 R7)"))
        res.append((not w91, f"[p{pat}] no write to reg 91 (uninitialised ms_reg.p1)", [f"0x{v:02X}" for v in w91], "no write"))
        res.append((run["state"]["clk_freq"][7] == 0, f"[p{pat}] clk_freq[7] unchanged", run["state"]["clk_freq"][7] / 100, 0))
    return res

def t_div0(clk):
    def f(rp):
        res = []
        for pat, run in rp.items():
            res.append((run["crash"] is None, f"[p{pat}] set_freq(0, CLK{clk}) with peer set does not crash", run["crash"] or "no crash", "no crash (no '% 0')"))
            if run["crash"] is None:
                w = [v for (r, v) in last_call_writes(run, "set_freq(0,") if r == 90 + clk - 6]
                res.append((all(valid_ms67(v) for v in w), f"[p{pat}] any reg{90+clk-6} write is valid", w, "even 6..254 or none (DEBATABLE D2 for semantics)"))
        return res
    return f

def t_fmax(rp):
    res = []
    for pat, run in rp.items():
        out = ms05_out(run["regs"], 0); f0 = run["state"]["clk_freq"][0] / 100
        ret = last_call_ret(run, "set_freq_manual" if any(x.startswith("set_freq_manual") for x in run["rets"]) else "set_freq")
        rejected = ret == 1 and not block_writes(run, 42)
        res.append((rejected or (out is not None and out <= 200e6), f"[p{pat}] realized CLK0 <= 200 MHz (or rejected: ret 1, MS0 not written)",
                    f"ret={ret} realized={mhz(out)} MHz", "<= 200 MHz (DS p1 / AN619.txt:38; DEBATABLE D3 reject vs clamp)"))
        res.append((f0 <= 200e6, f"[p{pat}] stored clk_freq[0] <= 200 MHz", f"{f0/1e6:g} MHz", "<= 200 MHz"))
    return res

def t_msratio(rp):
    res = []
    for pat, run in rp.items():
        ret = last_call_ret(run, "set_freq_manual")
        ms_w = [w for w in block_writes(run, 42)]
        r = ms05_ratio(run["regs"], 0)
        ok = (ret != 0 and len(ms_w) == 0) or (r is not None and valid_ms05_ratio(r))
        res.append((ok, f"[p{pat}] MS0 ratio valid (or call rejected without writing MS0)",
                    f"ret={ret} ratio={None if r is None else format(float(r), '.6g')}", "4, 6, 8 or 8+1/1048575..2048 (AN619.txt:58,235; DEBATABLE D4)"))
    return res

def t_phase(reg):
    def f(rp):
        return [((reg not in [r for r, _ in all_writes(run)]), f"[p{pat}] no write to reserved reg {reg}",
                 [v for r, v in all_writes(run) if r == reg], "none (AN619.txt:582 'Reg 171-176 Reserved')") for pat, run in rp.items()]
    return f

def t_setint(reg):
    def f(rp):
        res = []
        for pat, run in rp.items():
            before = [v for r, v in writes(next(c for c in calls(run) if c[0].startswith("set_correction"))[1]) if r == reg]
            pre = next((v for (r, v) in reversed(writes([e for c in calls(run) if not c[0].startswith("set_int") for e in c[1]])) if r == reg), None)
            v = run["regs"][reg]
            res.append(((v & 0x40) == ((pre or 0) & 0x40), f"[p{pat}] reg{reg} bit6 (FB{'A' if reg==22 else 'B'}_INT) unchanged by set_int(CLK{reg-16})",
                        f"0x{v:02X} (before 0x{(pre or 0):02X})", f"bit6 unchanged: FB{'A' if reg==22 else 'B'}_INT only for even-integer feedback (AN619.txt:138, :1234); MS6/7 have no INT bit"))
        return res
    return f

def t_clkindiv(rp):
    res = []
    for pat, run in rp.items():
        v = run["regs"][15]
        res.append(((v >> 6) == 0b10, f"[p{pat}] reg15 CLKIN_DIV[1:0] after 80 MHz CLKIN", f"{v>>6:02b}b (reg15=0x{v:02X})", "10b = /4 (AN619.txt:834-840; 80 MHz needs /4 for 10-40 MHz, AN619.txt:94)"))
    return res

def t_xtal0(rp):
    res = []
    for pat, run in rp.items():
        v = run["regs"][183]
        res.append(((v >> 6) != 0 and (v & 0x3F) == 0b010010, f"[p{pat}] reg183", f"0x{v:02X} (XTAL_CL={v>>6:02b}b)",
                    "XTAL_CL != 00b, bits5:0 = 010010b (AN619.txt:3134-3141; DEBATABLE D5 which value)"))
    return res

def t_vcxo(rp, ref_regs):
    res = []
    for pat, run in rp.items():
        b = run["regs"][34:42]; p1, p2, p3 = p123(b); v = vco(run["regs"], 34)
        res.append((p3 == 1_000_000, f"[p{pat}] MSNB c (P3)", p3, "1000000 (AN619.txt:418 'c ... fixed to 10^6')"))
        res.append((v is not None and abs(float(v) - 876e6) < 1, f"[p{pat}] PLLB VCO", f"{mhz(v)} MHz", "876 MHz (set_vcxo value preserved)"))
        res.append((b == ref_regs, f"[p{pat}] PLLB regs 34..41 == set_vcxo() result", b, ref_regs))
    return res

def t_ref5(rp):
    res = []
    for pat, run in rp.items():
        bad = []
        for base in (26, 34):
            for blk in block_writes(run, base):
                r = ratio(blk)
                if r is None or not (15 <= r <= 90): bad.append((base, None if r is None else float(r)))
        res.append((not bad, f"[p{pat}] every PLLA/PLLB feedback block written has a+b/c in 15..90", bad or "all valid",
                    "15..90 (AN619.txt:116; DEBATABLE D6 for the 5 MHz policy)"))
    return res

# id -> (item, scenario, title, checker, debatable-id or None)
ACCEPT = [
    ("A01", "1 DEV-03", "V231:DEV03-clk6-10kHz", "set_freq(10 kHz, CLK6): reg90 must not be 0x00, PLLB must stay in 600-900 MHz", t_dev03, "D1"),
    ("A02", "4 CLK67 clamp", "V231:CLK67MIN-clk6-15kHz-clamp", "CLK6/7 lower clamp must use CLKOUT67_MIN (15 kHz request)", t_dev03, "D1"),
    ("A03", "1 DEV-15", "V231:DEV15-manual-clk7-87p4-874", "set_freq_manual(87.4 MHz, 874 MHz, CLK7) -> reg91 = 0x0A", t_dev15(7), None),
    ("A04", "1 DEV-15", "V231:DEV15-manual-clk6-87p4-874", "set_freq_manual(87.4 MHz, 874 MHz, CLK6) -> reg90 = 0x0A", t_dev15(6), None),
    ("A05", "2 DEV-14", "V231:DEV14-clk6-23MHz-enable", "set_freq(CLK6) enables CLK6 (reg3 bit6 = 0)", t_dev14([6]), None),
    ("A06", "2 DEV-14", "V231:DEV14-clk7-87p4-after-clk6", "set_freq(CLK6) then set_freq(CLK7) enables both", t_dev14([6, 7]), None),
    ("A07", "2 DEV-14", "V231:DEV14-clk7-first-23MHz", "set_freq(CLK7) as first 6/7 output enables CLK7", t_dev14([7]), None),
    ("A08", "4 uninit", "V231:UNINIT-clk7-1MHz-peer-fail", "CLK7 peer accepted but multisynth67_calc fails: must return 1, no reg91 write", t_uninit, None),
    ("A09", "4 div0", "V231:DIV0-clk7-zero-with-peer", "set_freq(0, CLK7) with CLK6 set must not divide by zero", t_div0(7), "D2"),
    ("A10", "4 div0", "V231:DIV0-clk6-zero-with-peer", "set_freq(0, CLK6) with CLK7 set must not divide by zero", t_div0(6), "D2"),
    ("A11", "4 fmax", "V231:FMAX-clk0-210MHz", "set_freq(210 MHz, CLK0) must not output > 200 MHz", t_fmax, "D3"),
    ("A12", "4 fmax", "V231:FMAX-clk0-225MHz", "set_freq(225 MHz, CLK0) must not output > 200 MHz", t_fmax, "D3"),
    ("A13", "4 fmax", "V231:FMAX-manual-clk0-225MHz-900", "set_freq_manual(225 MHz, 900 MHz, CLK0) must not output > 200 MHz", t_fmax, "D3"),
    ("A14", "4 ms ratio", "V231:MSRATIO-manual-140-700-r5", "set_freq_manual(140 MHz, 700 MHz): ratio 5 illegal", t_msratio, "D4"),
    ("A15", "4 ms ratio", "V231:MSRATIO-manual-90-600-r6p67", "set_freq_manual(90 MHz, 600 MHz): ratio 6.667 illegal", t_msratio, "D4"),
    ("A16", "4 ms ratio", "V231:MSRATIO-manual-100-700-r7", "set_freq_manual(100 MHz, 700 MHz): ratio 7 illegal", t_msratio, "D4"),
    ("A17", "4 phase", "V231:PHASE-clk6-reg171", "set_phase(CLK6) must not write reserved reg 171", t_phase(171), None),
    ("A18", "4 phase", "V231:PHASE-clk7-reg172", "set_phase(CLK7) must not write reserved reg 172", t_phase(172), None),
    ("A19", "4 set_int", "V231:SETINT-clk6-fba-int", "set_int(CLK6) must not set FBA_INT (PLLA fractional after correction)", t_setint(22), None),
    ("A20", "4 set_int", "V231:SETINT-clk7-fbb-int", "set_int(CLK7) must not set FBB_INT", t_setint(23), None),
    ("A21", "4 clkin div", "V231:CLKINDIV-50-then-80MHz", "set_pll_input after CLKIN 50->80 MHz: CLKIN_DIV = /4, not OR'd /8", t_clkindiv, None),
    ("A22", "4 0pF", "V231:XTAL-0PF-reserved", "init(CRYSTAL_LOAD_0PF) must not write XTAL_CL = 00b", t_xtal0, "D5"),
    ("A23", "4 vcxo", "V231:VCXO-c-after-set_correction", "set_vcxo(876 MHz) then set_correction(0): MSNB c stays 1e6, VCO 876 MHz", None, None),
    ("A24", "4 vcxo", "V231:VCXO-c-after-set_pll_input", "set_vcxo(876 MHz) then set_pll_input(PLLA, XO): MSNB c stays 1e6", None, None),
    ("A25", "4 DEV-13", "V231:DEV13-ref-5MHz", "init(.., 5 MHz ref): no feedback ratio outside 15..90 (master writes 160)", t_ref5, "D6"),
]

DEBATABLE = {
    "D1": {"topic": "set_freq(f < 18.454 kHz) on CLK6/7 (e.g. 10 kHz, 15 kHz)",
           "facts": "Lowest CLK6/7 output = 600 MHz / 254 / 128 = 18454 Hz (MS6/7 even 6..254, R<=128, VCO>=600 MHz). 10 kHz is unreachable.",
           "options": ["(a) reject: return 1, write nothing, keep clk_freq[clk] (matches CLK7 non-integer reject)",
                       "(b) clamp to SI5351_CLKOUT67_MIN_FREQ (18454 Hz -> MS 254, R 128, VCO 600 MHz) and return 0 (matches CLK0-5 clamp style)",
                       "(c) clamp and return a distinct non-zero code"],
           "test_accepts": "(a) or (b)/(c) as long as reg90 is even 6..254, pllb_freq in 600..900 MHz, output >= 18454 Hz"},
    "D2": {"topic": "set_freq(0, CLK6/7)",
           "facts": "freq 0 reaches pllb_freq % freq (SIGFPE). Without a peer it reaches multisynth67_calc and writes MS6_P1 = 0. CLK0-5 set_freq(0) currently clamps to 500 kHz MS and enables the output (MNB-INFO only, not locked).",
           "options": ["(a) return 1, no writes", "(b) treat 0 as 'disable output' (output_enable(clk,0), clk_freq=0), return 0", "(c) same as CLK0-5 current behavior"],
           "test_accepts": "any, as long as there is no crash and no invalid reg90/91 value"},
    "D3": {"topic": "CLK0-5 request > 200 MHz (210, 225 MHz)",
           "facts": "SI5351_MULTISYNTH_MAX_FREQ = 225 MHz; datasheet/AN619 max output is 200 MHz.",
           "options": ["(a) clamp to 200 MHz (DIVBY4, PLL 800 MHz, a=32) and return 0", "(b) reject with return 1 and no writes"],
           "test_accepts": "either; asserts realized <= 200 MHz and stored clk_freq <= 200 MHz"},
    "D4": {"topic": "set_freq_manual with an illegal MS ratio (5, 6.667, 7)",
           "facts": "AN619 valid ratios: 4, 6, 8, (8+1/1048575 .. 2048]. set_freq_manual's contract is 'track settings yourself'.",
           "options": ["(a) return 1 without writing MS (keep PLL write?)", "(b) round to nearest legal ratio (6 or 8) and return the realized-freq error some other way",
                       "(c) keep writing but return non-zero"],
           "test_accepts": "(a) with no MS0 block written, or any written MS0 ratio that is legal. NOTE: master also writes the PLL before validating; whether a rejected call may still change the PLL is part of this decision"},
    "D5": {"topic": "init(SI5351_CRYSTAL_LOAD_0PF)",
           "facts": "XTAL_CL=00b is 'Reserved. Do not select this option.' (AN619 Reg 183). Reset default is 11b (10 pF).",
           "options": ["(a) map 0PF to 6 pF (01b, smallest legal, closest to the request)", "(b) leave XTAL_CL at default 10 pF (11b)", "(c) remove/deprecate the SI5351_CRYSTAL_LOAD_0PF constant (API change)"],
           "test_accepts": "any XTAL_CL != 00b with bits 5:0 = 010010b"},
    "D6": {"topic": "Reference below 10 MHz (5 MHz XO/CLKIN)",
           "facts": "PLL input range 10-40 MHz (AN619.txt:94); XTAL 25/27 MHz (DS). With 5 MHz, VCO 600 MHz needs ratio 120 > 90: no legal configuration exists.",
           "options": ["(a) init() returns false / set_ref_freq rejects < 10 MHz", "(b) clamp feedback a to 90 (recompute a after the clamp) -> VCO 450 MHz, still below VCO_MIN",
                       "(c) accept but never write an out-of-range feedback block"],
           "test_accepts": "any policy where no PLLA/PLLB block with ratio outside 15..90 is written. (b) passes this test but leaves the VCO at 450 MHz (out of spec): flag if chosen"},
}

# ---------------------------------------------------------------- must-not-break
MNB_MAP = {   # scenario -> (golden id in golden_cases.json or None, notes)
    "MNB:clk0-10MHz": "P0-clk0-common-2-10MHz", "MNB:clk0-14p07MHz": "P0-clk0-fractional-MS-14p07MHz",
    "MNB:clk0-100MHz": "F-clk0-100MHz-share-max-boundary", "MNB:clk0-112p5MHz": "S-boundary-112p5MHz-ratio8",
    "MNB:clk0-150MHz-divby4": "P0-clk0-DIVBY4-boundary-150MHz", "MNB:clk0-200MHz-divby4": "P0-clk0-DIVBY4-200MHz",
    "MNB:clk3-200MHz-divby4": "S-divby4-block-clk3-200MHz", "MNB:clk0-4kHz": "F-clk0-4kHz-library-min",
    "MNB:clk2-5kHz": "S-rdiv-placement-clk2-5kHz", "MNB:clk0-512kHz": "P0-rdiv-1-512kHz-boundary",
    "MNB:clk6-23MHz": "P0-clk6-23MHz", "MNB:clk7-87p4-after-clk6": "P0-clk7-874MHz-even-div-of-clk6-pll",
    "MNB:clk7-reject-12p345678": "P0-clk7-reject-noninteger-12p345678MHz", "MNB:clk7-reject-odd-ratio": None,
    "MNB:clk6-100kHz-R6": "S-clk6-R6div-100kHz", "MNB:clk7-50kHz-R7": "S-clk7-R7div-50kHz",
    "MNB:clk6-150MHz-clamp": "S-clk6-150MHz-upper-clamp", "MNB:manual-clk1-175-700-divby4": "P0-manual-DIVBY4-175MHz-pll700",
    "MNB:manual-clk0-14-800-parity": "P1-parity-set_freq-vs-manual-14MHz-pll800", "MNB:clk1-pllb-14MHz": "P0-clk1-PLLB-14MHz",
    "MNB:vcxo-876-40ppm": "P0-vcxo-876MHz-40ppm", "MNB:pll-assign-after-reset": "M-clk7-pllb-assignment",
    "MNB:clk7-pll-assignment-lock": None, "MNB:set_ms-byte2-rmw-5kHz": "M-set-ms-byte2-rmw",
    "MNB:divby4-exit-reset-155-96": None, "MNB:divby4-exit-reset-156-80": None, "MNB:low-clock-moves-155-clk0-99-clk2": None,
    "MNB:phase-clk0-mask": "S-phase-clk0-mask", "MNB:phase-clk5": None, "MNB:setint-clk0": "A-set_int-clk0-ms-int",
    "MNB:xtal-6pF": "S-xtal-load-6-8-10pF", "MNB:xtal-8pF": "A-init-crystal-load-8pF", "MNB:xtal-10pF": "S-xtal-load-6-8-10pF",
    "MNB:clkin-div4-clean": "A-set_pll_input-clkin-div4-clean", "MNB:ref-27MHz": "A-ref27MHz-set_pll-fixed",
    "MNB:corr-100000-14MHz": "P0-corr-100000-14MHz-existing-pll",
}
INFO_ONLY = {"MNB:both-plls-held-return-code": "item 3 deferred: return codes in both-PLLs-held / other-PLL-too-low cases are NOT locked here",
             "MNB:clk0-0Hz": "set_freq(0) on CLK0-5 recorded only (see D2)"}
# registers a v2.3.1 fix is ALLOWED to change in a MNB scenario
ALLOWED_CHANGE = {"MNB:clk6-23MHz": {"reg3.6"}, "MNB:clk7-87p4-after-clk6": {"reg3.6", "reg3.7"}, "MNB:clk7-reject-12p345678": {"reg3.6"},
                  "MNB:clk7-reject-odd-ratio": {"reg3.6"}, "MNB:clk6-100kHz-R6": {"reg3.6"}, "MNB:clk7-50kHz-R7": {"reg3.7"},
                  "MNB:clk6-150MHz-clamp": {"reg3.6"}, "MNB:clk7-pll-assignment-lock": {"reg3.7"}}
# object-state fields a v2.3.1 fix is ALLOWED to change (bug component, not behavior to keep)
ALLOWED_STATE = {"MNB:vcxo-876-40ppm": {"pllb_freq"}}   # DEV-08: set_vcxo() leaves pllb_freq at 800 MHz although PLLB runs at 876 MHz
LOCK_REGS = [3, 15] + list(range(16, 24)) + list(range(26, 50)) + list(range(50, 90, 8)) + list(range(42, 90)) + [90, 91, 92, 162, 163, 164] + list(range(165, 171)) + [183]
LOCK_REGS = sorted(set(LOCK_REGS))

def golden_checks(g, run):
    """lib_* register expectations from golden_cases.json (the v2.2.0 regression lock)."""
    regs, out = run["regs"], []
    for k, v in g.items():
        if k in ("lib_plla_regs", "lib_pllb_regs", "lib_pll_regs", "lib_ms_regs", "lib_ms67_regs", "lib_clk_ctrl", "lib_vcxo_regs"):
            for reg, val in v.items(): out.append((int(reg), val))
        elif k == "lib_reg92": out.append((92, v))
        elif k == "lib_reg3_after": out.append((3, v))
        elif k == "lib_write": out.append(("write", v["reg"], v["value"]))
    return out

def run_mnb(runs, lock):
    res = []
    gold = {c["id"]: c for c in json.loads(GOLDEN67.read_text())["cases"]} if GOLDEN67.exists() else {}
    for sid, gid in MNB_MAP.items():
        allowed = ALLOWED_CHANGE.get(sid, set())
        for pat in (0, 1):
            run = runs.get((sid, pat))
            if run is None: res.append((sid, pat, False, "missing run", "", "")); continue
            if run["crash"]: res.append((sid, pat, False, "crash", run["crash"], "no crash")); continue
            # 1) golden_cases.json lib_* fields (pattern 0 only: goldens assume a zeroed register file)
            if gid and pat == 0 and gid in gold:
                g = gold[gid]
                if gid == "S-xtal-load-6-8-10pF":
                    want = {"MNB:xtal-6pF": 82, "MNB:xtal-10pF": 210}[sid]; ok = run["regs"][183] == want
                    res.append((sid, pat, ok, f"golden {gid} reg183", run["regs"][183], want))
                elif gid == "A-init-crystal-load-8pF":
                    res.append((sid, pat, run["regs"][183] == 146, f"golden {gid} reg183", run["regs"][183], 146))
                for c in golden_checks(g, run):
                    if c[0] == "write":
                        got = [v for r, v in all_writes(run) if r == c[1]]
                        res.append((sid, pat, c[2] in got, f"golden {gid} write reg{c[1]}", got, c[2]))
                    else:
                        reg, val = c; got = run["regs"][reg]
                        if reg == 3 and allowed: mask = ~sum(1 << int(a.split(".")[1]) for a in allowed) & 0xFF; got, val = got & mask, val & mask
                        res.append((sid, pat, got == val, f"golden {gid} reg{reg}", got, val))
            # 2) full lock of the 1ddd43e register map for the locked register set + return codes + PLL/clk state
            L = lock.get(f"{sid}#{pat}")
            if L:
                for reg in LOCK_REGS:
                    got, want = run["regs"][reg], L["regs"][str(reg)]
                    if reg == 3 and allowed:
                        mask = ~sum(1 << int(a.split(".")[1]) for a in allowed) & 0xFF; got, want = got & mask, want & mask
                    if got != want: res.append((sid, pat, False, f"1ddd43e lock reg{reg}", got, want))
                for k in ("rets",):
                    res.append((sid, pat, run[k] == L[k], "1ddd43e lock return codes", run[k], L[k]))
                for k in ("plla_freq", "pllb_freq", "clk_freq", "pll_assignment"):
                    if k in ALLOWED_STATE.get(sid, ()): continue
                    res.append((sid, pat, run["state"][k] == L["state"][k], f"1ddd43e lock state.{k}", run["state"][k], L["state"][k]))
                rst = [v for r, v in all_writes(run) if r == 177]
                res.append((sid, pat, rst == L["reg177_writes"], "1ddd43e lock PLL-reset (reg177) write sequence", rst, L["reg177_writes"]))
    return res

def make_lock(runs):
    lock = {}
    for (sid, pat), run in runs.items():
        if sid in MNB_MAP and not run["crash"]:
            lock[f"{sid}#{pat}"] = {"regs": {str(r): run["regs"][r] for r in LOCK_REGS}, "rets": run["rets"],
                                   "state": {k: run["state"][k] for k in ("plla_freq", "pllb_freq", "clk_freq", "pll_assignment")},
                                   "reg177_writes": [v for r, v in all_writes(run) if r == 177]}
    return lock

def scenario_text():
    t = {}
    for l in (HERE / "scenarios_v231.inc").read_text().splitlines():
        m = re.match(r'add\("([^"]+)",\s*\[\]\s*\{\s*(.*)\s*\}\);', l.strip())
        if m: t[m.group(1)] = m.group(2)
    return t

def main():
    a = sys.argv[1:]
    path = Path(a[0]) if a and not a[0].startswith("--") else HERE / "logs/acc_master.jsonl"
    runs = {}
    for l in open(path):
        r = json.loads(l); runs[(r["id"], r["pattern"])] = r
    vcxo_ref = runs[("MNB:vcxo-876-40ppm", 0)]["regs"][34:42]
    seq = scenario_text()
    lock_path = HERE / "must_not_break.json"
    if "--write-golden" in a:
        lock_path.write_text(json.dumps({"source": f"{path.name} (library 1ddd43e)", "lock_regs": LOCK_REGS, "allowed_change": {k: sorted(v) for k, v in ALLOWED_CHANGE.items()}, "allowed_state_change": {k: sorted(v) for k, v in ALLOWED_STATE.items()},
                                         "golden_map": MNB_MAP, "info_only": INFO_ONLY, "locks": make_lock(runs)}, indent=1))
    lock = json.loads(lock_path.read_text())["locks"] if lock_path.exists() else {}

    print(f"== ACCEPTANCE (expected-correct per AN619/datasheet) on {path.name}")
    acc_rows, n_pass = [], 0
    for tid, item, sid, title, fn, deb in ACCEPT:
        rp = {p: runs[(sid, p)] for p in (0, 1)}
        if fn is None: fn = lambda rp: t_vcxo(rp, vcxo_ref)
        checks = fn(rp)
        ok = all(c[0] for c in checks); n_pass += ok
        print(f"{'PASS' if ok else 'FAIL'} {tid} [{item}] {title}" + (f"  (debatable {deb})" if deb else ""))
        for c in checks:
            if not c[0] or ok: print(f"      {'ok ' if c[0] else 'BAD'} {c[1]}: observed={c[2]}  expected={c[3]}")
        acc_rows.append({"id": tid, "item": item, "scenario": sid, "calls": seq.get(sid), "title": title, "debatable": deb,
                         "pass": ok, "checks": [{"ok": c[0], "what": c[1], "observed": str(c[2]), "expected": str(c[3])} for c in checks]})
    print(f"acceptance: {n_pass}/{len(ACCEPT)} pass, {len(ACCEPT)-n_pass} fail")

    mres = run_mnb(runs, lock)
    bad = [r for r in mres if not r[2]]
    print(f"\n== MUST-NOT-BREAK: {len(MNB_MAP)} scenarios x 2 patterns, {len(mres)} checks, {len(bad)} failures")
    for r in bad[:60]: print(f"   FAIL {r[0]} p{r[1]} {r[3]}: observed={r[4]} expected={r[5]}")
    for sid, why in INFO_ONLY.items():
        r = runs.get((sid, 0)); print(f"   INFO {sid}: rets={r['rets'] if r else None}  ({why})")

    if "--write-golden" in a:
        (HERE / "v231_acceptance.json").write_text(json.dumps({
            "library": "etherkit/Si5351Arduino master 1ddd43e (v2.3.0)", "runner_output": path.name,
            "refs": {"AN619": "ref/AN619.txt", "datasheet": "ref/datasheet.txt"},
            "tests": acc_rows, "debatable": DEBATABLE}, indent=1))
    rc = 1 if bad else 0
    if "--expect-master" in a and n_pass != 0: print("ERROR: some acceptance tests already pass on this build"); rc = 1
    if "--require-fixed" in a and n_pass != len(ACCEPT): print("ERROR: acceptance tests still failing"); rc = 1
    print("\nRESULT:", "OK" if rc == 0 else "NOT OK"); return rc

if __name__ == "__main__":
    sys.exit(main())
