#!/usr/bin/env python3
"""Compare original vs refactored harness output and check goldens vs ORIGINAL C++."""
import json, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
GOLDEN = ROOT / "logs/golden_cases.snapshot.json"   # pinned copy of /workspace/si5351-regression/golden_cases.json
ORIG27 = json.loads((ROOT / "orig27_ids.json").read_text())
ORIG_P = sys.argv[1] if len(sys.argv) > 1 else ROOT / "logs/orig.jsonl"
REFAC_P = sys.argv[2] if len(sys.argv) > 2 else ROOT / "logs/refac.jsonl"
REPORT_P = sys.argv[3] if len(sys.argv) > 3 else ROOT / "logs/compare_report.txt"
orig = [json.loads(l) for l in open(ORIG_P)]
refac = [json.loads(l) for l in open(REFAC_P)]
report = []
def say(s=""):
    print(s); report.append(s)

# ---------------- Task A.2: behavioural equivalence ----------------
assert len(orig) == len(refac), (len(orig), len(refac))
diffs = 0
tx_events = 0
for o, r in zip(orig, refac):
    assert o["id"] == r["id"] and o["pattern"] == r["pattern"]
    tx_events += len(o["tx"])
    problems = []
    if o["rets"] != r["rets"]:
        problems.append(f"return codes differ: orig={o['rets']} refac={r['rets']}")
    if o["tx"] != r["tx"]:
        n = next((i for i, (a, b) in enumerate(zip(o["tx"], r["tx"])) if a != b), min(len(o["tx"]), len(r["tx"])))
        last_call = next((o["tx"][j] for j in range(min(n, len(o["tx"]) - 1), -1, -1) if o["tx"][j].startswith("CALL")), "?")
        problems.append(f"I2C transcript differs at event #{n} (during {last_call}): "
                        f"orig={o['tx'][n] if n < len(o['tx']) else '<end>'} | refac={r['tx'][n] if n < len(r['tx']) else '<end>'}"
                        f" (len orig={len(o['tx'])} refac={len(r['tx'])})")
    rd = [(i, a, b) for i, (a, b) in enumerate(zip(o["regs"], r["regs"])) if a != b]
    if rd:
        problems.append("register file differs: " + ", ".join(f"reg{i}: orig=0x{a:02X} refac=0x{b:02X}" for i, a, b in rd[:12]))
    if o["state"] != r["state"]:
        problems.append(f"object state differs: orig={o['state']} refac={r['state']}")
    if problems:
        diffs += 1
        say(f"DIFF {o['id']} (pattern {o['pattern']}):")
        for p in problems: say("   " + p)

ids = sorted({x["id"] for x in orig})
n_g = len([i for i in ids if i.startswith("G:")]); n_x = len([i for i in ids if i.startswith("X:")]); n_f = len([i for i in ids if i.startswith("F:")]); n_n = len([i for i in ids if i.startswith("N:")]); n_h = len([i for i in ids if i.startswith("H:")])
say(f"Equivalence: {len(ids)} scenarios ({n_g} from the 27 original goldens, {n_n} from newer lib_* goldens, {n_x} targeted P0/P1, {n_h} FreqChecks.ino hardware sequences, {n_f} seeded fuzz) x 2 register-file patterns "
    f"= {len(orig)} runs, {tx_events} I2C/marker events compared; differing runs: {diffs}")

# ---------------- Task A.3: goldens vs ORIGINAL C++ ----------------
def decode(b):
    p3 = (b[0] << 8) | b[1] | ((b[5] >> 4) << 16)
    p1 = ((b[2] & 3) << 16) | (b[3] << 8) | b[4]
    p2 = ((b[5] & 0xF) << 16) | (b[6] << 8) | b[7]
    return p1, p2, p3
def frac(a, b, c):
    q = (128 * b) // c
    return (128 * a + q - 512) & 0xFFFFFFFF, (128 * b - c * q) & 0xFFFFFFFF, c

runs = {(x["id"], x["pattern"]): x for x in orig}
gold = json.loads(GOLDEN.read_text())
gfail, gchecks = [], 0
def chk(cid, what, got, exp):
    global gchecks
    gchecks += 1
    if got != exp: gfail.append(f"{cid}: {what}: C++ wrote {got}, golden says {exp}")

