/* R-divider band edges (issue #89) and AN619 Register 183 XTAL_CL.
 * Compile with -Dprivate=public so select_r_div() can be called. */
#include "si5351.h"
#include "sim.h"
#include <stdio.h>

static int fails = 0;
#define CHECK(c, msg) do { \
	int _ok = (c); \
	printf("%s  %s\n", _ok ? "PASS" : "FAIL", msg); \
	if (!_ok) fails++; \
} while (0)

/* One octave per R code. Low edge inclusive, high edge exclusive.
 * Code n means divide by 2^n (AN619 R0_DIV). */
static void check_octaves(Si5351 &si, uint32_t min_hz, const char *tag)
{
	uint32_t seen = 0;
	int shift;

	for (shift = 7; shift >= 1; shift--)
	{
		uint64_t lo_hz = (uint64_t)min_hz << (7 - shift);
		uint64_t hi_hz = lo_hz << 1;
		uint32_t mul = 1u << shift;
		uint64_t f;
		uint8_t r;
		char buf[96];

		f = lo_hz * SI5351_FREQ_MULT;
		r = si.select_r_div(&f, min_hz);
		snprintf(buf, sizeof buf, "%s /%u low %llu Hz", tag, mul, (unsigned long long)lo_hz);
		CHECK(r == (uint8_t)shift && f == lo_hz * SI5351_FREQ_MULT * mul, buf);
		/* Bottom of every divided band is min_hz * 128, which for the
		 * 4 kHz CLK0-5 floor is 512 kHz (>= AN619's 500 kHz MS minimum). */
		if (min_hz == SI5351_CLKOUT_MIN_FREQ)
		{
			CHECK(f >= 500000ULL * SI5351_FREQ_MULT, "multisynth at/above 500 kHz");
		}

		f = hi_hz * SI5351_FREQ_MULT - 1;
		r = si.select_r_div(&f, min_hz);
		snprintf(buf, sizeof buf, "%s /%u just below %llu Hz", tag, mul, (unsigned long long)hi_hz);
		CHECK(r == (uint8_t)shift && f == (hi_hz * SI5351_FREQ_MULT - 1) * mul, buf);

		/* The next hertz belongs to the following divider, so two adjacent
		 * bands never share a code (the #89 divide-by-64 report). */
		f = hi_hz * SI5351_FREQ_MULT;
		r = si.select_r_div(&f, min_hz);
		snprintf(buf, sizeof buf, "%s boundary %llu Hz is not /%u", tag,
			(unsigned long long)hi_hz, mul);
		CHECK(r != (uint8_t)shift, buf);

		seen |= (1u << shift);
	}

	{
		uint64_t start_hz = (uint64_t)min_hz << 7;
		uint64_t f = start_hz * SI5351_FREQ_MULT;
		uint8_t r = si.select_r_div(&f, min_hz);
		char buf[96];
		snprintf(buf, sizeof buf, "%s /1 from %llu Hz", tag, (unsigned long long)start_hz);
		CHECK(r == SI5351_OUTPUT_CLK_DIV_1 && f == start_hz * SI5351_FREQ_MULT, buf);
		seen |= 1u;
	}

	CHECK(seen == 0xFFu, "eight distinct R codes (1,2,4,8,16,32,64,128)");
}

static void check_xtal(uint8_t load, uint8_t expect, const char *msg)
{
	g_sim.reset(0, true);
	Si5351 si;
	bool ok = si.init(load, 0, 0);
	CHECK(ok, "init found the device");
	CHECK(g_sim.regs[SI5351_CRYSTAL_LOAD] == expect, msg);
	CHECK((g_sim.regs[SI5351_CRYSTAL_LOAD] >> 6) != 0, "XTAL_CL is not reserved 00b");
	CHECK((g_sim.regs[SI5351_CRYSTAL_LOAD] & 0x3F) == 0x12, "reg 183 bits 5:0 are 010010b");
}

