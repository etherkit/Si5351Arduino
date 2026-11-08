/* R-divider edges (issue #89), AN619 Register 183 XTAL_CL, and a check that
 * README limit numbers match the header (and SI5351_FOUT_MAX in si5351.cpp).
 *
 * CLK0-5: select_r_div() octave table, plus the register 44 field set_freq()
 * writes. CLK6/7: set_freq() itself. That path does not call select_r_div();
 * it picks the smallest R whose doubled frequency reaches 600 MHz/254.
 *
 * Compile with -Dprivate=public so select_r_div() can be called.
 * Usage: t_rdiv README.md src/si5351.cpp
 */
#include "si5351.h"
#include "sim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, msg) do { \
	int _ok = (c); \
	printf("%s  %s\n", _ok ? "PASS" : "FAIL", msg); \
	if (!_ok) fails++; \
} while (0)

/* MS6/7 minimum in 0.01 Hz units: ceil(600 MHz / 254). Same expression as
 * SI5351_MS67_MIN_FREQ in si5351.cpp. */
static uint64_t ms67_min_centi(void)
{
	return (SI5351_PLL_VCO_MIN * (uint64_t)SI5351_FREQ_MULT
		+ SI5351_MULTISYNTH67_A_MAX - 1) / SI5351_MULTISYNTH67_A_MAX;
}

/* What set_freq() does for a solo CLK6/7: clamp up to SI5351_CLKOUT67_MIN,
 * then the smallest R in 0..7 with fout << R >= MS67_MIN. */
static uint8_t r_for_clk67(uint64_t centi)
{
	uint64_t ms = ms67_min_centi();
	uint64_t floor = (ms + 127) / 128;
	uint8_t r = 0;

	if (centi < floor)
	{
		centi = floor;
	}
	while (centi < ms && r < 7)
	{
		centi <<= 1;
		r++;
	}
	return r;
}

/* One octave per R code for CLK0-5. Low edge inclusive, high edge exclusive.
 * The distinct-code mask is built from the divider select_r_div() returned,
 * not from the loop index, so a duplicated band leaves a hole. */
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
		snprintf(buf, sizeof buf, "%s /%u low %llu Hz -> code %u", tag, mul,
			(unsigned long long)lo_hz, r);
		CHECK(r == (uint8_t)shift && f == lo_hz * SI5351_FREQ_MULT * mul, buf);
		seen |= (1u << r);
		if (min_hz == SI5351_CLKOUT_MIN_FREQ)
		{
			CHECK(f >= 500000ULL * SI5351_FREQ_MULT, "multisynth at/above 500 kHz");
		}

		f = hi_hz * SI5351_FREQ_MULT - 1;
		r = si.select_r_div(&f, min_hz);
		snprintf(buf, sizeof buf, "%s /%u just below %llu Hz -> code %u", tag, mul,
			(unsigned long long)hi_hz, r);
		CHECK(r == (uint8_t)shift && f == (hi_hz * SI5351_FREQ_MULT - 1) * mul, buf);

		f = hi_hz * SI5351_FREQ_MULT;
		r = si.select_r_div(&f, min_hz);
		snprintf(buf, sizeof buf, "%s boundary %llu Hz is not /%u (got code %u)", tag,
			(unsigned long long)hi_hz, mul, r);
		CHECK(r != (uint8_t)shift, buf);
	}

	{
		uint64_t start_hz = (uint64_t)min_hz << 7;
		uint64_t f = start_hz * SI5351_FREQ_MULT;
		uint8_t r = si.select_r_div(&f, min_hz);
		char buf[96];
		snprintf(buf, sizeof buf, "%s /1 from %llu Hz -> code %u", tag,
			(unsigned long long)start_hz, r);
		CHECK(r == SI5351_OUTPUT_CLK_DIV_1 && f == start_hz * SI5351_FREQ_MULT, buf);
		seen |= (1u << r);
	}

	CHECK(seen == 0xFFu, "eight distinct R codes from select_r_div (1,2,4,8,16,32,64,128)");
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

static uint8_t reg92_r(enum si5351_clock clk)
{
	uint8_t v = g_sim.regs[SI5351_CLK6_7_OUTPUT_DIVIDER];
	if (clk == SI5351_CLK6)
	{
		return v & 7;
	}
	return (v >> SI5351_OUTPUT_CLK_DIV_SHIFT) & 7;
}

