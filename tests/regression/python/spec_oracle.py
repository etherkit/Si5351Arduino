#!/usr/bin/env python3
"""
INDEPENDENT spec oracle for the Si5351, written from:
  * AN619 Rev 0.8 (Sep 23 2021) "Manually Generating an Si5351 Register Map
    for 10-MSOP and 20-QFN Devices"            -> cited as AN619 §x / Reg n
  * Si5351A/B/C-B datasheet Rev 1.3 (Aug 27 2021) -> cited as DS Table n / §x

This module deliberately does NOT import si5351_math.py (the library mirror)
and does not reuse any of its code. It works in exact rational arithmetic
(fractions.Fraction) in plain Hz, not the library's Hz*100 units.
"""
from __future__ import annotations
from fractions import Fraction as F
from typing import Dict, List, Optional, Tuple

# ---------------------------------------------------------------------------
# Spec limits (with citations)
# ---------------------------------------------------------------------------
VCO_MIN_HZ = 600_000_000          # AN619 §2 "VCO ... range of 600 to 900 MHz"
VCO_MAX_HZ = 900_000_000          # AN619 §2 (datasheet does not state VCO range)
FB_RATIO_MIN = F(15)              # AN619 §3.2 "valid range of 15 + 0/1,048,575 and 90"
FB_RATIO_MAX = F(90)
C_MAX = 1_048_575                 # AN619 §2.1.1 / §3.2 / §4.1.2 (20-bit P3 = c)
P1_BITS, P2_BITS, P3_BITS = 18, 20, 20   # AN619 §3.2 / §4.1.2 MSx_P1[17:0], P2[19:0], P3[19:0]
MS_INT_RATIOS = {4, 6, 8}         # AN619 §2.1.1 item 1
MS_FRAC_MIN = 8 + F(1, C_MAX)     # AN619 §2.1.1 item 1, §4.1.2
MS_RATIO_MAX = F(2048)            # AN619 §2.1.1 item 1, §4.1.2
MS67_MIN, MS67_MAX = 6, 254       # AN619 §2.2 Note, §4.1.2, Reg 90/91 (even integers only)
MS_OUT_MIN_HZ = 500_000           # AN619 §2 "Multisynth ... between 500 kHz and 200 MHz"
MS_OUT_MAX_HZ = 200_000_000       # AN619 §2, §4.1.3; DS Table 7
DIVBY4_ABOVE_HZ = 150_000_000     # AN619 §4.1.3 "150 MHz < Fout <= 200 MHz" -> divide by 4
CLKOUT_MIN_HZ = 2_500             # DS Table 7 FCLK min 0.0025 MHz; AN619 §2 "down to 2.5 kHz"
CLKOUT_MAX_HZ = 200_000_000       # DS Table 7 FCLK max 200 MHz
R_VALUES = [1, 2, 4, 8, 16, 32, 64, 128]  # AN619 §4.2.2, Reg 44 R0_DIV / Reg 92
UNIQUE_ABOVE_HZ = 112_500_000     # DS Table 7 note 1 / §4.2: only two unique freqs > 112.5 MHz
VCXO_APR_MIN, VCXO_APR_MAX = 30, 240   # DS Table 5 VCXO pull range ±30..±240 ppm
VCXO_C = 1_000_000                # AN619 §3.2 & §7: MSNB c "fixed to 10^6" for VCXO
VCXO_MARGIN = F(103, 100)         # AN619 §7 "1.03 is a margining factor"
VCXO_CENTER_MAX_HZ = 112_500_000  # DS §4.2 "VCXO output's center frequency up to 112.5 MHz"
XTAL_MIN_HZ, XTAL_MAX_HZ = 25_000_000, 27_000_000   # DS Table 6 / Table 8
PLL_IN_MIN_HZ, PLL_IN_MAX_HZ = 10_000_000, 40_000_000  # AN619 §3.1.2 (DS §4.1 says CLKIN div limits to 30 MHz)
PHOFF_BITS = 7                    # AN619 Reg 165-170 CLKx_PHOFF[6:0], bit 7 "Only write 0"

# Register addresses (AN619 §8.1 Register Map Summary)
REG_OEB = 3
REG_PLL_INPUT_SRC = 15
REG_CLK_CTRL_BASE = 16            # regs 16..23
REG_MSNA_BASE = 26                # 26..33
REG_MSNB_BASE = 34                # 34..41
REG_MS0_BASE = 42                 # MSn base = 42 + 8n for n=0..5 (42,50,58,66,74,82)
REG_MS6_P1 = 90
REG_MS7_P1 = 91
REG_R67 = 92
REG_VCXO = (162, 163, 164)
REG_PHOFF_BASE = 165              # 165..170 for CLK0..5 only
REG_PLL_RESET = 177
REG_XTAL_CL = 183

