#!/usr/bin/env python3
"""
Host-side mirror of etherkit/Si5351Arduino pure-math helpers from master
(commit 0559cbf82865e2957d46af72e70cc88207421434, library 2.2.0).

Faithfully ports:
  pll_calc, multisynth_calc, multisynth67_calc, select_r_div, select_r_div_ms67
plus register packing used by set_pll / set_ms (CLK0-5 path).

Frequencies in this library are Hz * SI5351_FREQ_MULT (100).
"""
from __future__ import annotations
from dataclasses import dataclass, asdict
from typing import Optional, Tuple, List, Dict, Any
import json

# --- constants from si5351.h ---
SI5351_XTAL_FREQ = 25_000_000
SI5351_PLL_FIXED = 80_000_000_000  # 800 MHz * 100
SI5351_FREQ_MULT = 100

SI5351_PLL_VCO_MIN = 600_000_000
SI5351_PLL_VCO_MAX = 900_000_000
SI5351_MULTISYNTH_MIN_FREQ = 500_000
SI5351_MULTISYNTH_DIVBY4_FREQ = 150_000_000
SI5351_MULTISYNTH_MAX_FREQ = 225_000_000
SI5351_MULTISYNTH_SHARE_MAX = 100_000_000
SI5351_CLKOUT_MIN_FREQ = 4_000
SI5351_MULTISYNTH67_A_MAX = 254
SI5351_CLKOUT67_MS_MIN = SI5351_PLL_VCO_MIN // SI5351_MULTISYNTH67_A_MAX
SI5351_CLKOUT67_MIN_FREQ = SI5351_CLKOUT67_MS_MIN // 128

SI5351_PLL_A_MIN = 15
SI5351_PLL_A_MAX = 90
SI5351_MULTISYNTH_A_MIN = 6
SI5351_MULTISYNTH_A_MAX = 1800

RFRAC_DENOM = 1_000_000

SI5351_OUTPUT_CLK_DIV_1 = 0
SI5351_OUTPUT_CLK_DIV_2 = 1
SI5351_OUTPUT_CLK_DIV_4 = 2
SI5351_OUTPUT_CLK_DIV_8 = 3
SI5351_OUTPUT_CLK_DIV_16 = 4
SI5351_OUTPUT_CLK_DIV_32 = 5
SI5351_OUTPUT_CLK_DIV_64 = 6
SI5351_OUTPUT_CLK_DIV_128 = 7

SI5351_VCXO_PULL_MIN = 30
SI5351_VCXO_PULL_MAX = 240
SI5351_VCXO_MARGIN = 103

R_DIV_NAMES = {
    0: "DIV_1", 1: "DIV_2", 2: "DIV_4", 3: "DIV_8",
    4: "DIV_16", 5: "DIV_32", 6: "DIV_64", 7: "DIV_128",
}


@dataclass
class RegSet:
    p1: int
    p2: int
    p3: int


def do_div(n: int, base: int) -> Tuple[int, int]:
    """Return (n // base, n % base) mirroring do_div macro semantics on *n."""
    return n // base, n % base