int main(void)
{
	Si5351 si;

	check_octaves(si, SI5351_CLKOUT_MIN_FREQ, "CLK0-5");
	check_octaves(si, SI5351_CLKOUT67_MIN_FREQ, "CLK6-7");

	/* Below the floor, select_r_div leaves the frequency alone (R=1).
	 * set_freq() clamps before it calls this. */
	{
		uint64_t f = (uint64_t)(SI5351_CLKOUT_MIN_FREQ - 1) * SI5351_FREQ_MULT;
		uint64_t orig = f;
		uint8_t r = si.select_r_div(&f, SI5351_CLKOUT_MIN_FREQ);
		CHECK(r == SI5351_OUTPUT_CLK_DIV_1 && f == orig, "below 4 kHz: select_r_div does not scale");
	}

	/* 6 pF = 0x40 | 0x12, 8 pF = 0x80 | 0x12, 10 pF and the deprecated
	 * 0 pF code = 0xC0 | 0x12. AN619 Register 183. */
	check_xtal(SI5351_CRYSTAL_LOAD_6PF, 0x52, "reg183 6 pF is 0x52");
	check_xtal(SI5351_CRYSTAL_LOAD_8PF, 0x92, "reg183 8 pF is 0x92");
	check_xtal(SI5351_CRYSTAL_LOAD_10PF, 0xD2, "reg183 10 pF is 0xD2");
	check_xtal(SI5351_CRYSTAL_LOAD_0PF, 0xD2, "reg183 reserved 0 pF keeps 10 pF (0xD2)");
	CHECK(SI5351_CRYSTAL_LOAD_0PF == (0 << 6), "0 pF macro is still the reserved code");

	/* Programmed divider in register 44 bits 6:4. 8 kHz is /64, 16 kHz is /32. */
	g_sim.reset(0, true);
	CHECK(si.init(SI5351_CRYSTAL_LOAD_8PF, 0, 0), "init before set_freq");
	CHECK(si.set_freq(8000ULL * SI5351_FREQ_MULT, SI5351_CLK0) == 0, "set_freq 8 kHz");
	CHECK(((g_sim.regs[SI5351_CLK0_PARAMETERS + 2] >> 4) & 7) == SI5351_OUTPUT_CLK_DIV_64,
		"reg44 R = /64 at 8 kHz");
	CHECK(si.set_freq(16000ULL * SI5351_FREQ_MULT, SI5351_CLK0) == 0, "set_freq 16 kHz");
	CHECK(((g_sim.regs[SI5351_CLK0_PARAMETERS + 2] >> 4) & 7) == SI5351_OUTPUT_CLK_DIV_32,
		"reg44 R = /32 at 16 kHz");
	CHECK(si.set_freq(4000ULL * SI5351_FREQ_MULT, SI5351_CLK0) == 0, "set_freq 4 kHz");
	CHECK(((g_sim.regs[SI5351_CLK0_PARAMETERS + 2] >> 4) & 7) == SI5351_OUTPUT_CLK_DIV_128,
		"reg44 R = /128 at 4 kHz");
	CHECK(si.set_freq(1000ULL * SI5351_FREQ_MULT, SI5351_CLK0) == 0, "set_freq 1 kHz clamps");
	CHECK(si.clk_freq[0] == 4000ULL * SI5351_FREQ_MULT, "1 kHz request stored as 4 kHz");

	/* v2.4.1 CLK0-5 cap. The 225 MHz macros are not what set_freq programs. */
	CHECK(si.set_freq(225000000ULL * SI5351_FREQ_MULT, SI5351_CLK0) == 0, "set_freq 225 MHz");
	CHECK(si.clk_freq[0] == 200000000ULL * SI5351_FREQ_MULT, "225 MHz stored as 200 MHz");
	CHECK(SI5351_CLKOUT_MAX_FREQ == 225000000UL, "compatibility macro still says 225 MHz");

	/* PHOFF[6:0]: 127 fits, 128 does not. 600 MHz / 127 is the 90 degree floor. */
	si.set_phase(SI5351_CLK0, 255);
	CHECK(g_sim.regs[SI5351_CLK0_PHASE_OFFSET] == 127, "set_phase masks to 127");
	si.set_phase(SI5351_CLK0, 128);
	CHECK(g_sim.regs[SI5351_CLK0_PHASE_OFFSET] == 0, "phase word 128 does not fit in bits 6:0");
	CHECK(SI5351_PLL_VCO_MIN / 127 == 4724409UL, "600 MHz / 127 = 4724409 Hz");

	printf("%s (%d failures)\n", fails ? "FAILED" : "ALL PASS", fails);
	return fails ? 1 : 0;
}