/* Register 92 after a real set_freq(). CLK6 is bits 2:0, CLK7 is bits 6:4. */
static void check_clk67(uint64_t centi, uint8_t expect, const char *name, uint32_t *seen)
{
	enum si5351_clock clks[2] = { SI5351_CLK6, SI5351_CLK7 };
	int i;

	CHECK(r_for_clk67(centi) == expect, "edge table matches 600 MHz/254 formula");
	for (i = 0; i < 2; i++)
	{
		enum si5351_clock clk = clks[i];
		Si5351 si;
		uint8_t got;
		char buf[128];

		g_sim.reset(0, true);
		CHECK(si.init(SI5351_CRYSTAL_LOAD_8PF, 0, 0), "init before CLK6/7 set_freq");
		snprintf(buf, sizeof buf, "%s CLK%d set_freq", name, (int)clk);
		CHECK(si.set_freq(centi, clk) == 0, buf);
		got = reg92_r(clk);
		snprintf(buf, sizeof buf, "%s CLK%d reg92 R code %u (got %u)", name, (int)clk, expect, got);
		CHECK(got == expect, buf);
		if (clk == SI5351_CLK6 && seen)
		{
			*seen |= (1u << got);
		}
	}
}

static char *read_file(const char *path)
{
	FILE *fp = fopen(path, "rb");
	long n;
	char *b;

	if (!fp)
	{
		return NULL;
	}
	if (fseek(fp, 0, SEEK_END) != 0 || (n = ftell(fp)) < 0)
	{
		fclose(fp);
		return NULL;
	}
	rewind(fp);
	b = (char *)malloc((size_t)n + 1);
	if (!b || fread(b, 1, (size_t)n, fp) != (size_t)n)
	{
		free(b);
		fclose(fp);
		return NULL;
	}
	b[n] = 0;
	fclose(fp);
	return b;
}

static unsigned long long parse_define(const char *src, const char *name)
{
	char key[80];
	const char *p;
	snprintf(key, sizeof key, "#define %s", name);
	p = strstr(src, key);
	if (!p)
	{
		return 0;
	}
	p += strlen(key);
	while (*p == ' ' || *p == '\t')
	{
		p++;
	}
	return strtoull(p, NULL, 0);
}

static void require_text(const char *readme, const char *needle)
{
	char buf[160];
	snprintf(buf, sizeof buf, "README contains: %s", needle);
	CHECK(strstr(readme, needle) != NULL, buf);
}