def check_pll(cid, run, base, g, prefix="pll_"):
    regs = run["regs"]
    blk = regs[base:base + 8]
    p = decode(blk)
    chk(cid, f"PLL@{base} p1/p2/p3", p, (g[prefix + "p1"], g[prefix + "p2"], g[prefix + "p3"]))
    if prefix + "a" in g:
        chk(cid, f"PLL a/b/c -> p consistency", p, frac(g[prefix + "a"], g[prefix + "b"], g[prefix + "c"]))
    if "pll_packed_bytes" in g:
        chk(cid, f"PLL@{base} packed bytes", blk, g["pll_packed_bytes"])

def check_ms05(cid, run, clk, g):
    regs = run["regs"]; base = 42 + 8 * clk
    blk = regs[base:base + 8]
    p = decode(blk)
    chk(cid, f"MS{clk} p1/p2/p3", p, (g["ms_p1"], g["ms_p2"], g["ms_p3"]))
    exp_abc = (0, 0, 1) if g.get("div_by_4") else frac(g["ms_a"], g["ms_b"], g["ms_c"])
    chk(cid, f"MS{clk} a/b/c -> p consistency", p, exp_abc)
    if "ms_packed_bytes" in g:
        chk(cid, f"MS{clk} packed bytes (excl. byte2)", blk[:2] + blk[3:], g["ms_packed_bytes"][:2] + g["ms_packed_bytes"][3:])
    chk(cid, f"MS{clk} byte+2 (ms_div byte, reg {base+2})", blk[2], g["ms_div_byte"])
    chk(cid, f"MS{clk} r_div field", (blk[2] >> 4) & 7, g["r_div"])
    chk(cid, f"MS{clk} DIVBY4 field", (blk[2] >> 2) & 3, 3 if g["div_by_4"] else 0)
    chk(cid, f"CLK{clk}_CTRL integer-mode bit", (regs[16 + clk] >> 6) & 1, g["int_mode"])

def calls_after_last(run, name):
    tx = run["tx"]; i = max(j for j, e in enumerate(tx) if e.startswith("CALL " + name))
    return tx[i:]

