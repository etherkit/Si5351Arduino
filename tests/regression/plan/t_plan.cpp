/* Branch locks for v2.4.2 planning code. Behavior is snapshotted as it is.
 *
 * Unreachable without an out-of-range enum cast (CWG 1766), left uncovered
 * on purpose:
 *   drive_strength default, set_clock_source default, set_pll_input default,
 *   set_clock_disable's clk>CLK7 return, set_ms's clk>CLK7 else,
 *   pll_reset / set_ms_source "neither PLL" side, fanout switch default.
 * ms67_div's final `return 0` (ratio still >254 after eight even halvings)
 * is reached through set_freq_manual, which is public.
 * multisynth67_calc's a+=2 path is not reached by set_freq(): that path
 * pre-scales the multisynth up to ceil(600 MHz/254) first. The direct call
 * is compiled with -Dprivate=public, same as the other host tests, and does
 * not change the library. It currently stores a=256 in a uint8_t (p1 becomes
 * 0). That is locked and flagged, not "fixed" here.
 */
#include "si5351.h"
#include "sim.h"
#include <stdio.h>
#include <stdint.h>

static int fails = 0;
#define CHECK(c, msg) do { \
	int _ok = (c); \
	printf("%s  %s\n", _ok ? "PASS" : "FAIL", msg); \
	if (!_ok) fails++; \
} while (0)

static Si5351 *boot(void)
{
	static Si5351 obj;
	g_sim.reset(0, true);
	obj.~Si5351();
	Si5351 *s = new (&obj) Si5351();
	CHECK(s->init(SI5351_CRYSTAL_LOAD_8PF, 0, 0), "init");
	return s;
}

static int reg_changed(unsigned long writes_before)
{
	return g_sim.n_writes != writes_before;
}