static void check_readme(const char *readme_path, const char *cpp_path)
{
	char *readme = read_file(readme_path);
	char *cpp = read_file(cpp_path);
	char buf[160];
	unsigned long long fout_max;
	unsigned long q;
	uint64_t floor_c, cap_c, cap_hz;

	CHECK(readme != NULL, "README opened");
	CHECK(cpp != NULL, "si5351.cpp opened");
	if (!readme || !cpp)
	{
		free(readme);
		free(cpp);
		return;
	}

	fout_max = parse_define(cpp, "SI5351_FOUT_MAX");
	snprintf(buf, sizeof buf, "parsed SI5351_FOUT_MAX (%llu)", fout_max);
	CHECK(fout_max == 200000000ULL, buf);
	snprintf(buf, sizeof buf, "`SI5351_FOUT_MAX` as %llu", fout_max);
	require_text(readme, buf);

	snprintf(buf, sizeof buf, "`SI5351_CLKOUT_MIN_FREQ` is %lu",
		(unsigned long)SI5351_CLKOUT_MIN_FREQ);
	require_text(readme, buf);
	snprintf(buf, sizeof buf, "`SI5351_CLKOUT67_MIN_FREQ` is %lu Hz",
		(unsigned long)SI5351_CLKOUT67_MIN_FREQ);
	require_text(readme, buf);
	snprintf(buf, sizeof buf, "`SI5351_MULTISYNTH_A_MIN` is %lu",
		(unsigned long)SI5351_MULTISYNTH_A_MIN);
	require_text(readme, buf);
	snprintf(buf, sizeof buf, "`SI5351_MULTISYNTH_A_MAX` is %lu",
		(unsigned long)SI5351_MULTISYNTH_A_MAX);
	require_text(readme, buf);
	snprintf(buf, sizeof buf, "`SI5351_MULTISYNTH67_A_MAX` is %lu",
		(unsigned long)SI5351_MULTISYNTH67_A_MAX);
	require_text(readme, buf);
	snprintf(buf, sizeof buf, "`SI5351_PLL_A_MIN` is %lu",
		(unsigned long)SI5351_PLL_A_MIN);
	require_text(readme, buf);
	snprintf(buf, sizeof buf, "`SI5351_PLL_A_MAX` is %lu",
		(unsigned long)SI5351_PLL_A_MAX);
	require_text(readme, buf);
	snprintf(buf, sizeof buf, "still contain %lu",
		(unsigned long)SI5351_CLKOUT_MAX_FREQ);
	require_text(readme, buf);

	floor_c = (ms67_min_centi() + 127) / 128;
	snprintf(buf, sizeof buf, "%llu.%02llu Hz",
		(unsigned long long)(floor_c / 100),
		(unsigned long long)(floor_c % 100));
	require_text(readme, buf);

	cap_c = (uint64_t)SI5351_MULTISYNTH_DIVBY4_FREQ * SI5351_FREQ_MULT - 1;
	cap_hz = cap_c / 100;
	snprintf(buf, sizeof buf, "%llu,%03llu,%03llu.%02llu",
		(unsigned long long)(cap_hz / 1000000ULL),
		(unsigned long long)((cap_hz / 1000ULL) % 1000ULL),
		(unsigned long long)(cap_hz % 1000ULL),
		(unsigned long long)(cap_c % 100));
	require_text(readme, buf);
	require_text(readme, "`set_freq()` clamps CLK6 and CLK7 to 149,999,999.99 Hz");
	require_text(readme, "set_freq_manual(150 MHz, 900 MHz PLL, CLK6)");

	q = SI5351_PLL_VCO_MIN / 127;
	snprintf(buf, sizeof buf, "%lu,%03lu,%03lu Hz",
		q / 1000000UL, (q / 1000UL) % 1000UL, q % 1000UL);
	require_text(readme, buf);

	require_text(readme, "from 4 kHz to 200 MHz");
	CHECK(strstr(readme, "from 4 kHz to 225 MHz") == NULL,
		"README does not say set_freq runs to 225 MHz");

	free(readme);
	free(cpp);
}