# ---------------------------------------------------------------------------
# Encoders / decoders
# ---------------------------------------------------------------------------

def encode_abc(a: int, b: int, c: int) -> Tuple[int, int, int]:
    """AN619 §3.2 (feedback) and §4.1.2 (MS0-5):
       P1 = 128a + floor(128 b/c) - 512 ; P2 = 128b - c*floor(128 b/c) ; P3 = c"""
    if c < 1:
        raise ValueError("c must be >= 1")
    fl = (128 * b) // c
    return 128 * a + fl - 512, 128 * b - c * fl, c


def decode_p(p1: int, p2: int, p3: int) -> F:
    """Invert AN619 encoding: a + b/c = (P1 + 512 + P2/P3) / 128."""
    if p3 == 0:
        raise ValueError("P3 == 0 is not a valid encoding")
    return (F(p1 + 512) + F(p2, p3)) / 128


def r_code(r: int) -> int:
    """AN619 Reg 44 R0_DIV[2:0]: 000b=/1 ... 111b=/128"""
    return R_VALUES.index(r)


def _param_block(base: int, p1: int, p2: int, p3: int, reg2_hi: int = 0) -> Dict[int, int]:
    """8-byte parameter block layout, AN619 §8.1 / Reg 26-33, 42-49:
       +0 P3[15:8]  +1 P3[7:0]  +2 {hi bits | P1[17:16]}  +3 P1[15:8]  +4 P1[7:0]
       +5 P3[19:16]<<4 | P2[19:16]  +6 P2[15:8]  +7 P2[7:0]"""
    return {
        base + 0: (p3 >> 8) & 0xFF,
        base + 1: p3 & 0xFF,
        base + 2: (reg2_hi & 0xFC) | ((p1 >> 16) & 0x3),
        base + 3: (p1 >> 8) & 0xFF,
        base + 4: p1 & 0xFF,
        base + 5: (((p3 >> 16) & 0xF) << 4) | ((p2 >> 16) & 0xF),
        base + 6: (p2 >> 8) & 0xFF,
        base + 7: p2 & 0xFF,
    }


def feedback_block(pll: str, p1: int, p2: int, p3: int) -> Dict[int, int]:
    """MSNA regs 26-33 / MSNB regs 34-41. Reg 28/36 bits 3:2 Reserved 'Leave as default, 0', 7:4 unused."""
    base = REG_MSNA_BASE if pll == "PLLA" else REG_MSNB_BASE
    return _param_block(base, p1, p2, p3, 0)


def ms_block(n: int, p1: int, p2: int, p3: int, r: int = 1, divby4: bool = False) -> Dict[int, int]:
    """MS0-5 block at 42+8n. Reg 44 (+2): bit7 unused, R_DIV[2:0] bits 6:4,
       MSx_DIVBY4[1:0] bits 3:2 (11b enable / 00b otherwise), P1[17:16] bits 1:0."""
    if not 0 <= n <= 5:
        raise ValueError("ms_block only for MS0..MS5")
    hi = (r_code(r) << 4) | ((0b11 << 2) if divby4 else 0)
    return _param_block(REG_MS0_BASE + 8 * n, p1, p2, p3, hi)


def divby4_params() -> Tuple[int, int, int]:
    """AN619 §4.1.3: MSx_P1=0, MSx_P2=0, MSx_P3=1, MSx_INT=1, MSx_DIVBY4=11b"""
    return 0, 0, 1


def ms67_reg(n: int, ratio: int) -> Dict[int, int]:
    """AN619 §4.1.2 / Reg 90, 91: MSx_P1 = divide value (even integer 6..254)."""
    return {REG_MS6_P1 + (n - 6): ratio & 0xFF}


def reg92(r6: int = 1, r7: int = 1) -> int:
    """AN619 Reg 92 (summary map): bit7 rsvd, R7_DIV[2:0] bits 6:4, bit3 rsvd, R6_DIV[2:0] bits 2:0."""
    return (r_code(r7) << 4) | r_code(r6)