int main(void)
{
	/* CLK6/7 manual rejects: no MS block write. */
	{
		Si5351 *si = boot();
		unsigned long w = g_sim.n_writes;
		uint8_t r = si->set_freq_manual(1234567800ULL, 80000000000ULL, SI5351_CLK6);
		CHECK(r == 1 && !reg_changed(w), "manual CLK6 non-integer PLL returns 1, no write");
	}
	{
		Si5351 *si = boot();
		unsigned long w = g_sim.n_writes;
		/* 600 MHz / 150 MHz = 4, even but below 6. */
		uint8_t r = si->set_freq_manual(15000000000ULL, 60000000000ULL, SI5351_CLK7);
		CHECK(r == 1 && !reg_changed(w), "manual CLK7 ratio 4 returns 1, no write");
	}
	{
		Si5351 *si = boot();
		unsigned long w = g_sim.n_writes;
		/* 510 MHz / 2 MHz = 255, odd and above 254. */
		uint8_t r = si->set_freq_manual(200000000ULL, 51000000000ULL, SI5351_CLK6);
		CHECK(r == 1 && !reg_changed(w), "manual CLK6 ratio 255 returns 1, no write");
	}
	{
		Si5351 *si = boot();
		unsigned long w = g_sim.n_writes;
		/* 65536 Hz * 65536 = 2^32 Hz worth of ratio steps: eight even
		 * halvings still leave n>254, ms67_div returns 0. */
		uint8_t r = si->set_freq_manual(6553600ULL, 65536ULL * 6553600ULL, SI5351_CLK6);
		CHECK(r == 1 && !reg_changed(w), "manual CLK6 ratio 65536 returns 1, no write");
	}

	/* Public set_freq hits multisynth67_calc's a-=2 adjustment.
	 * 4.6 MHz -> even a=196, product 901.6 MHz, then a-=2 -> 194. */
	{
		Si5351 *si = boot();
		uint8_t r = si->set_freq(460000000ULL, SI5351_CLK6);
		CHECK(r == 0, "set_freq 4.6 MHz CLK6");
		CHECK(g_sim.regs[SI5351_CLK6_PARAMETERS] == 194, "CLK6 MS ratio 194 after a-=2");
		CHECK(si->pllb_freq == 194ULL * 460000000ULL, "PLLB is 194 * 4.6 MHz");
	}

	/* Direct a+=2. Locked current behavior: a becomes 256 and the uint8_t
	 * parameter register is 0. A refactor that stops the truncation must
	 * change this assertion on purpose. */
	{
		Si5351RegSet reg = {0, 0, 0};
		uint64_t freq = 500000ULL * SI5351_FREQ_MULT;
		Si5351 *si = boot();
		uint64_t got = si->multisynth67_calc(freq, 0, &reg);
		CHECK(reg.p1 == 0 && reg.p2 == 0 && reg.p3 == 0, "LOCKED: a+=2 to 256 truncates p1 to 0");
		CHECK(got == 256ULL * freq, "LOCKED: returned VCO uses untruncated a=256");
		printf("INFO  multisynth67_calc a+=2 stores p1 in uint8_t; 256 becomes 0. Not changed.\n");
	}

	/* set_vcxo ppm clamp and the ref_ok reject. */
	{
		Si5351 *si = boot();
		si->set_vcxo(80000000000ULL, 10);
		uint8_t low10 = g_sim.regs[SI5351_VXCO_PARAMETERS_LOW];
		uint8_t mid10 = g_sim.regs[SI5351_VXCO_PARAMETERS_MID];
		uint8_t hi10 = g_sim.regs[SI5351_VXCO_PARAMETERS_HIGH];
		g_sim.reset(0, true);
		si = boot();
		si->set_vcxo(80000000000ULL, 30);
		CHECK(g_sim.regs[SI5351_VXCO_PARAMETERS_LOW] == low10
		      && g_sim.regs[SI5351_VXCO_PARAMETERS_MID] == mid10
		      && g_sim.regs[SI5351_VXCO_PARAMETERS_HIGH] == hi10,
		      "set_vcxo 10 ppm clamps to the 30 ppm bytes");
		si->set_vcxo(80000000000ULL, 255);
		CHECK(g_sim.regs[SI5351_VXCO_PARAMETERS_LOW] != low10
		      || g_sim.regs[SI5351_VXCO_PARAMETERS_MID] != mid10
		      || g_sim.regs[SI5351_VXCO_PARAMETERS_HIGH] != hi10,
		      "set_vcxo 255 ppm clamps to 240 ppm and differs from 30");
		unsigned long w = g_sim.n_writes;
		uint8_t pull = si->vcxo_pull;
		si->set_ref_freq(5000000, SI5351_PLL_INPUT_XO);
		si->set_vcxo(80000000000ULL, 60);
		CHECK(g_sim.n_writes == w && si->vcxo_pull == pull,
		      "set_vcxo with a 5 MHz reference writes nothing");
	}

	/* pll_calc clamps the divider math to 600..900 MHz, but pll_update stores
	 * the requested frequency anyway (v2.4.2). Locked, not corrected. */
	{
		Si5351 *si = boot();
		si->set_pll(10000000000ULL, SI5351_PLLA); /* 100 MHz request */
		CHECK(si->plla_freq == 10000000000ULL, "LOCKED: set_pll stores the 100 MHz request");
		CHECK(g_sim.regs[29] == 0x0A && g_sim.regs[30] == 0x00,
		      "LOCKED: PLLA registers are the clamped 600 MHz ratio (a=24)");
		si->set_pll(200000000000ULL, SI5351_PLLB); /* 2 GHz request */
		CHECK(si->pllb_freq == 200000000000ULL, "LOCKED: set_pll stores the 2 GHz request");
		CHECK(g_sim.regs[37] == 0x10 && g_sim.regs[38] == 0x00,
		      "LOCKED: PLLB registers are the clamped 900 MHz ratio (a=36)");
		printf("INFO  set_pll stores an out-of-range request while pll_calc clamps the registers.\n");
	}

	/* Both PLLs occupied above the share limit: second >100 MHz CLK is refused. */
	{
		Si5351 *si = boot();
		CHECK(si->set_freq(1000000000ULL, SI5351_CLK6) == 0, "CLK6 holds PLLB");
		CHECK(si->set_freq(12000000000ULL, SI5351_CLK0) == 0, "CLK0 120 MHz holds PLLA");
		uint64_t f1 = si->clk_freq[1];
		unsigned long w = g_sim.n_writes;
		uint8_t r = si->set_freq(11000000000ULL, SI5351_CLK1);
		CHECK(r == 1 && si->clk_freq[1] == f1 && g_sim.n_writes == w,
		      "CLK1 110 MHz refused while both PLLs are held");
	}

	/* 160 MHz then 80 MHz leaves DIVBY4 and resets the PLL (reg 177). */
	{
		Si5351 *si = boot();
		CHECK(si->set_freq(16000000000ULL, SI5351_CLK0) == 0, "CLK0 160 MHz");
		g_sim.regs[SI5351_PLL_RESET] = 0;
		CHECK(si->set_freq(8000000000ULL, SI5351_CLK0) == 0, "CLK0 80 MHz leaves DIVBY4");
		CHECK(g_sim.regs[SI5351_PLL_RESET] == SI5351_PLL_RESET_A,
		      "leaving DIVBY4 writes PLLA reset");
	}

	/* set_ms on CLK6/CLK7 writes only that clock's one-byte ratio, and ms_div
	 * updates only that clock's field in register 92. */
	{
		Si5351 *si = boot();
		Si5351RegSet ms = {42, 0, 0};
		g_sim.regs[90] = 0x11;
		g_sim.regs[91] = 0x22;
		g_sim.regs[92] = 0xFF;
		si->set_ms(SI5351_CLK6, ms, 0, 3, 0);
		CHECK(g_sim.regs[90] == 42 && g_sim.regs[91] == 0x22, "set_ms CLK6 writes reg 90 only");
		CHECK(g_sim.regs[92] == (uint8_t)((0xFF & ~0x07) | 3), "set_ms CLK6 R-div keeps CLK7 bits");
		g_sim.regs[90] = 0x11;
		g_sim.regs[92] = 0xFF;
		ms.p1 = 40;
		si->set_ms(SI5351_CLK7, ms, 0, 5, 0);
		CHECK(g_sim.regs[91] == 40 && g_sim.regs[90] == 0x11, "set_ms CLK7 writes reg 91 only");
		CHECK(g_sim.regs[92] == (uint8_t)((0xFF & ~0x70) | (5 << 4)), "set_ms CLK7 R-div keeps CLK6 bits");
	}

	/* set_ms DIVBY4 entry, then the exit that resets PLLA. */
	{
		Si5351 *si = boot();
		Si5351RegSet ms = {100, 0, 1};
		g_sim.regs[44] = 0x00;
		g_sim.regs[SI5351_PLL_RESET] = 0;
		si->set_ms(SI5351_CLK0, ms, 0, 0, 1);
		CHECK((g_sim.regs[44] & SI5351_OUTPUT_CLK_DIVBY4) == SI5351_OUTPUT_CLK_DIVBY4,
		      "set_ms CLK0 div_by_4 sets the DIVBY4 bits");
		CHECK(g_sim.regs[SI5351_PLL_RESET] == 0, "entering DIVBY4 does not reset the PLL");
		si->set_ms(SI5351_CLK0, ms, 0, 0, 0);
		CHECK((g_sim.regs[44] & SI5351_OUTPUT_CLK_DIVBY4) == 0, "set_ms clears DIVBY4");
		CHECK(g_sim.regs[SI5351_PLL_RESET] == SI5351_PLL_RESET_A,
		      "set_ms leaving DIVBY4 resets PLLA");
	}

	/* multisynth67_calc's pll_freq != 0 arm has no caller in v2.4.2 (set_freq
	 * always passes 0; set_freq_manual uses ms67_div). Locked here so a future
	 * caller cannot change the reject rules unnoticed. -Dprivate=public is the
	 * existing host-test switch; the library sources are not edited. */
	{
		Si5351 *si = boot();
		Si5351RegSet reg = {9, 9, 9};
		uint64_t got = si->multisynth67_calc(1400000000ULL, 90000000000ULL, &reg);
		CHECK(got == 0 && reg.p1 == 9, "LOCKED: non-integer PLL/MS returns 0 and does not write p1");
		got = si->multisynth67_calc(15000000000ULL, 60000000000ULL, &reg);
		CHECK(got == 0, "LOCKED: integer ratio 4 is below 6 and returns 0");
		got = si->multisynth67_calc(100000000ULL, 256ULL * 100000000ULL, &reg);
		CHECK(got == 0, "LOCKED: integer ratio 256 is above 254 and returns 0");
		got = si->multisynth67_calc(10000000000ULL, 60000000000ULL, &reg);
		CHECK(got == 1 && reg.p1 == 6 && reg.p2 == 0 && reg.p3 == 0,
		      "LOCKED: integer ratio 6 returns 1 and stores p1=6");
		reg.p1 = 0;
		got = si->multisynth67_calc(20000000000ULL, 0, &reg);
		CHECK(reg.p1 == 6 && got == 90000000000ULL,
		      "LOCKED: freq above 150 MHz is clamped before the integer search");
		got = si->multisynth67_calc(100000ULL, 0, &reg);
		CHECK(reg.p1 == 0 && got == 256ULL * 500000ULL * SI5351_FREQ_MULT,
		      "LOCKED: freq below 500 kHz clamps, then a+=2 truncates p1");
		printf("INFO  multisynth67_calc(freq, pll!=0) is unreachable from set_freq/set_freq_manual.\n");
		printf("INFO  multisynth67_calc a<6 is unreachable: freq is clamped to 150 MHz first, and that ratio is already 6.\n");
		printf("INFO  set_freq CLK6/7 reset_owed arm is unreachable: CLK6/7 set_ms never sets reset_owed, and every public path clears it before returning.\n");
		printf("INFO  multisynth_calc's lltmp==5 arm is unreachable: that ratio is 180 MHz, which is already DIVBY4.\n");
		printf("INFO  pll_calc a<15 is unreachable via set_ref_freq: the setter never stores a crystal above 30 MHz, and 600 MHz/30 MHz is 20.\n");
		printf("INFO  switch defaults in drive_strength, set_clock_source, set_pll_input, and set_clock_disable's clk>7 return need an out-of-range enum and are not tested.\n");
	}

	/* pll_calc's a>90 clamp. 7 MHz is inside ref_ok; 900/7 is 128. */
	{
		Si5351 *si = boot();
		si->set_ref_freq(7000000, SI5351_PLL_INPUT_XO);
		si->set_pll(90000000000ULL, SI5351_PLLA);
		CHECK(si->plla_freq == 90000000000ULL, "LOCKED: set_pll still stores the 900 MHz request at 7 MHz");
		CHECK(g_sim.regs[29] == 0x2B && g_sim.regs[30] == 0x00,
		      "7 MHz reference clamps PLL feedback a from 128 to 90");
	}

	/* multisynth_calc's >200 MHz clamp. set_freq clamps before it calls this,
	 * so the line is only reached by a direct call. Same -Dprivate=public
	 * switch as the rest of the host tests. */
	{
		Si5351 *si = boot();
		Si5351RegSet hi, cap;
		si->multisynth_calc(21000000000ULL, 0, &hi);
		si->multisynth_calc(20000000000ULL, 0, &cap);
		CHECK(hi.p1 == cap.p1 && hi.p2 == cap.p2 && hi.p3 == cap.p3,
		      "LOCKED: multisynth_calc above 200 MHz matches the 200 MHz parameters");
	}

	printf("%s (%d failures)\n", fails ? "FAILED" : "ALL PASS", fails);
	return fails ? 1 : 0;
}