int main(int argc, char **argv)
{
	Si5351 si;
	uint32_t seen67 = 0;
	/* Literal edges, in 0.01 Hz units. 36908 Hz is the case that the
	 * CLKOUT67_MIN_FREQ octave table gets wrong (/64); set_freq writes /128
	 * because 36908*64 Hz is still under 600 MHz / 254. */
	static const struct { uint64_t centi; uint8_t r; const char *name; int band_low; } edges[] = {
		{ 1845472ULL, 7, "below 18454.73 Hz clamps into /128", 0 },
		{ 1845473ULL, 7, "/128 from 18454.73 Hz", 1 },
		{ 3690800ULL, 7, "36908 Hz is /128", 0 },
		{ 3690944ULL, 7, "just below /64", 0 },
		{ 3690945ULL, 6, "/64 from 36909.45 Hz", 1 },
		{ 7381889ULL, 6, "just below /32", 0 },
		{ 7381890ULL, 5, "/32 from 73818.90 Hz", 1 },
		{ 14763779ULL, 5, "just below /16", 0 },
		{ 14763780ULL, 4, "/16 from 147637.80 Hz", 1 },
		{ 29527559ULL, 4, "just below /8", 0 },
		{ 29527560ULL, 3, "/8 from 295275.60 Hz", 1 },
		{ 59055118ULL, 3, "just below /4", 0 },
		{ 59055119ULL, 2, "/4 from 590551.19 Hz", 1 },
		{ 118110236ULL, 2, "just below /2", 0 },
		{ 118110237ULL, 1, "/2 from 1181102.37 Hz", 1 },
		{ 236220472ULL, 1, "just below /1", 0 },
		{ 236220473ULL, 0, "/1 from 2362204.73 Hz", 1 },
	};
	size_t i;

	if (argc != 3)
	{
		fprintf(stderr, "usage: %s README.md src/si5351.cpp\n", argv[0]);
		return 2;
	}

	check_octaves(si, SI5351_CLKOUT_MIN_FREQ, "CLK0-5");

	{
		uint64_t f = (uint64_t)(SI5351_CLKOUT_MIN_FREQ - 1) * SI5351_FREQ_MULT;
		uint64_t orig = f;
		uint8_t r = si.select_r_div(&f, SI5351_CLKOUT_MIN_FREQ);
		CHECK(r == SI5351_OUTPUT_CLK_DIV_1 && f == orig, "below 4 kHz: select_r_div does not scale");
	}

	for (i = 0; i < sizeof edges / sizeof edges[0]; i++)
	{
		check_clk67(edges[i].centi, edges[i].r, edges[i].name,
			edges[i].band_low ? &seen67 : NULL);
	}
	CHECK(seen67 == 0xFFu, "eight distinct CLK6 reg92 R codes from set_freq");

	check_xtal(SI5351_CRYSTAL_LOAD_6PF, 0x52, "reg183 6 pF is 0x52");
	check_xtal(SI5351_CRYSTAL_LOAD_8PF, 0x92, "reg183 8 pF is 0x92");
	check_xtal(SI5351_CRYSTAL_LOAD_10PF, 0xD2, "reg183 10 pF is 0xD2");
	check_xtal(SI5351_CRYSTAL_LOAD_0PF, 0xD2, "reg183 reserved 0 pF keeps 10 pF (0xD2)");
	CHECK(SI5351_CRYSTAL_LOAD_0PF == (0 << 6), "0 pF macro is still the reserved code");

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

	CHECK(si.set_freq(225000000ULL * SI5351_FREQ_MULT, SI5351_CLK0) == 0, "set_freq 225 MHz");
	CHECK(si.clk_freq[0] == 200000000ULL * SI5351_FREQ_MULT, "225 MHz stored as 200 MHz");
	CHECK(SI5351_CLKOUT_MAX_FREQ == 225000000UL, "compatibility macro still says 225 MHz");

	/* set_freq stops CLK6 just under 150 MHz. set_freq_manual(150 MHz, 900 MHz)
	 * is an even ratio of 6 and is accepted. */
	{
		uint64_t f150 = 150000000ULL * SI5351_FREQ_MULT;
		uint64_t pll900 = 900000000ULL * SI5351_FREQ_MULT;
		Si5351 s2;

		g_sim.reset(0, true);
		CHECK(s2.init(SI5351_CRYSTAL_LOAD_8PF, 0, 0), "init before CLK6 150 MHz");
		CHECK(s2.set_freq(f150, SI5351_CLK6) == 0, "set_freq 150 MHz CLK6");
		CHECK(s2.clk_freq[SI5351_CLK6] == f150 - 1, "set_freq stores 149999999.99 Hz");
		CHECK(reg92_r(SI5351_CLK6) == 0, "set_freq 150 MHz CLK6 uses R=/1");

		g_sim.reset(0, true);
		CHECK(s2.init(SI5351_CRYSTAL_LOAD_8PF, 0, 0), "init before CLK6 manual 150 MHz");
		CHECK(s2.set_freq_manual(f150, pll900, SI5351_CLK6) == 0,
			"set_freq_manual 150 MHz / 900 MHz CLK6 returns 0");
		CHECK(s2.clk_freq[SI5351_CLK6] == f150, "manual 150 MHz is stored as 150 MHz");
		CHECK(reg92_r(SI5351_CLK6) == 0, "manual 150 MHz CLK6 uses R=/1");
		CHECK(g_sim.regs[SI5351_CLK6_PARAMETERS] == 6, "manual 150/900 writes MS6 ratio 6");
	}

	si.set_phase(SI5351_CLK0, 255);
	CHECK(g_sim.regs[SI5351_CLK0_PHASE_OFFSET] == 127, "set_phase masks to 127");
	si.set_phase(SI5351_CLK0, 128);
	CHECK(g_sim.regs[SI5351_CLK0_PHASE_OFFSET] == 0, "phase word 128 does not fit in bits 6:0");
	CHECK(SI5351_PLL_VCO_MIN / 127 == 4724409UL, "600 MHz / 127 = 4724409 Hz");

	check_readme(argv[1], argv[2]);

	printf("%s (%d failures)\n", fails ? "FAILED" : "ALL PASS", fails);
	return fails ? 1 : 0;
}