def clk_ctrl(clk: int, pdn=0, int_bit=0, src_pllb=0, inv=0, clk_src=0b11, idrv=0) -> int:
    """AN619 Reg 16-23: D7 PDN, D6 MSx_INT (CLK0-5) / FBA_INT (reg22) / FBB_INT (reg23),
       D5 MSx_SRC (0=PLLA, 1=PLLB/VCXO), D4 INV, D3:2 CLK_SRC (00 XTAL, 01 CLKIN,
       10 MS0/MS4 (reserved for CLK0), 11 own MS), D1:0 IDRV (00 2mA .. 11 8mA)."""
    return (pdn << 7) | (int_bit << 6) | (src_pllb << 5) | (inv << 4) | (clk_src << 2) | idrv


def pll_reset_byte(plla: bool, pllb: bool) -> int:
    """AN619 Reg 177: bit7 PLLB_RST, bit5 PLLA_RST, others reserved/default 0."""
    return (0x80 if pllb else 0) | (0x20 if plla else 0)


def xtal_cl_byte(cl_code: int) -> int:
    """AN619 Reg 183: XTAL_CL[1:0] bits 7:6 (00 reserved, 01 6pF, 10 8pF, 11 10pF),
       bits 5:0 'should be written to 010010b'."""
    return ((cl_code & 0x3) << 6) | 0b010010


def phase_reg(clk: int, value: int) -> Tuple[int, int]:
    """AN619 Reg 165-170 (CLK0..5 only); bit7 'Only write 0'."""
    if not 0 <= clk <= 5:
        raise ValueError("phase offset only exists for CLK0-5")
    return REG_PHOFF_BASE + clk, value & 0x7F


def phase_word(offset_s: F, fvco_hz: F) -> int:
    """AN619 §6: CLKx_PHOFF = Round(DesiredOffset[s] * 4 * Fvco)."""
    x = offset_s * 4 * fvco_hz
    return int(x + F(1, 2)) if x >= 0 else -int(-x + F(1, 2))


def vcxo_param(a: int, b: int, apr: int) -> Dict[str, object]:
    """AN619 §7: VCXO_Param[21:0] = 1.03 * (128a + b/10^6) * APR, with MSNB c = 10^6.
       AN619 does not state a rounding rule; we report exact, floor and round."""
    exact = VCXO_MARGIN * (128 * a + F(b, 10**6)) * apr
    fl = exact.numerator // exact.denominator
    rd = int(exact + F(1, 2))
    return {"exact": exact, "floor": fl, "round": rd,
            "regs_floor": {162: fl & 0xFF, 163: (fl >> 8) & 0xFF, 164: (fl >> 16) & 0x3F},
            "fits_22bit": fl < (1 << 22)}


# ---------------------------------------------------------------------------
# Constraint checks. Each returns a dict: rule, ok, severity, cite, detail
# severity: "error" (spec violation) | "advisory" (recommendation, never fails tests)
# ---------------------------------------------------------------------------

def chk(rule: str, ok: bool, cite: str, detail: str, severity: str = "error") -> Dict[str, object]:
    return {"rule": rule, "ok": bool(ok), "severity": severity, "cite": cite, "detail": detail}


def check_abc_fields(prefix: str, p1: int, p2: int, p3: int) -> List[Dict]:
    out = [chk(f"{prefix}_P_WIDTH", p1 < (1 << P1_BITS) and p2 < (1 << P2_BITS) and 1 <= p3 < (1 << P3_BITS),
               "AN619 §3.2/§4.1.2 P1[17:0], P2[19:0], P3[19:0]",
               f"P1={p1} P2={p2} P3={p3}"),
           chk(f"{prefix}_C_MAX", 1 <= p3 <= C_MAX, "AN619 §3.2/§4.1.2 (c <= 1,048,575)", f"c=P3={p3}")]
    return out


def check_feedback(p1: int, p2: int, p3: int, fref_hz: F) -> Tuple[List[Dict], F, F]:
    ratio = decode_p(p1, p2, p3)
    vco = fref_hz * ratio
    out = check_abc_fields("FB", p1, p2, p3)
    out.append(chk("FB_RATIO_RANGE", FB_RATIO_MIN <= ratio <= FB_RATIO_MAX, "AN619 §3.2 (15 .. 90)",
                   f"a+b/c={float(ratio):.9f}"))
    out.append(chk("VCO_RANGE", VCO_MIN_HZ <= vco <= VCO_MAX_HZ, "AN619 §2 (600-900 MHz)",
                   f"fVCO={float(vco):.3f} Hz"))
    is_even_int = ratio.denominator == 1 and ratio.numerator % 2 == 0
    if is_even_int:
        out.append(chk("FB_INT_ADVISORY", False, "AN619 §3.2.1 / Reg 22-23 bit6 FBx_INT",
                       f"feedback ratio {ratio} is an even integer; FBx_INT=1 'in most cases ... improve jitter' "
                       "(optional; library never sets FBx_INT)", "advisory"))
    return out, ratio, vco