def _c_div_trunc(n: int, d: int) -> int:
    """C99 toward-zero integer division (Python // is floor for negatives)."""
    if d < 0:
        n, d = -n, -d
    if n >= 0:
        return n // d
    return -((-n) // d)


def apply_correction(ref_freq: int, correction: int) -> int:
    """Exact C: ref_freq + (int32_t)((((((int64_t)correction) << 31) / 1000000000LL) * ref_freq) >> 31)

    C99 '/' truncates toward zero; '>>' on a negative int64 is arithmetic on
    gcc (verified against compiled C for +100000 / -50000 ppb).
    """
    term = _c_div_trunc(int(correction) << 31, 1_000_000_000) * ref_freq
    delta = term >> 31  # Python >> is arithmetic for negatives (matches gcc)
    delta &= 0xFFFFFFFF  # (int32_t) cast
    if delta >= 0x80000000:
        delta -= 0x100000000
    return ref_freq + delta


def pll_calc(
    pll_freq: int,
    ref_xtal_hz: int = SI5351_XTAL_FREQ,
    correction: int = 0,
    vcxo: bool = False,
) -> Tuple[RegSet, int, Dict[str, int]]:
    """Mirror Si5351::pll_calc. Returns (reg, return_value, intermediates a/b/c)."""
    ref_freq = ref_xtal_hz * SI5351_FREQ_MULT
    ref_freq = apply_correction(ref_freq, correction)

    freq = pll_freq
    if freq < SI5351_PLL_VCO_MIN * SI5351_FREQ_MULT:
        freq = SI5351_PLL_VCO_MIN * SI5351_FREQ_MULT
    if freq > SI5351_PLL_VCO_MAX * SI5351_FREQ_MULT:
        freq = SI5351_PLL_VCO_MAX * SI5351_FREQ_MULT

    a = freq // ref_freq
    # NOTE: library clamps freq but does NOT recompute `a` (stale a is used
    # for P1). Mirrored exactly; see spec deviation DEV-PLL-STALE-A.
    if a < SI5351_PLL_A_MIN:
        freq = ref_freq * SI5351_PLL_A_MIN
    if a > SI5351_PLL_A_MAX:
        freq = ref_freq * SI5351_PLL_A_MAX

    if vcxo:
        b = ((freq % ref_freq) * 1_000_000) // ref_freq
        c = 1_000_000
    else:
        b = ((freq % ref_freq) * RFRAC_DENOM) // ref_freq
        c = RFRAC_DENOM if b else 1

    p1 = 128 * a + ((128 * b) // c) - 512
    p2 = 128 * b - c * ((128 * b) // c)
    p3 = c

    # Recalculate frequency as fIN * (a + b/c)
    lltmp = ref_freq * b
    lltmp, _ = do_div(lltmp, c)
    freq_out = lltmp + ref_freq * a

    reg = RegSet(p1=p1, p2=p2, p3=p3)
    if vcxo:
        ret = 128 * a * 1_000_000 + b
    else:
        ret = freq_out
    return reg, ret, {"a": a, "b": b, "c": c, "ref_freq_corrected": ref_freq}


def multisynth_calc(freq: int, pll_freq: int) -> Tuple[RegSet, int, Dict[str, Any]]:
    """Mirror Si5351::multisynth_calc."""
    divby4 = 0
    ret_val = 0

    if freq > SI5351_MULTISYNTH_MAX_FREQ * SI5351_FREQ_MULT:
        freq = SI5351_MULTISYNTH_MAX_FREQ * SI5351_FREQ_MULT
    if freq < SI5351_MULTISYNTH_MIN_FREQ * SI5351_FREQ_MULT:
        freq = SI5351_MULTISYNTH_MIN_FREQ * SI5351_FREQ_MULT

    if freq >= SI5351_MULTISYNTH_DIVBY4_FREQ * SI5351_FREQ_MULT:
        divby4 = 1

    a = b = c = 0
    if pll_freq == 0:
        if divby4 == 0:
            lltmp = SI5351_PLL_VCO_MAX * SI5351_FREQ_MULT
            lltmp, _ = do_div(lltmp, freq)
            if lltmp == 5:
                lltmp = 4
            elif lltmp == 7:
                lltmp = 6
            a = int(lltmp)
        else:
            a = 4
        b = 0
        c = 1
        pll_freq = a * freq
    else:
        ret_val = 1
        a = pll_freq // freq
        # NOTE: library clamps freq but does NOT recompute `a` (stale a).
        # Mirrored exactly; see spec deviation DEV-MS-STALE-A.
        if a < SI5351_MULTISYNTH_A_MIN:
            freq = pll_freq // SI5351_MULTISYNTH_A_MIN
        if a > SI5351_MULTISYNTH_A_MAX:
            freq = pll_freq // SI5351_MULTISYNTH_A_MAX
        b = ((pll_freq % freq) * RFRAC_DENOM) // freq
        c = RFRAC_DENOM if b else 1

    if divby4 == 1:
        p3, p2, p1 = 1, 0, 0
    else:
        p1 = 128 * a + ((128 * b) // c) - 512
        p2 = 128 * b - c * ((128 * b) // c)
        p3 = c

    reg = RegSet(p1=p1, p2=p2, p3=p3)
    out = pll_freq if ret_val == 0 else freq
    return reg, out, {"a": a, "b": b, "c": c, "divby4": divby4, "pll_freq_used_or_computed": pll_freq if ret_val == 0 else None}


def multisynth67_calc(freq: int, pll_freq: int) -> Tuple[RegSet, int, Dict[str, Any]]:
    """Mirror Si5351::multisynth67_calc."""
    if freq > SI5351_MULTISYNTH_DIVBY4_FREQ * SI5351_FREQ_MULT:
        freq = SI5351_MULTISYNTH_DIVBY4_FREQ * SI5351_FREQ_MULT
    if freq < SI5351_MULTISYNTH_MIN_FREQ * SI5351_FREQ_MULT:
        freq = SI5351_MULTISYNTH_MIN_FREQ * SI5351_FREQ_MULT

    if pll_freq == 0:
        lltmp = (SI5351_PLL_VCO_MAX * SI5351_FREQ_MULT) - 100_000_000
        lltmp, _ = do_div(lltmp, freq)
        a = int(lltmp)
        if a % 2 != 0:
            a += 1
        if a < SI5351_MULTISYNTH_A_MIN:
            a = SI5351_MULTISYNTH_A_MIN
        if a > SI5351_MULTISYNTH67_A_MAX:
            a = SI5351_MULTISYNTH67_A_MAX
        pll_freq = a * freq
        if pll_freq > (SI5351_PLL_VCO_MAX * SI5351_FREQ_MULT):
            a -= 2
            pll_freq = a * freq
        elif pll_freq < (SI5351_PLL_VCO_MIN * SI5351_FREQ_MULT):
            a += 2
            pll_freq = a * freq
        reg = RegSet(p1=a & 0xFF, p2=0, p3=0)
        return reg, pll_freq, {"a": a}
    else:
        if pll_freq % freq:
            return RegSet(0, 0, 0), 0, {"a": None, "error": "not_integer_divide"}
        a = pll_freq // freq
        if a < SI5351_MULTISYNTH_A_MIN or a > SI5351_MULTISYNTH67_A_MAX:
            return RegSet(0, 0, 0), 0, {"a": a, "error": "a_out_of_range"}
        reg = RegSet(p1=a & 0xFF, p2=0, p3=0)
        return reg, 1, {"a": a}


def select_r_div(freq: int) -> Tuple[int, int]:
    """Mirror select_r_div; returns (r_div_code, adjusted_freq)."""
    r_div = SI5351_OUTPUT_CLK_DIV_1
    f = freq
    lo = SI5351_CLKOUT_MIN_FREQ * SI5351_FREQ_MULT
    if lo <= f < lo * 2:
        r_div, f = SI5351_OUTPUT_CLK_DIV_128, f * 128
    elif lo * 2 <= f < lo * 4:
        r_div, f = SI5351_OUTPUT_CLK_DIV_64, f * 64
    elif lo * 4 <= f < lo * 8:
        r_div, f = SI5351_OUTPUT_CLK_DIV_32, f * 32
    elif lo * 8 <= f < lo * 16:
        r_div, f = SI5351_OUTPUT_CLK_DIV_16, f * 16
    elif lo * 16 <= f < lo * 32:
        r_div, f = SI5351_OUTPUT_CLK_DIV_8, f * 8
    elif lo * 32 <= f < lo * 64:
        r_div, f = SI5351_OUTPUT_CLK_DIV_4, f * 4
    elif lo * 64 <= f < lo * 128:
        r_div, f = SI5351_OUTPUT_CLK_DIV_2, f * 2
    return r_div, f


def select_r_div_ms67(freq: int) -> Tuple[int, int]:
    r_div = SI5351_OUTPUT_CLK_DIV_1
    f = freq
    lo = SI5351_CLKOUT67_MIN_FREQ * SI5351_FREQ_MULT
    if lo <= f < lo * 2:
        r_div, f = SI5351_OUTPUT_CLK_DIV_128, f * 128
    elif lo * 2 <= f < lo * 4:
        r_div, f = SI5351_OUTPUT_CLK_DIV_64, f * 64
    elif lo * 4 <= f < lo * 8:
        r_div, f = SI5351_OUTPUT_CLK_DIV_32, f * 32
    elif lo * 8 <= f < lo * 16:
        r_div, f = SI5351_OUTPUT_CLK_DIV_16, f * 16
    elif lo * 16 <= f < lo * 32:
        r_div, f = SI5351_OUTPUT_CLK_DIV_8, f * 8
    elif lo * 32 <= f < lo * 64:
        r_div, f = SI5351_OUTPUT_CLK_DIV_4, f * 4
    elif lo * 64 <= f < lo * 128:
        r_div, f = SI5351_OUTPUT_CLK_DIV_2, f * 2
    return r_div, f


def pack_pll_params(reg: RegSet) -> List[int]:
    """8 bytes written by set_pll to PLLA_PARAMETERS (26) or PLLB (34)."""
    params = []
    params.append((reg.p3 >> 8) & 0xFF)
    params.append(reg.p3 & 0xFF)
    params.append((reg.p1 >> 16) & 0x03)
    params.append((reg.p1 >> 8) & 0xFF)
    params.append(reg.p1 & 0xFF)
    params.append(((reg.p3 >> 12) & 0xF0) + ((reg.p2 >> 16) & 0x0F))
    params.append((reg.p2 >> 8) & 0xFF)
    params.append(reg.p2 & 0xFF)
    return params


def pack_ms_params(reg: RegSet, prior_reg44_low2: int = 0) -> List[int]:
    """
    8 bytes for CLK0-5 MS parameters.
    Note: set_ms ORs p1[17:16] into existing register+2 low 2 bits after
    clearing them; prior_reg44_low2 models those preserved upper bits
    (r_div/divby4 live in same byte but are written later by ms_div).
    For golden math we report packing with prior=0 (fresh after reset path
    where ms_div overwrites the byte afterward anyway for r_div/divby4).
    """
    params = []
    params.append((reg.p3 >> 8) & 0xFF)
    params.append(reg.p3 & 0xFF)
    # reg_val &= ~0x03; temp = reg_val | (p1>>16 & 0x03)
    params.append((prior_reg44_low2 & ~0x03) | ((reg.p1 >> 16) & 0x03))
    params.append((reg.p1 >> 8) & 0xFF)
    params.append(reg.p1 & 0xFF)
    params.append(((reg.p3 >> 12) & 0xF0) + ((reg.p2 >> 16) & 0x0F))
    params.append((reg.p2 >> 8) & 0xFF)
    params.append(reg.p2 & 0xFF)
    return params


def ms_div_byte_clk05(prior: int, r_div: int, div_by_4: int) -> int:
    """Final value of MS parameter byte+2 after ms_div for CLK0-5."""
    reg_val = prior & 0xFF
    reg_val &= ~(0x7C)
    if div_by_4:
        reg_val |= (3 << 2)  # SI5351_OUTPUT_CLK_DIVBY4
    else:
        reg_val &= ~((3 << 2))
    reg_val |= (r_div << 4)
    return reg_val & 0xFF


def simulate_set_freq_clk05(
    freq_lib: int,
    pll_assignment: str = "PLLA",
    existing_pll_freq: int = SI5351_PLL_FIXED,
    correction: int = 0,
    xtal_hz: int = SI5351_XTAL_FREQ,
) -> Dict[str, Any]:
    """
    Simulate set_freq for CLK0-5 (single clock, no peer >100 MHz conflict).
    Mirrors the two branches: > SHARE_MAX recalculates PLL; else uses existing PLL.
    """
    freq = freq_lib
    # bounds
    if 0 < freq < SI5351_CLKOUT_MIN_FREQ * SI5351_FREQ_MULT:
        freq = SI5351_CLKOUT_MIN_FREQ * SI5351_FREQ_MULT
    if freq > SI5351_MULTISYNTH_MAX_FREQ * SI5351_FREQ_MULT:
        freq = SI5351_MULTISYNTH_MAX_FREQ * SI5351_FREQ_MULT

    result: Dict[str, Any] = {
        "api": "set_freq",
        "clk_path": "CLK0-5",
        "input_freq_lib": freq_lib,
        "input_freq_hz": freq_lib / SI5351_FREQ_MULT,
        "pll_assignment": pll_assignment,
        "xtal_hz": xtal_hz,
        "correction_ppb": correction,
    }

    if freq > SI5351_MULTISYNTH_SHARE_MAX * SI5351_FREQ_MULT:
        # recalculate PLL from multisynth_calc(freq, 0)
        ms_reg, pll_freq, ms_abc = multisynth_calc(freq, 0)
        pll_reg, pll_ret, pll_abc = pll_calc(pll_freq, xtal_hz, correction, vcxo=False)
        # then for this clock: select_r_div on clk_freq, multisynth_calc with pll
        temp_freq = freq
        r_div, temp_freq = select_r_div(temp_freq)
        ms_reg2, _, ms_abc2 = multisynth_calc(temp_freq, pll_freq)
        div_by_4 = 1 if temp_freq >= SI5351_MULTISYNTH_DIVBY4_FREQ * SI5351_FREQ_MULT else 0
        int_mode = 1 if div_by_4 else 0
        # Note: when >100MHz and not DIVBY4, the first ms_reg from pll_freq==0 is
        # overwritten by the recalculation loop which uses r_div-adjusted freq.
        # For freqs >100MHz, r_div is always 1 (freq >> 512kHz), so ms_reg == ms_reg2
        # when divby4 or integer a from max VCO.
        result.update({
            "branch": "recalc_pll_gt_100mhz",
            "r_div": r_div,
            "r_div_name": R_DIV_NAMES[r_div],
            "div_by_4": div_by_4,
            "int_mode": int_mode,
            "ms_a": ms_abc2["a"],
            "ms_b": ms_abc2["b"],
            "ms_c": ms_abc2["c"],
            "ms_p1": ms_reg2.p1,
            "ms_p2": ms_reg2.p2,
            "ms_p3": ms_reg2.p3,
            "pll_freq_lib": pll_freq,
            "pll_freq_hz": pll_freq / SI5351_FREQ_MULT,
            "pll_a": pll_abc["a"],
            "pll_b": pll_abc["b"],
            "pll_c": pll_abc["c"],
            "pll_p1": pll_reg.p1,
            "pll_p2": pll_reg.p2,
            "pll_p3": pll_reg.p3,
            "pll_packed_bytes": pack_pll_params(pll_reg),
            "ms_packed_bytes": pack_ms_params(ms_reg2),
            "ms_div_byte": ms_div_byte_clk05(
                pack_ms_params(ms_reg2)[2], r_div, div_by_4
            ),
            "pll_reset": True,
        })
    else:
        r_div, freq_adj = select_r_div(freq)
        ms_reg, _, ms_abc = multisynth_calc(freq_adj, existing_pll_freq)
        pll_reg, _, pll_abc = pll_calc(existing_pll_freq, xtal_hz, correction, vcxo=False)
        result.update({
            "branch": "use_existing_pll_le_100mhz",
            "r_div": r_div,
            "r_div_name": R_DIV_NAMES[r_div],
            "freq_after_r_div_lib": freq_adj,
            "div_by_4": 0,
            "int_mode": 0,
            "ms_a": ms_abc["a"],
            "ms_b": ms_abc["b"],
            "ms_c": ms_abc["c"],
            "ms_p1": ms_reg.p1,
            "ms_p2": ms_reg.p2,
            "ms_p3": ms_reg.p3,
            "pll_freq_lib": existing_pll_freq,
            "pll_freq_hz": existing_pll_freq / SI5351_FREQ_MULT,
            "pll_a": pll_abc["a"],
            "pll_b": pll_abc["b"],
            "pll_c": pll_abc["c"],
            "pll_p1": pll_reg.p1,
            "pll_p2": pll_reg.p2,
            "pll_p3": pll_reg.p3,
            "note": "set_freq does NOT rewrite PLL in this branch; pll_* shown for reset-time SI5351_PLL_FIXED baseline",
            "ms_packed_bytes": pack_ms_params(ms_reg),
            "ms_div_byte": ms_div_byte_clk05(pack_ms_params(ms_reg)[2], r_div, 0),
            "pll_reset": False,
        })
    return result


def simulate_set_freq_manual(
    freq_lib: int,
    pll_freq: int,
    correction: int = 0,
    xtal_hz: int = SI5351_XTAL_FREQ,
) -> Dict[str, Any]:
    freq = freq_lib
    if 0 < freq < SI5351_CLKOUT_MIN_FREQ * SI5351_FREQ_MULT:
        freq = SI5351_CLKOUT_MIN_FREQ * SI5351_FREQ_MULT
    if freq > SI5351_MULTISYNTH_MAX_FREQ * SI5351_FREQ_MULT:
        freq = SI5351_MULTISYNTH_MAX_FREQ * SI5351_FREQ_MULT

    r_div, freq_adj = select_r_div(freq)
    ms_reg, _, ms_abc = multisynth_calc(freq_adj, pll_freq)
    div_by_4 = 1 if freq_adj >= SI5351_MULTISYNTH_DIVBY4_FREQ * SI5351_FREQ_MULT else 0
    int_mode = 1 if div_by_4 else 0
    pll_reg, _, pll_abc = pll_calc(pll_freq, xtal_hz, correction, vcxo=False)

    return {
        "api": "set_freq_manual",
        "clk_path": "CLK0-5",
        "input_freq_lib": freq_lib,
        "input_freq_hz": freq_lib / SI5351_FREQ_MULT,
        "pll_freq_lib": pll_freq,
        "pll_freq_hz": pll_freq / SI5351_FREQ_MULT,
        "xtal_hz": xtal_hz,
        "correction_ppb": correction,
        "r_div": r_div,
        "r_div_name": R_DIV_NAMES[r_div],
        "div_by_4": div_by_4,
        "int_mode": int_mode,
        "ms_a": ms_abc["a"],
        "ms_b": ms_abc["b"],
        "ms_c": ms_abc["c"],
        "ms_p1": ms_reg.p1,
        "ms_p2": ms_reg.p2,
        "ms_p3": ms_reg.p3,
        "pll_a": pll_abc["a"],
        "pll_b": pll_abc["b"],
        "pll_c": pll_abc["c"],
        "pll_p1": pll_reg.p1,
        "pll_p2": pll_reg.p2,
        "pll_p3": pll_reg.p3,
        "pll_packed_bytes": pack_pll_params(pll_reg),
        "ms_packed_bytes": pack_ms_params(ms_reg),
        "ms_div_byte": ms_div_byte_clk05(pack_ms_params(ms_reg)[2], r_div, div_by_4),
    }


def simulate_set_freq_clk67(
    freq_lib: int,
    existing_peer_freq: Optional[int] = None,
    existing_pllb: Optional[int] = None,
    correction: int = 0,
    xtal_hz: int = SI5351_XTAL_FREQ,
) -> Dict[str, Any]:
    """Simulate set_freq for CLK6/7 when no peer, or with peer constraint."""
    freq = freq_lib
    # lower bound quirk: uses CLKOUT67_MIN but clamps to CLKOUT_MIN (4 kHz) — library bug/quirk
    if 0 < freq < SI5351_CLKOUT67_MIN_FREQ * SI5351_FREQ_MULT:
        freq = SI5351_CLKOUT_MIN_FREQ * SI5351_FREQ_MULT
    if freq >= SI5351_MULTISYNTH_DIVBY4_FREQ * SI5351_FREQ_MULT:
        freq = SI5351_MULTISYNTH_DIVBY4_FREQ * SI5351_FREQ_MULT - 1

    result: Dict[str, Any] = {
        "api": "set_freq",
        "clk_path": "CLK6/7",
        "input_freq_lib": freq_lib,
        "input_freq_hz": freq_lib / SI5351_FREQ_MULT,
        "pll_assignment": "PLLB",
        "xtal_hz": xtal_hz,
        "correction_ppb": correction,
    }

    if existing_peer_freq and existing_pllb:
        if existing_pllb % freq != 0 or (existing_pllb // freq) % 2 != 0:
            result["rejected"] = True
            result["reason"] = "peer_requires_even_integer_divide_of_pllb"
            return result
        r_div, freq_adj = select_r_div_ms67(freq)
        ms_reg, ret, abc = multisynth67_calc(freq_adj, existing_pllb)
        if ret == 0:
            result["rejected"] = True
            result["reason"] = abc.get("error", "multisynth67_calc_failed")
            return result
        pll_reg, _, pll_abc = pll_calc(existing_pllb, xtal_hz, correction)
        result.update({
            "rejected": False,
            "branch": "peer_already_set",
            "r_div": r_div,
            "r_div_name": R_DIV_NAMES[r_div],
            "ms_a": abc["a"],
            "ms_p1": ms_reg.p1,  # for MS6/7, p1 is the integer a written to single register
            "ms_p2": 0,
            "ms_p3": 0,
            "pll_freq_lib": existing_pllb,
            "pll_a": pll_abc["a"],
            "pll_b": pll_abc["b"],
            "pll_c": pll_abc["c"],
            "pll_p1": pll_reg.p1,
            "pll_p2": pll_reg.p2,
            "pll_p3": pll_reg.p3,
            "clk67_ms_register_byte": ms_reg.p1 & 0xFF,
        })
        return result

    # no peer: set PLLB from multisynth67_calc(freq, 0)
    r_div, freq_adj = select_r_div_ms67(freq)
    ms_reg, pll_freq, abc = multisynth67_calc(freq_adj, 0)
    pll_reg, _, pll_abc = pll_calc(pll_freq, xtal_hz, correction)
    result.update({
        "rejected": False,
        "branch": "set_pllb_from_clk67",
        "r_div": r_div,
        "r_div_name": R_DIV_NAMES[r_div],
        "freq_after_r_div_lib": freq_adj,
        "ms_a": abc["a"],
        "ms_p1": ms_reg.p1,
        "ms_p2": 0,
        "ms_p3": 0,
        "pll_freq_lib": pll_freq,
        "pll_freq_hz": pll_freq / SI5351_FREQ_MULT,
        "pll_a": pll_abc["a"],
        "pll_b": pll_abc["b"],
        "pll_c": pll_abc["c"],
        "pll_p1": pll_reg.p1,
        "pll_p2": pll_reg.p2,
        "pll_p3": pll_reg.p3,
        "pll_packed_bytes": pack_pll_params(pll_reg),
        "clk67_ms_register_byte": ms_reg.p1 & 0xFF,
    })
    return result


def simulate_set_vcxo(
    pll_freq: int,
    ppm: int,
    correction: int = 0,
    xtal_hz: int = SI5351_XTAL_FREQ,
) -> Dict[str, Any]:
    if ppm < SI5351_VCXO_PULL_MIN:
        ppm = SI5351_VCXO_PULL_MIN
    if ppm > SI5351_VCXO_PULL_MAX:
        ppm = SI5351_VCXO_PULL_MAX
    pll_reg, vcxo_param_raw, pll_abc = pll_calc(pll_freq, xtal_hz, correction, vcxo=True)
    # vcxo_param = ((vcxo_param * ppm * SI5351_VCXO_MARGIN) / 100ULL) / 1000000ULL;
    vcxo_param = ((vcxo_param_raw * ppm * SI5351_VCXO_MARGIN) // 100) // 1_000_000
    return {
        "api": "set_vcxo",
        "pll_freq_lib": pll_freq,
        "pll_freq_hz": pll_freq / SI5351_FREQ_MULT,
        "ppm": ppm,
        "xtal_hz": xtal_hz,
        "correction_ppb": correction,
        "pll_a": pll_abc["a"],
        "pll_b": pll_abc["b"],
        "pll_c": pll_abc["c"],
        "pll_p1": pll_reg.p1,
        "pll_p2": pll_reg.p2,
        "pll_p3": pll_reg.p3,
        "pll_packed_bytes": pack_pll_params(pll_reg),
        "vcxo_param_raw_from_pll_calc": vcxo_param_raw,
        "vcxo_param": vcxo_param,
        "vcxo_bytes": [
            vcxo_param & 0xFF,
            (vcxo_param >> 8) & 0xFF,
            (vcxo_param >> 16) & 0x3F,
        ],
        "regs": {
            "VXCO_PARAMETERS_LOW(162)": vcxo_param & 0xFF,
            "VXCO_PARAMETERS_MID(163)": (vcxo_param >> 8) & 0xFF,
            "VXCO_PARAMETERS_HIGH(164)": (vcxo_param >> 16) & 0x3F,
        },
    }


def hz(f_hz: float) -> int:
    """Convert Hz to library units (Hz * 100)."""
    return int(round(f_hz * SI5351_FREQ_MULT))


def build_golden_suite() -> List[Dict[str, Any]]:
    cases: List[Dict[str, Any]] = []

    # --- P0: common CLK0-5 with default PLL_FIXED after reset ---
    for i, f_hz in enumerate([14_000_000, 10_000_000, 7_000_000, 28_000_000]):
        c = simulate_set_freq_clk05(hz(f_hz), "PLLA", SI5351_PLL_FIXED, 0)
        c["id"] = f"P0-clk0-common-{i+1}-{int(f_hz/1e6)}MHz"
        c["priority"] = "P0"
        c["covers"] = ["CLK0-5", "fractional_or_integer_MS", "existing_PLLA_800MHz"]
        cases.append(c)

    # integer MS example: 800e6 / 16e6 = 50 exactly
    c = simulate_set_freq_clk05(hz(16_000_000), "PLLA", SI5351_PLL_FIXED, 0)
    c["id"] = "P0-clk0-integer-MS-16MHz"
    c["priority"] = "P0"
    c["covers"] = ["CLK0-5", "integer_MS", "b_eq_0"]
    cases.append(c)

    # fractional MS (14 MHz already fractional); add explicit odd freq
    c = simulate_set_freq_clk05(hz(14_070_000), "PLLA", SI5351_PLL_FIXED, 0)
    c["id"] = "P0-clk0-fractional-MS-14p07MHz"
    c["priority"] = "P0"
    c["covers"] = ["CLK0-5", "fractional_MS"]
    cases.append(c)

    # --- P0: >100 MHz path (recalc PLL) ---
    c = simulate_set_freq_clk05(hz(112_000_000), "PLLA", SI5351_PLL_FIXED, 0)
    c["id"] = "P0-clk0-gt100MHz-recalc-pll-112MHz"
    c["priority"] = "P0"
    c["covers"] = ["CLK0-5", "recalc_pll", "gt_SHARE_MAX"]
    cases.append(c)

    # --- P0: DIVBY4 path (>=150 MHz) via set_freq_manual (set_freq also uses this in recalc loop) ---
    c = simulate_set_freq_manual(hz(175_000_000), hz(700_000_000), 0)  # example sketch
    c["id"] = "P0-manual-DIVBY4-175MHz-pll700"
    c["priority"] = "P0"
    c["covers"] = ["DIVBY4", "int_mode", "set_freq_manual", "CLK0-5"]
    cases.append(c)

    c = simulate_set_freq_clk05(hz(150_000_000), "PLLA", SI5351_PLL_FIXED, 0)
    c["id"] = "P0-clk0-DIVBY4-boundary-150MHz"
    c["priority"] = "P0"
    c["covers"] = ["DIVBY4", "recalc_pll", "a_eq_4"]
    cases.append(c)

    c = simulate_set_freq_clk05(hz(200_000_000), "PLLA", SI5351_PLL_FIXED, 0)
    c["id"] = "P0-clk0-DIVBY4-200MHz"
    c["priority"] = "P0"
    c["covers"] = ["DIVBY4", "recalc_pll"]
    cases.append(c)

    # --- P0: PLLB assignment (CLK1 with set_ms_source PLLB, then set_freq <=100MHz uses pllb) ---
    c = simulate_set_freq_clk05(hz(14_000_000), "PLLB", SI5351_PLL_FIXED, 0)
    c["id"] = "P0-clk1-PLLB-14MHz"
    c["priority"] = "P0"
    c["covers"] = ["PLLB_assignment", "CLK0-5"]
    cases.append(c)

    # --- P0: correction nonzero ---
    for corr in [100_000, -50_000]:  # ppb
        c = simulate_set_freq_clk05(hz(14_000_000), "PLLA", SI5351_PLL_FIXED, corr)
        # Also show effect on PLL register when PLL is rewritten (correction path via set_pll)
        c["id"] = f"P0-corr-{corr}-14MHz-existing-pll"
        c["priority"] = "P0"
        c["covers"] = ["correction_factor"]
        # For <=100MHz set_freq doesn't rewrite PLL; show set_pll effect explicitly:
        pll_reg, _, pll_abc = pll_calc(SI5351_PLL_FIXED, SI5351_XTAL_FREQ, corr)
        c["pll_rewritten_by_set_correction"] = {
            "pll_a": pll_abc["a"], "pll_b": pll_abc["b"], "pll_c": pll_abc["c"],
            "pll_p1": pll_reg.p1, "pll_p2": pll_reg.p2, "pll_p3": pll_reg.p3,
            "pll_packed_bytes": pack_pll_params(pll_reg),
            "note": "set_correction() calls set_pll(plla_freq) and set_pll(pllb_freq); MS unchanged until next set_freq",
        }
        cases.append(c)

    # Correction with >100 MHz so both PLL and MS see corrected ref
    c = simulate_set_freq_clk05(hz(112_000_000), "PLLA", SI5351_PLL_FIXED, 100_000)
    c["id"] = "P0-corr-100000-gt100MHz-112MHz"
    c["priority"] = "P0"
    c["covers"] = ["correction_factor", "recalc_pll"]
    cases.append(c)

    # --- P0: R-divider edges ---
    # CLKOUT_MIN = 4 kHz → R=128 for [4kHz, 8kHz)
    c = simulate_set_freq_clk05(hz(5_000), "PLLA", SI5351_PLL_FIXED, 0)
    c["id"] = "P0-rdiv-128-5kHz"
    c["priority"] = "P0"
    c["covers"] = ["R_divider", "select_r_div"]
    cases.append(c)

    c = simulate_set_freq_clk05(hz(10_000), "PLLA", SI5351_PLL_FIXED, 0)
    c["id"] = "P0-rdiv-64-10kHz"
    c["priority"] = "P0"
    c["covers"] = ["R_divider"]
    cases.append(c)

    c = simulate_set_freq_clk05(hz(100_000), "PLLA", SI5351_PLL_FIXED, 0)
    c["id"] = "P0-rdiv-8-100kHz"
    c["priority"] = "P0"
    c["covers"] = ["R_divider"]
    cases.append(c)

    c = simulate_set_freq_clk05(hz(400_000), "PLLA", SI5351_PLL_FIXED, 0)
    c["id"] = "P0-rdiv-2-400kHz"
    c["priority"] = "P0"
    c["covers"] = ["R_divider"]
    cases.append(c)

    c = simulate_set_freq_clk05(hz(512_000), "PLLA", SI5351_PLL_FIXED, 0)
    c["id"] = "P0-rdiv-1-512kHz-boundary"
    c["priority"] = "P0"
    c["covers"] = ["R_divider", "no_r_div"]
    cases.append(c)

    # --- P0: CLK6/7 ---
    c = simulate_set_freq_clk67(hz(23_000_000))  # example: library sets PLL ~874 MHz
    c["id"] = "P0-clk6-23MHz"
    c["priority"] = "P0"
    c["covers"] = ["CLK6/7", "multisynth67_calc", "PLLB"]
    cases.append(c)

    # CLK7 with peer CLK6: 87.4 MHz works if pllb is 874 MHz (874/87.4=10 even)
    pllb = c["pll_freq_lib"]
    c7 = simulate_set_freq_clk67(hz(87_400_000), existing_peer_freq=hz(23_000_000), existing_pllb=pllb)
    c7["id"] = "P0-clk7-874MHz-even-div-of-clk6-pll"
    c7["priority"] = "P0"
    c7["covers"] = ["CLK6/7", "peer_even_integer"]
    cases.append(c7)

    # Reject non-integer / odd divide (12345678 Hz from example)
    c_bad = simulate_set_freq_clk67(hz(12_345_678), existing_peer_freq=hz(23_000_000), existing_pllb=pllb)
    c_bad["id"] = "P0-clk7-reject-noninteger-12p345678MHz"
    c_bad["priority"] = "P0"
    c_bad["covers"] = ["CLK6/7", "reject_path"]
    cases.append(c_bad)

    # --- P0: VCXO ---
    c = simulate_set_vcxo(hz(876_000_000), 40)  # example sketch
    c["id"] = "P0-vcxo-876MHz-40ppm"
    c["priority"] = "P0"
    c["covers"] = ["set_vcxo", "PLLB", "vcxo_param"]
    cases.append(c)

    # VCXO + manual MS for 146 MHz (example)
    c = simulate_set_freq_manual(hz(146_000_000), hz(876_000_000), 0)
    c["id"] = "P0-vcxo-manual-146MHz-on-876pll"
    c["priority"] = "P0"
    c["covers"] = ["set_freq_manual", "VCXO_companion", "near_DIVBY4"]
    cases.append(c)

    # --- P1: set_freq vs set_freq_manual parity ---
    # For freq <=100 MHz with same PLL, MS params should match if r_div/path same.
    auto = simulate_set_freq_clk05(hz(14_000_000), "PLLA", SI5351_PLL_FIXED, 0)
    manual = simulate_set_freq_manual(hz(14_000_000), SI5351_PLL_FIXED, 0)
    parity = {
        "id": "P1-parity-set_freq-vs-manual-14MHz-pll800",
        "priority": "P1",
        "covers": ["set_freq_vs_set_freq_manual_parity"],
        "api": "parity_check",
        "description": (
            "For freq<=100 MHz, set_freq uses existing PLL and does not call set_pll; "
            "set_freq_manual always calls set_pll(pll_freq). MS a/b/c and p1/p2/p3 must match "
            "when the same pll_freq is supplied. Differences: set_freq_manual always "
            "output_enable(1); set_freq enables only on first set. set_freq skips pll_reset "
            "in <=100 MHz branch; set_freq_manual does not reset either."
        ),
        "auto_ms": {"a": auto["ms_a"], "b": auto["ms_b"], "c": auto["ms_c"],
                    "p1": auto["ms_p1"], "p2": auto["ms_p2"], "p3": auto["ms_p3"],
                    "r_div": auto["r_div"], "div_by_4": auto["div_by_4"]},
        "manual_ms": {"a": manual["ms_a"], "b": manual["ms_b"], "c": manual["ms_c"],
                      "p1": manual["ms_p1"], "p2": manual["ms_p2"], "p3": manual["ms_p3"],
                      "r_div": manual["r_div"], "div_by_4": manual["div_by_4"]},
        "ms_match": (
            auto["ms_a"] == manual["ms_a"] and auto["ms_b"] == manual["ms_b"]
            and auto["ms_c"] == manual["ms_c"]
            and auto["ms_p1"] == manual["ms_p1"] and auto["ms_p2"] == manual["ms_p2"]
            and auto["ms_p3"] == manual["ms_p3"]
            and auto["r_div"] == manual["r_div"] and auto["div_by_4"] == manual["div_by_4"]
        ),
        "manual_also_writes_pll": True,
        "manual_pll": {"a": manual["pll_a"], "b": manual["pll_b"], "c": manual["pll_c"],
                       "p1": manual["pll_p1"], "p2": manual["pll_p2"], "p3": manual["pll_p3"]},
    }
    cases.append(parity)

    # High-freq parity: set_freq(175e6) recalc vs manual(175e6, computed_pll)
    auto_hi = simulate_set_freq_clk05(hz(175_000_000), "PLLA", SI5351_PLL_FIXED, 0)
    # set_freq for >100 uses multisynth_calc(freq,0) → pll = a*freq with a=4 for DIVBY4
    manual_hi = simulate_set_freq_manual(hz(175_000_000), auto_hi["pll_freq_lib"], 0)
    parity2 = {
        "id": "P1-parity-gt100-set_freq-vs-manual-175MHz",
        "priority": "P1",
        "covers": ["set_freq_vs_set_freq_manual_parity", "DIVBY4"],
        "api": "parity_check",
        "description": (
            "set_freq(175 MHz) chooses pll_freq via multisynth_calc(freq,0) → a=4, pll=700 MHz. "
            "set_freq_manual(175 MHz, 700 MHz) must yield identical MS p1/p2/p3, div_by_4=1, int_mode=1 "
            "and identical PLL a/b/c when correction matches."
        ),
        "auto": auto_hi,
        "manual": manual_hi,
        "ms_match": (
            auto_hi["ms_p1"] == manual_hi["ms_p1"]
            and auto_hi["ms_p2"] == manual_hi["ms_p2"]
            and auto_hi["ms_p3"] == manual_hi["ms_p3"]
            and auto_hi["div_by_4"] == manual_hi["div_by_4"]
        ),
        "pll_match": (
            auto_hi["pll_p1"] == manual_hi["pll_p1"]
            and auto_hi["pll_p2"] == manual_hi["pll_p2"]
            and auto_hi["pll_p3"] == manual_hi["pll_p3"]
        ),
    }
    cases.append(parity2)

    # --- P1: reset-time PLL_FIXED baseline ---
    pll_reg, _, pll_abc = pll_calc(SI5351_PLL_FIXED, SI5351_XTAL_FREQ, 0)
    cases.append({
        "id": "P1-reset-pll-fixed-800MHz",
        "priority": "P1",
        "covers": ["reset", "SI5351_PLL_FIXED", "set_pll"],
        "api": "set_pll",
        "pll_freq_lib": SI5351_PLL_FIXED,
        "pll_freq_hz": 800_000_000,
        "xtal_hz": SI5351_XTAL_FREQ,
        "correction_ppb": 0,
        "pll_a": pll_abc["a"],
        "pll_b": pll_abc["b"],
        "pll_c": pll_abc["c"],
        "pll_p1": pll_reg.p1,
        "pll_p2": pll_reg.p2,
        "pll_p3": pll_reg.p3,
        "pll_packed_bytes": pack_pll_params(pll_reg),
        "note": "reset() calls set_pll(SI5351_PLL_FIXED) for both PLLA and PLLB",
    })

    # Scenario metadata (which CLK / PLL the example or case targets)
    meta = {
        "P0-clk1-PLLB-14MHz": (1, "PLLB"),
        "P0-manual-DIVBY4-175MHz-pll700": (1, "PLLB"),   # si5351_example.ino uses CLK1 on PLLB
        "P0-clk6-23MHz": (6, "PLLB"),
        "P0-clk7-874MHz-even-div-of-clk6-pll": (7, "PLLB"),
        "P0-clk7-reject-noninteger-12p345678MHz": (7, "PLLB"),
        "P0-vcxo-876MHz-40ppm": (0, "PLLB"),
        "P0-vcxo-manual-146MHz-on-876pll": (0, "PLLB"),  # si5351_vcxo.ino uses CLK0 on PLLB
    }
    for c in cases:
        clk, pll = meta.get(c["id"], (0, c.get("pll_assignment", "PLLA")))
        c["clk"] = clk
        if c.get("api") in ("set_freq", "set_freq_manual", "set_vcxo"):
            c["pll_assignment"] = pll
        if c["id"] == "P0-vcxo-manual-146MHz-on-876pll":
            c["vcxo_context"] = True
        annotate_library_regs(c)
    return cases



# ---------------------------------------------------------------------------
# Library register-level mirrors (addressed bytes as the library writes them)
# ---------------------------------------------------------------------------
REG_CLK0_CTRL = 16
REG_PLLA = 26
REG_PLLB = 34
REG_CLK0_PARAMS = 42
REG_CLK67_DIV = 92


def lib_pll_regs(pll: str, reg: RegSet) -> Dict[str, int]:
    base = REG_PLLA if pll == "PLLA" else REG_PLLB
    return {str(base + i): v for i, v in enumerate(pack_pll_params(reg))}


def lib_ms_regs(clk: int, reg: RegSet, r_div: int, div_by_4: int, prior44: int = 0) -> Dict[str, int]:
    """set_ms bulk write followed by ms_div RMW of byte +2.

    prior44 is the pre-existing value of MS parameter byte+2 (reg 44+8*clk).
    Library sequence (si5351.cpp set_ms + ms_div):
      1. set_ms:  read byte+2; clear bits 1:0; OR P1[17:16]; include in 8-byte bulk write
         -> preserves bits 7:2 of prior (R_DIV/DIVBY4/unused) during the bulk write
      2. ms_div:  read byte+2; clear bits 6:2 (0x7C); OR R_DIV<<4 and DIVBY4; write
         -> preserves bits 1:0 (P1[17:16]) and bit 7 (unused)
    A naive full-byte overwrite at either step fails the packed-bytes golden.
    """
    base = REG_CLK0_PARAMS + 8 * clk
    b = pack_ms_params(reg, prior44)
    after_set_ms_byte2 = b[2]
    b[2] = ms_div_byte_clk05(b[2], r_div, div_by_4)
    return {str(base + i): v for i, v in enumerate(b)}


def lib_ms_byte2_rmw_transcript(clk: int, reg: RegSet, r_div: int, div_by_4: int, prior44: int) -> Dict[str, Any]:
    """Wire-stub transcript of the two RMWs on MS parameter byte+2. Future I2C mock must assert these."""
    addr = REG_CLK0_PARAMS + 8 * clk + 2
    after_set_ms = (prior44 & ~0x03) | ((reg.p1 >> 16) & 0x03)
    after_ms_div = ms_div_byte_clk05(after_set_ms, r_div, div_by_4)
    return {
        "reg": addr,
        "prior": prior44,
        "steps": [
            {"op": "si5351_read", "reg": addr, "returns": prior44,
             "note": "set_ms RMW start"},
            {"op": "si5351_write_bulk_byte2", "reg": addr, "value": after_set_ms,
             "mask_cleared": 0x03, "bits_preserved": "7:2",
             "note": "set_ms: reg_val &= ~0x03; |= P1[17:16]"},
            {"op": "si5351_read", "reg": addr, "returns": after_set_ms,
             "note": "ms_div RMW start"},
            {"op": "si5351_write", "reg": addr, "value": after_ms_div,
             "mask_cleared": 0x7C, "bits_preserved": "7 and 1:0",
             "note": "ms_div: reg_val &= ~0x7C; |= R_DIV<<4 | DIVBY4"},
        ],
        "final": after_ms_div,
        "mutation_detectors": {
            "naive_ms_div_overwrite": (r_div << 4) | ((0x0C) if div_by_4 else 0),
            "naive_set_ms_then_correct_ms_div": ms_div_byte_clk05((reg.p1 >> 16) & 0x03, r_div, div_by_4),
            "note": ("naive_ms_div_overwrite loses P1[17:16] and bit7; "
                     "naive_set_ms_then_correct_ms_div loses bit7 (and any prior R_DIV/DIVBY4 mid-bulk)"),
        },
    }


def lib_ctrl_after_reset(clk: int, pll: str) -> int:
    """reset(): write 0x80 then 0x0C to regs 16-23, then set_ms_source per default map;
    a later set_ms_source(clk, PLLB) sets bit5."""
    v = 0x0C
    if pll == "PLLB":
        v |= 0x20
    return v


def lib_set_int(prior: int, enable: int) -> int:
    return (prior | 0x40) if enable == 1 else (prior & ~0x40 & 0xFF)


def lib_ms_div_clk67(prior: int, clk: int, r_div: int) -> int:
    if clk == 6:
        return ((prior & ~0x07) | r_div) & 0xFF
    return ((prior & ~0x70) | (r_div << 4)) & 0xFF


def lib_output_enable(prior: int, clk: int, enable: int) -> int:
    return (prior & ~(1 << clk)) & 0xFF if enable == 1 else (prior | (1 << clk)) & 0xFF


REG3_AFTER_RESET = 0xFF  # reset(): output_enable(i, 0) for i=0..7 (prior assumed 0x00)


def annotate_library_regs(c: Dict[str, Any]) -> Dict[str, Any]:
    """Attach AN619-addressed register bytes as the LIBRARY would write them."""
    api = c.get("api")
    clk = c.get("clk", 0)
    pll = c.get("pll_assignment", "PLLA")
    if c.get("clk_path") == "CLK0-5" and "ms_p1" in c:
        c["lib_ms_regs"] = lib_ms_regs(clk, RegSet(c["ms_p1"], c["ms_p2"], c["ms_p3"]), c["r_div"], c["div_by_4"])
        c["lib_clk_ctrl"] = {str(REG_CLK0_CTRL + clk): lib_set_int(lib_ctrl_after_reset(clk, pll), c["int_mode"])}
        c["lib_reg3_after"] = lib_output_enable(REG3_AFTER_RESET, clk, 1)
    if c.get("clk_path") == "CLK6/7" and not c.get("rejected"):
        c["lib_ms67_regs"] = {str(90 + clk - 6): c["ms_p1"] & 0xFF}
        c["lib_reg92"] = lib_ms_div_clk67(0, clk, c["r_div"])
        # set_ms() for CLK6/7 does not call set_int(); ctrl stays at reset value (PLLB)
        c["lib_clk_ctrl"] = {str(REG_CLK0_CTRL + clk): lib_ctrl_after_reset(clk, "PLLB")}
        # set_freq() CLK6/7 branch never calls output_enable(): OEB bit stays 1
        c["lib_reg3_after"] = REG3_AFTER_RESET
    if "pll_p1" in c and api != "set_vcxo":
        c["lib_pll_regs"] = lib_pll_regs("PLLB" if c.get("clk_path") == "CLK6/7" else pll,
                                         RegSet(c["pll_p1"], c["pll_p2"], c["pll_p3"]))
    if api == "set_vcxo":
        c["lib_pll_regs"] = lib_pll_regs("PLLB", RegSet(c["pll_p1"], c["pll_p2"], c["pll_p3"]))
        c["lib_vcxo_regs"] = {"162": c["vcxo_bytes"][0], "163": c["vcxo_bytes"][1], "164": c["vcxo_bytes"][2]}
    return c


def build_spec_coverage_cases() -> List[Dict[str, Any]]:
    """New cases added for AN619/datasheet coverage (library-side values only;
    spec-side values are computed independently by spec_crosscheck.py)."""
    cases: List[Dict[str, Any]] = []

    def add(c, cid, covers, clk=0, pll="PLLA", **extra):
        c["id"], c["priority"], c["covers"], c["clk"] = cid, "S", covers, clk
        c.setdefault("pll_assignment", pll)
        c.update(extra)
        cases.append(annotate_library_regs(c))

    # S: full PLL blocks after reset() + PLL reset writes
    reg, _, abc = pll_calc(SI5351_PLL_FIXED)
    cases.append({
        "id": "S-reset-pll-blocks-and-reg177", "priority": "S", "api": "reset_plls",
        "covers": ["MSNA regs 26-33", "MSNB regs 34-41", "PLL reset reg 177", "CLK ctrl regs 16-23"],
        "pll_freq_hz": 800_000_000, "xtal_hz": SI5351_XTAL_FREQ, "correction_ppb": 0,
        "pll_a": abc["a"], "pll_b": abc["b"], "pll_c": abc["c"],
        "pll_p1": reg.p1, "pll_p2": reg.p2, "pll_p3": reg.p3,
        "lib_plla_regs": lib_pll_regs("PLLA", reg), "lib_pllb_regs": lib_pll_regs("PLLB", reg),
        "lib_reg177_writes": [0x20, 0x80],  # pll_reset(PLLA) then pll_reset(PLLB)
        "lib_clk_ctrl": {str(16 + i): lib_ctrl_after_reset(i, "PLLB" if i >= 6 else "PLLA") for i in range(8)},
        "lib_reg3_after": REG3_AFTER_RESET,
    })

    add(simulate_set_freq_clk05(hz(10_000_000)), "S-ms5-block-10MHz-clk5",
        ["MS5 regs 82-89 address offset", "reg 21"], clk=5)
    add(simulate_set_freq_clk05(hz(5_000)), "S-rdiv-placement-clk2-5kHz",
        ["R2_DIV bits 6:4 of reg 60", "MS2 regs 58-65"], clk=2)
    add(simulate_set_freq_clk05(hz(200_000_000)), "S-divby4-block-clk3-200MHz",
        ["MS3_DIVBY4 bits 3:2 of reg 68", "MS3_INT reg 19 bit6", "MSNA block"], clk=3)
    add(simulate_set_freq_clk05(hz(112_500_000)), "S-boundary-112p5MHz-ratio8",
        ["AN619 2.1.1 900/8 boundary", "integer ratio 8"], clk=0)

    c = simulate_set_freq_clk67(hz(100_000))
    add(c, "S-clk6-R6div-100kHz", ["R6_DIV bits 2:0 of reg 92", "reg 90"], clk=6, pll="PLLB")
    c = simulate_set_freq_clk67(hz(50_000))
    add(c, "S-clk7-R7div-50kHz", ["R7_DIV bits 6:4 of reg 92", "reg 91"], clk=7, pll="PLLB")
    c = simulate_set_freq_clk67(hz(150_000_000))
    add(c, "S-clk6-150MHz-upper-clamp", ["CLK6 upper bound", "MS6 ratio 6"], clk=6, pll="PLLB")

    add(simulate_set_vcxo(hz(800_000_000), 100), "S-vcxo-800MHz-100ppm", ["VCXO regs 162-164", "c=10^6"],
        clk=0, pll="PLLB")
    add(simulate_set_vcxo(hz(876_000_000), 10), "S-vcxo-ppm-clamp-low-10to30", ["VCXO APR min 30"],
        clk=0, pll="PLLB", requested_ppm=10)
    add(simulate_set_vcxo(hz(876_000_000), 250), "S-vcxo-ppm-clamp-high-250to240", ["VCXO APR max 240"],
        clk=0, pll="PLLB", requested_ppm=250)

    cases.append({"id": "S-phase-clk0-mask", "priority": "S", "api": "set_phase", "covers": ["reg 165", "PHOFF[6:0]"],
                  "clk": 0, "requested_phase": 0xFF, "lib_write": {"reg": 165 + 0, "value": 0xFF & 0x7F}})
    cases.append({"id": "S-xtal-load-6-8-10pF", "priority": "S", "api": "init_xtal_load", "covers": ["reg 183"],
                  "lib_writes": {"6pF": (1 << 6 & 0xC0) | 0x12, "8pF": (2 << 6 & 0xC0) | 0x12,
                                 "10pF": (3 << 6 & 0xC0) | 0x12}})

    # ---- D: scenarios that expose library-vs-spec deviations (library behaviour locked) ----
    add(simulate_set_freq_clk05(hz(210_000_000)), "D-clk0-210MHz-above-spec-max", ["Fout max 200 MHz"])
    add(simulate_set_freq_clk05(hz(250_000_000)), "D-clk0-250MHz-clamped-to-225", ["Fout max 200 MHz", "clamp"])
    add(simulate_set_freq_clk05(hz(2_500)), "D-clk0-2p5kHz-clamped-to-4kHz", ["Fout min 2.5 kHz"], clk=0)
    c = simulate_set_freq_clk67(hz(10_000))
    c["pllb_freq_member_lib"] = c["pll_freq_lib"]
    add(c, "D-clk6-10kHz-ms6-p1-zero", ["MS6 even 6..254", "uint8 truncation"], clk=6, pll="PLLB")
    m = simulate_set_freq_manual(hz(100_000_000), hz(700_000_000))
    add(m, "D-manual-100MHz-pll700-ratio7", ["MS ratio 7 invalid"], clk=0)
    m = simulate_set_freq_manual(hz(90_000_000), hz(600_000_000))
    add(m, "D-manual-90MHz-pll600-ratio6p67", ["MS fractional ratio < 8 invalid"], clk=0)
    m = simulate_set_freq_manual(hz(140_000_000), hz(700_000_000))
    add(m, "D-manual-140MHz-pll700-stale-a5", ["multisynth_calc stale a after a<6 clamp"], clk=0)
    m = simulate_set_freq_manual(hz(175_000_000), hz(800_000_000))
    add(m, "D-manual-175MHz-pll800-divby4-precondition", ["DIVBY4 requires fVCO = 4*Fout"], clk=0)

    # set_vcxo(876 MHz) leaves pllb_freq member at 800 MHz; set_correction() then rewrites PLLB
    reg_v, raw, abc_v = pll_calc(hz(876_000_000), vcxo=True)
    reg_b, _, abc_b = pll_calc(SI5351_PLL_FIXED)
    cases.append({
        "id": "D-vcxo-then-set_correction-clobbers-pllb", "priority": "S", "api": "vcxo_sequence",
        "covers": ["set_vcxo does not update pllb_freq", "VCXO c=10^6"],
        "sequence": ["reset()", "set_vcxo(87600000000ULL, 40)", "set_correction(0, SI5351_PLL_INPUT_XO)"],
        "vcxo_context": True,
        "pllb_after_set_vcxo": {"a": abc_v["a"], "b": abc_v["b"], "c": abc_v["c"],
                                "p1": reg_v.p1, "p2": reg_v.p2, "p3": reg_v.p3},
        "pllb_freq_member_lib": SI5351_PLL_FIXED,
        "pll_a": abc_b["a"], "pll_b": abc_b["b"], "pll_c": abc_b["c"],
        "pll_p1": reg_b.p1, "pll_p2": reg_b.p2, "pll_p3": reg_b.p3,
        "lib_pll_regs": lib_pll_regs("PLLB", reg_b), "xtal_hz": SI5351_XTAL_FREQ, "correction_ppb": 0,
    })
    # set_vcxo(875 MHz) then set_freq_manual(..., 875 MHz) -> set_pll() non-VCXO path gives c=1 (b==0)
    reg_v2, _, abc_v2 = pll_calc(hz(875_000_000), vcxo=True)
    m = simulate_set_freq_manual(hz(87_500_000), hz(875_000_000))
    m["sequence"] = ["reset()", "set_vcxo(87500000000ULL, 40)", "set_ms_source(SI5351_CLK0, SI5351_PLLB)",
                     "set_freq_manual(8750000000ULL, 87500000000ULL, SI5351_CLK0)"]
    m["pllb_after_set_vcxo"] = {"a": abc_v2["a"], "b": abc_v2["b"], "c": abc_v2["c"],
                                "p1": reg_v2.p1, "p2": reg_v2.p2, "p3": reg_v2.p3}
    add(m, "D-vcxo-875MHz-then-manual-rewrites-c1", ["VCXO c=10^6 lost via set_pll"], clk=0, pll="PLLB",
        vcxo_context=True)

    # set_freq_manual() has no CLK6/7 guard: multisynth_calc (fractional MS0-5 math) + select_r_div,
    # then set_ms() writes (uint8_t)ms_reg.p1 to reg 90/91.
    m = simulate_set_freq_manual(hz(87_400_000), hz(874_000_000))
    m.update({"id": "D-manual-clk7-87p4MHz-p1-truncated", "priority": "S", "clk": 7, "clk_path": "CLK6/7",
              "api": "set_freq_manual", "pll_assignment": "PLLB",
              "covers": ["set_freq_manual on CLK6/7", "reg 91"],
              "sequence": ["reset()", "set_freq_manual(8740000000ULL, 87400000000ULL, SI5351_CLK7)"],
              "lib_ms67_regs": {"91": m["ms_p1"] & 0xFF},
              "lib_reg92": lib_ms_div_clk67(0, 7, m["r_div"]),
              "lib_clk_ctrl": {"23": lib_ctrl_after_reset(7, "PLLB")},
              "lib_reg3_after": lib_output_enable(REG3_AFTER_RESET, 7, 1),
              "lib_pll_regs": lib_pll_regs("PLLB", RegSet(m["pll_p1"], m["pll_p2"], m["pll_p3"]))})
    cases.append(m)
    cases.append({"id": "D-set_phase-clk6-writes-reserved-171", "priority": "S", "api": "set_phase",
                  "covers": ["phase regs only CLK0-5"], "clk": 6, "requested_phase": 10,
                  "lib_write": {"reg": 165 + 6, "value": 10 & 0x7F}})
    cases.append({"id": "D-init-crystal-load-0pF-reserved", "priority": "S", "api": "init_xtal_load",
                  "covers": ["XTAL_CL=00 reserved"], "lib_writes": {"0pF": (0 & 0xC0) | 0x12}})
    # set_int(CLK6,1) after set_correction(100000) made MSNA fractional
    reg_c, _, abc_c = pll_calc(SI5351_PLL_FIXED, correction=100_000)
    cases.append({"id": "D-set_int-clk6-sets-FBA_INT-on-fractional-plla", "priority": "S", "api": "set_int",
                  "covers": ["reg 22 bit6 = FBA_INT"], "clk": 6,
                  "sequence": ["reset()", "set_correction(100000, SI5351_PLL_INPUT_XO)", "set_int(SI5351_CLK6, 1)"],
                  "lib_write": {"reg": 22, "value": lib_set_int(lib_ctrl_after_reset(6, "PLLB"), 1)},
                  "plla": {"a": abc_c["a"], "b": abc_c["b"], "c": abc_c["c"],
                           "p1": reg_c.p1, "p2": reg_c.p2, "p3": reg_c.p3}})
    # set_pll_input: reg_val |= clkin_div without clearing CLKIN_DIV[1:0]
    prior15 = 0x40 | 0x04  # earlier CLKIN_DIV=/2 and PLLA_SRC=CLKIN
    cases.append({"id": "D-set_pll_input-clkin_div-or-without-clear", "priority": "S", "api": "set_pll_input",
                  "covers": ["reg 15 CLKIN_DIV[1:0]"],
                  "sequence": ["(reg15 = 0x44 from earlier CLKIN @ 40 MHz)", "set_ref_freq(80000000, CLKIN)",
                               "set_pll_input(SI5351_PLLA, SI5351_PLL_INPUT_CLKIN)"],
                  "prior_reg15": prior15, "intended_clkin_div": 4,
                  "lib_write": {"reg": 15, "value": prior15 | 0x04 | (2 << 6)}})
    # Non-standard 5 MHz reference: pll_calc clamps freq but keeps stale a=160
    reg5, ret5, abc5 = pll_calc(SI5351_PLL_FIXED, ref_xtal_hz=5_000_000)
    cases.append({"id": "D-ref5MHz-pll_calc-stale-a160", "priority": "S", "api": "set_pll",
                  "covers": ["feedback ratio 15..90", "PLL input 10..40 MHz"],
                  "sequence": ["init(SI5351_CRYSTAL_LOAD_8PF, 5000000, 0)  -> reset() -> set_pll(800 MHz)"],
                  "pll_freq_lib": SI5351_PLL_FIXED, "xtal_hz": 5_000_000, "correction_ppb": 0,
                  "pll_a": abc5["a"], "pll_b": abc5["b"], "pll_c": abc5["c"],
                  "pll_p1": reg5.p1, "pll_p2": reg5.p2, "pll_p3": reg5.p3,
                  "pll_calc_return_lib": ret5, "lib_pll_regs": lib_pll_regs("PLLA", reg5)})
    # F: Jason's short frequency check list (FREQ_CHECKS.md) - cases not already present
    add(simulate_set_freq_clk05(hz(100_000_000)), "F-clk0-100MHz-share-max-boundary",
        ["<=100 MHz branch boundary", "ratio 8 on 800 MHz"], clk=0)
    add(simulate_set_freq_clk05(hz(4_000)), "F-clk0-4kHz-library-min",
        ["CLKOUT_MIN", "R=128"], clk=0)

    # =========================================================================
    # M: mutation gaps found by Coding PM's original-vs-refactor harness
    # =========================================================================

    # M1: CLK7 default PLL assignment is PLLB (mutation: swap to PLLA)
    # AN619 §4.1.1: MSx_SRC may be PLLA or PLLB; library reset() hard-codes CLK6/7 → PLLB.
    cases.append({
        "id": "M-clk7-pllb-assignment", "priority": "M", "api": "reset_pll_assignment",
        "covers": ["CLK7 → PLLB after reset", "reg 23 MS7_SRC bit5", "mutation: CLK7→PLLA"],
        "clk": 7,
        "pll_assignment": "PLLB",
        "pll_assignment_table": {str(i): ("PLLB" if i >= 6 else "PLLA") for i in range(8)},
        "lib_clk_ctrl": {str(16 + i): lib_ctrl_after_reset(i, "PLLB" if i >= 6 else "PLLA") for i in range(8)},
        "lib_write": {"reg": 23, "value": lib_ctrl_after_reset(7, "PLLB")},  # 0x2C: MS7_SRC=1
        "mutation_fail_if": {
            "clk7_ctrl_equals": lib_ctrl_after_reset(7, "PLLA"),  # 0x0C would mean PLLA
            "pll_assignment_7": "PLLA",
        },
        "spec_note": ("AN619 §4.1.1 / Reg 23 MS7_SRC: 0=PLLA, 1=PLLB/VCXO — either legal when no "
                      "spread-spectrum/VCXO constraint. Library reset() chooses PLLB for CLK7; that "
                      "choice is the regression lock. Spec agrees the bit encoding; does not mandate PLLB."),
    })

    # M2: set_ms + ms_div byte+2 RMW bit preservation (prior bit7 set, R_DIV=128, P1[17:16]=2)
    c5 = simulate_set_freq_clk05(hz(5_000))
    prior44 = 0xFF  # leftover bits incl. unused bit7; would be wiped by a naive overwrite
    regs = lib_ms_regs(0, RegSet(c5["ms_p1"], c5["ms_p2"], c5["ms_p3"]), c5["r_div"], 0, prior44=prior44)
    transcript = lib_ms_byte2_rmw_transcript(0, RegSet(c5["ms_p1"], c5["ms_p2"], c5["ms_p3"]), c5["r_div"], 0, prior44)
    assert regs["44"] == 0xF2, hex(regs["44"])  # bit7|R=111|DIVBY4=00|P1hi=10
    assert transcript["final"] == 0xF2
    assert transcript["mutation_detectors"]["naive_ms_div_overwrite"] == 0x70  # loses P1hi+bit7
    assert transcript["mutation_detectors"]["naive_set_ms_then_correct_ms_div"] == 0x72  # loses bit7
    c5.update({
        "id": "M-set-ms-byte2-rmw", "priority": "M", "api": "set_ms_rmw",
        "covers": ["set_ms byte+2 RMW", "ms_div byte+2 RMW", "bit7 preserve", "P1[17:16] preserve"],
        "clk": 0, "pll_assignment": "PLLA",
        "clk_path": None,  # force set_ms_rmw evaluator (not CLK0-5 path)
        "prior_byte2": prior44,
        "lib_ms_regs": regs,
        "lib_clk_ctrl": {"16": lib_set_int(lib_ctrl_after_reset(0, "PLLA"), 0)},
        "lib_reg3_after": lib_output_enable(REG3_AFTER_RESET, 0, 1),
        "rmw_transcript": transcript,
        "wire_stub_must_assert": (
            "Future Wire stub: (1) set_ms bulk byte at reg 44 must equal (prior & ~0x03) | P1[17:16] "
            "— bits 7:2 survive; (2) ms_div write at reg 44 must equal that value with bits 6:2 replaced "
            "by R_DIV/DIVBY4 — bits 7 and 1:0 survive. Final byte MUST be 0xF2 for this case, NOT 0x70 or 0x72."
        ),
    })
    # also keep pll fields from simulate
    c5["lib_pll_regs"] = lib_pll_regs("PLLA", RegSet(c5["pll_p1"], c5["pll_p2"], c5["pll_p3"]))
    cases.append(c5)

    # M3: CLK5 → PLLA after reset (mutation: CLK5→PLLB)
    cases.append({
        "id": "M-clk5-reset-plla", "priority": "M", "api": "reset_pll_assignment",
        "covers": ["CLK5 → PLLA after reset", "reg 21 MS5_SRC bit5", "mutation: CLK5→PLLB"],
        "clk": 5,
        "pll_assignment": "PLLA",
        "pll_assignment_table": {str(i): ("PLLB" if i >= 6 else "PLLA") for i in range(8)},
        "lib_clk_ctrl": {str(16 + i): lib_ctrl_after_reset(i, "PLLB" if i >= 6 else "PLLA") for i in range(8)},
        "lib_write": {"reg": 21, "value": lib_ctrl_after_reset(5, "PLLA")},  # 0x0C: MS5_SRC=0
        "mutation_fail_if": {
            "clk5_ctrl_equals": lib_ctrl_after_reset(5, "PLLB"),  # 0x2C would mean PLLB
            "pll_assignment_5": "PLLB",
        },
        "spec_note": ("AN619 §4.1.1 / Reg 21 MS5_SRC: 0=PLLA, 1=PLLB. Library reset() chooses PLLA for "
                      "CLK0–5; that choice is the regression lock."),
    })

    # =========================================================================
    # A: automate the six APIs Coding PM's C++ harness did not cover
    # (happy-path companions to existing D-/S- deviation cases)
    # =========================================================================

    # A1 set_phase happy path — CLK1, non-trivial 7-bit value
    cases.append({
        "id": "A-set_phase-clk1-0x2A", "priority": "A", "api": "set_phase",
        "covers": ["set_phase", "reg 166"], "clk": 1,
        "requested_phase": 0x2A,
        "lib_write": {"reg": 165 + 1, "value": 0x2A & 0x7F},
        "sequence": ["reset()", "set_phase(SI5351_CLK1, 0x2A)"],
    })

    # A2 crystal load — already have S-xtal-load + D-0pF; add explicit init(8pF) single-write lock
    cases.append({
        "id": "A-init-crystal-load-8pF", "priority": "A", "api": "init_xtal_load",
        "covers": ["init xtal load", "reg 183"],
        "lib_writes": {"8pF": ((2 & 0x3) << 6) | 0x12},  # 0x92
        "sequence": ["init(SI5351_CRYSTAL_LOAD_8PF, 0, 0)"],
    })

    # A3 set_int happy path — CLK0 MS0_INT (not FBA_INT)
    cases.append({
        "id": "A-set_int-clk0-ms-int", "priority": "A", "api": "set_int",
        "covers": ["set_int CLK0", "reg 16 bit6 = MS0_INT"], "clk": 0,
        "sequence": ["reset()", "set_int(SI5351_CLK0, 1)"],
        "lib_write": {"reg": 16, "value": lib_set_int(lib_ctrl_after_reset(0, "PLLA"), 1)},  # 0x4C
        "ms_int_target": True,  # evaluator: this is MSx_INT not FBx_INT
        "plla": None,  # not used for MS_INT path
    })

    # A4 set_pll_input happy path — prior reg15 clear, select CLKIN /4
    cases.append({
        "id": "A-set_pll_input-clkin-div4-clean", "priority": "A", "api": "set_pll_input",
        "covers": ["set_pll_input", "set_ref_freq", "reg 15"],
        "sequence": ["reset()", "set_ref_freq(80000000, SI5351_PLL_INPUT_CLKIN)",  # -> clkin_div=/4
                     "set_pll_input(SI5351_PLLA, SI5351_PLL_INPUT_CLKIN)"],
        "prior_reg15": 0x00,
        "intended_clkin_div": 4,
        "lib_write": {"reg": 15, "value": 0x00 | 0x04 | (2 << 6)},  # PLLA_SRC | CLKIN_DIV_4 = 0x84
    })

    # A5 5 MHz-ref — already D-ref5MHz; add companion 27 MHz in-spec ref (adjacent set_ref_freq/set_pll)
    reg27, _, abc27 = pll_calc(SI5351_PLL_FIXED, ref_xtal_hz=27_000_000)
    cases.append({
        "id": "A-ref27MHz-set_pll-fixed", "priority": "A", "api": "set_pll",
        "covers": ["set_ref_freq 27 MHz", "set_pll", "in-spec XTAL"],
        "sequence": ["init(SI5351_CRYSTAL_LOAD_8PF, 27000000, 0)", "reset() → set_pll(SI5351_PLL_FIXED)"],
        "pll_freq_lib": SI5351_PLL_FIXED, "xtal_hz": 27_000_000, "correction_ppb": 0,
        "pll_assignment": "PLLA",
        "pll_a": abc27["a"], "pll_b": abc27["b"], "pll_c": abc27["c"],
        "pll_p1": reg27.p1, "pll_p2": reg27.p2, "pll_p3": reg27.p3,
        "lib_pll_regs": lib_pll_regs("PLLA", reg27),
    })

    # A6 adjacent: set_correction nonzero then set_int CLK0 (pairs crystal-cal with MS_INT API)
    reg_corr, _, abc_corr = pll_calc(SI5351_PLL_FIXED, correction=100_000)
    cases.append({
        "id": "A-set_correction-then-set_int-clk0", "priority": "A", "api": "set_int",
        "covers": ["set_correction", "set_int CLK0"], "clk": 0,
        "sequence": ["reset()", "set_correction(100000, SI5351_PLL_INPUT_XO)", "set_int(SI5351_CLK0, 1)"],
        "lib_write": {"reg": 16, "value": lib_set_int(lib_ctrl_after_reset(0, "PLLA"), 1)},
        "ms_int_target": True,
        "plla": {"a": abc_corr["a"], "b": abc_corr["b"], "c": abc_corr["c"],
                 "p1": reg_corr.p1, "p2": reg_corr.p2, "p3": reg_corr.p3},
        "lib_pll_regs": lib_pll_regs("PLLA", reg_corr),
        "pll_a": abc_corr["a"], "pll_b": abc_corr["b"], "pll_c": abc_corr["c"],
        "pll_p1": reg_corr.p1, "pll_p2": reg_corr.p2, "pll_p3": reg_corr.p3,
        "xtal_hz": SI5351_XTAL_FREQ, "correction_ppb": 100_000,
    })


    return cases


def main():
    # JSON/markdown generation lives in generate_goldens.py so this mirror
    # module never imports the spec oracle.
    import generate_goldens
    generate_goldens.main()


if __name__ == "__main__":
    main()