for g in [c for c in gold["cases"] if c["id"] in ORIG27]:
    cid = g["id"]
    if g["api"] == "parity_check":
        a = runs[("G:" + cid + "#auto", 0)]; m = runs[("G:" + cid + "#manual", 0)]
        if "auto_ms" in g:
            for tag, run, ms in (("auto", a, g["auto_ms"]), ("manual", m, g["manual_ms"])):
                p = decode(run["regs"][42:50])
                chk(cid, f"{tag} MS0 p1/p2/p3", p, (ms["p1"], ms["p2"], ms["p3"]))
                chk(cid, f"{tag} MS0 r_div", (run["regs"][44] >> 4) & 7, ms["r_div"])
            chk(cid, "manual PLLA p1/p2/p3", decode(m["regs"][26:34]), (g["manual_pll"]["p1"], g["manual_pll"]["p2"], g["manual_pll"]["p3"]))
        else:
            for tag, run in (("auto", a), ("manual", m)):
                check_ms05(cid + "#" + tag, run, 0, g[tag]); check_pll(cid + "#" + tag, run, 26, g[tag])
        chk(cid, "auto vs manual MS0 bytes identical (ms_match)", a["regs"][42:50] == m["regs"][42:50], g["ms_match"])
        if "pll_match" in g:
            chk(cid, "auto vs manual PLLA bytes identical (pll_match)", a["regs"][26:34] == m["regs"][26:34], g["pll_match"])
        continue
    run = runs[("G:" + cid, 0)]
    rets = run["rets"]
    if g["api"] == "set_pll":   # reset baseline
        check_pll(cid, run, 26, g); check_pll(cid, run, 34, g)
        chk(cid, "pll_assignment after reset", run["state"]["pll_assignment"], [0]*6 + [1, 1])
        continue
    if g["api"] == "set_vcxo":
        check_pll(cid, run, 34, g)
        chk(cid, "VCXO regs 162-164", run["regs"][162:165], g["vcxo_bytes"])
        continue
    if g.get("clk_path") == "CLK6/7":
        clk = 7 if "clk7" in cid else 6
        if g.get("rejected"):
            chk(cid, "set_freq return (reject)", rets[-1], "set_freq=1")
            chk(cid, "clk_freq[7] untouched", run["state"]["clk_freq"][7], 0)
            chk(cid, "no I2C writes after rejected call", [e for e in calls_after_last(run, "set_freq") if e.startswith("W ")], [])
            continue
        chk(cid, "set_freq return", rets[-1], "set_freq=0")
        chk(cid, f"MS{clk} single byte reg {90 + clk - 6}", run["regs"][90 + clk - 6], g["clk67_ms_register_byte"])
        r92 = run["regs"][92]
        chk(cid, f"reg92 r_div field CLK{clk}", (r92 & 7) if clk == 6 else ((r92 >> 4) & 7), g["r_div"])
        check_pll(cid, run, 34, g)
        if "pll_freq_lib" in g: chk(cid, "pllb_freq state", run["state"]["pllb_freq"], g["pll_freq_lib"])
        continue
    # CLK0-5 set_freq / set_freq_manual
    clk = 1 if cid in ("P0-clk1-PLLB-14MHz", "P0-manual-DIVBY4-175MHz-pll700") else 0
    pll_base = 34 if (g.get("pll_assignment") == "PLLB" or clk == 1 or "vcxo" in cid) else 26
    chk(cid, f"{g['api']} return", rets[-1], f"{g['api']}=0")
    check_ms05(cid, run, clk, g)
    check_pll(cid, run, pll_base, g)
    if "pll_rewritten_by_set_correction" in g:
        check_pll(cid + "[set_correction]", run, 26, g["pll_rewritten_by_set_correction"])
    if g.get("pll_assignment") == "PLLB":
        chk(cid, f"CLK{clk}_CTRL PLL_SELECT bit", (run["regs"][16 + clk] >> 5) & 1, 1)
    if "pll_reset" in g:
        did_reset = any(e.startswith("W a=60 r=177 ") for e in calls_after_last(run, g["api"]))
        chk(cid, "pll_reset (reg 177 write) in this call", did_reset, g["pll_reset"])
    if g.get("branch") == "recalc_pll_gt_100mhz":
        chk(cid, "plla_freq state", run["state"]["plla_freq"], g["pll_freq_lib"])

assert len([c for c in gold["cases"] if c["id"] in ORIG27]) == 27
say(f"Goldens vs ORIGINAL C++: 27 original golden cases, {gchecks} field checks, {len(gfail)} mismatches")
for f in gfail: say("   GOLDEN MISMATCH " + f)

# ---------------- bonus: newer golden cases with lib_* register maps ----------------
nfail, nchecks, ncases = [], 0, 0
for g in gold["cases"]:
    key = ("N:" + g["id"], 0)
    if key not in runs: continue
    ncases += 1
    run = runs[key]; regs = run["regs"]
    def nchk(what, got, exp):
        global nchecks
        nchecks += 1
        if got != exp: nfail.append(f"{g['id']}: {what}: C++ wrote {got}, golden says {exp}")
    for k, v in g.items():
        if k in ("lib_plla_regs", "lib_pllb_regs", "lib_pll_regs", "lib_ms_regs", "lib_ms67_regs", "lib_clk_ctrl", "lib_vcxo_regs"):
            for reg, val in v.items(): nchk(f"{k} reg{reg}", regs[int(reg)], val)
        elif k == "lib_reg92": nchk("reg92", regs[92], v)
        elif k == "lib_reg3_after": nchk("reg3 (output enable)", regs[3], v)
        elif k == "lib_reg177_writes":
            tx = run["tx"]; i = max(j for j, e in enumerate(tx) if e.startswith("CALL reset()"))
            got = [int(e.split("[")[1].rstrip("]"), 16) for e in tx[i:] if e.startswith("W a=60 r=177 ")]
            nchk("reg177 writes during reset()", got, v)
say(f"Bonus - newer lib_* golden cases vs ORIGINAL C++: {ncases} cases, {nchecks} register checks, {len(nfail)} mismatches")
for f in nfail: say("   NEW-GOLDEN MISMATCH " + f)

Path(REPORT_P).write_text("\n".join(report) + "\n")
sys.exit(1 if (diffs or gfail) else 0)  # bonus new-golden mismatches reported, not gating