def check_ms05(p1: int, p2: int, p3: int, divby4: bool, ms_int: bool, f_ms_hz: Optional[F]) -> Tuple[List[Dict], F]:
    out: List[Dict] = []
    if divby4:
        out.append(chk("DIVBY4_PARAMS", (p1, p2, p3) == divby4_params(), "AN619 §4.1.3 (P1=0,P2=0,P3=1)",
                       f"P=({p1},{p2},{p3})"))
        out.append(chk("DIVBY4_MS_INT", ms_int, "AN619 §4.1.3 (MSx_INT=1)", f"MS_INT={int(ms_int)}"))
        ratio = F(4)
    else:
        out += check_abc_fields("MS", p1, p2, p3)
        ratio = decode_p(p1, p2, p3)
        valid = (ratio.denominator == 1 and ratio.numerator in MS_INT_RATIOS) or (MS_FRAC_MIN <= ratio <= MS_RATIO_MAX)
        out.append(chk("MS_RATIO_VALID", valid, "AN619 §2.1.1 item 1 / §4.1.2 (4, 6, 8, or 8+1/1048575 .. 2048)",
                       f"MS ratio={float(ratio):.9f}" + (f" (={ratio})" if ratio.denominator == 1 else "")))
        if ratio == 4:
            out.append(chk("RATIO4_NEEDS_DIVBY4", False, "AN619 §4.1.3", "ratio 4 requires DIVBY4=11b encoding"))
        is_even_int = ratio.denominator == 1 and ratio.numerator % 2 == 0
        if ms_int:
            out.append(chk("MS_INT_ONLY_EVEN_INT", is_even_int, "AN619 §4.1.2.1 / Reg 16-21 bit6",
                           f"MS_INT=1 with ratio {float(ratio)}"))
        elif is_even_int:
            out.append(chk("MS_INT_ADVISORY", False, "AN619 §2.1.1 item 2, §4.1.2.1",
                           f"MS ratio {ratio} is an even integer; MS_INT=1 'in most cases ... improve jitter' (optional)",
                           "advisory"))
    if f_ms_hz is not None:
        out.append(chk("MS_OUT_RANGE", MS_OUT_MIN_HZ <= f_ms_hz <= MS_OUT_MAX_HZ, "AN619 §2 (MS output 500 kHz - 200 MHz)",
                       f"f_MS={float(f_ms_hz):.3f} Hz"))
        if f_ms_hz > DIVBY4_ABOVE_HZ:
            out.append(chk("DIVBY4_REQUIRED", divby4, "AN619 §4.1.3 (150 < Fout <= 200 MHz needs DIVBY4)",
                           f"f_MS={float(f_ms_hz):.3f} Hz divby4={int(divby4)}"))
    return out, ratio


def check_ms67(ratio_reg: int) -> List[Dict]:
    return [chk("MS67_RATIO_VALID", ratio_reg % 2 == 0 and MS67_MIN <= ratio_reg <= MS67_MAX,
                "AN619 §2.2 Note, §4.1.2, Reg 90/91 (even integer 6..254)", f"MSx_P1={ratio_reg}")]


def check_out_freq(requested_hz: F, realized_hz: Optional[F], tol_ppb: int = 1000) -> List[Dict]:
    out = [chk("OUT_FREQ_RANGE", CLKOUT_MIN_HZ <= requested_hz <= CLKOUT_MAX_HZ,
               "DS Table 7 FCLK 2.5 kHz - 200 MHz", f"requested={float(requested_hz):.3f} Hz")]
    if realized_hz is not None:
        err_ppb = (realized_hz - requested_hz) / requested_hz * 10**9
        out.append(chk("OUT_FREQ_ACCURACY", abs(err_ppb) <= tol_ppb,
                       f"programmed registers must realize requested freq (tol {tol_ppb} ppb)",
                       f"realized={float(realized_hz):.6f} Hz err={float(err_ppb):.3f} ppb"))
    return out
