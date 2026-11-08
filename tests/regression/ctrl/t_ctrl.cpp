/* Gating checks for the control-register and status API on v2.4.2.
 *
 * Every value exercised is a named enumerator or the documented 0/1 enable
 * argument. Out-of-range enum casts are undefined (CWG 1766) and are not
 * used. The switch defaults in drive_strength and set_pll_input, and the
 * clk>CLK7 return in set_clock_disable, stay uncovered for that reason.
 *
 * Register files are seeded with a non-default byte so a read-modify-write
 * that drops unrelated bits fails.
 */
#include "si5351.h"
#include "sim.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <string>

static int fails = 0;
#define CHECK(c, msg) do { \
	int _ok = (c); \
	printf("%s  %s\n", _ok ? "PASS" : "FAIL", msg); \
	if (!_ok) fails++; \
} while (0)

static Si5351 *boot(int pattern)
{
	static Si5351 obj;
	g_sim.reset(pattern, true);
	obj.~Si5351();
	Si5351 *s = new (&obj) Si5351();
	CHECK(s->init(SI5351_CRYSTAL_LOAD_8PF, 0, 0), "init");
	g_sim.events.clear();
	s->get_i2c_error();
	return s;
}

static int last_write(uint8_t *reg, uint8_t *val)
{
	for (int i = (int)g_sim.events.size() - 1; i >= 0; i--) {
		unsigned r, n;
		unsigned b;
		if (sscanf(g_sim.events[i].c_str(), "W a=60 r=%u n=%u [%x]", &r, &n, &b) == 3
		    && n == 1) {
			*reg = (uint8_t)r;
			*val = (uint8_t)b;
			return 1;
		}
	}
	return 0;
}

static int saw_read(uint8_t reg)
{
	char needle[32];
	snprintf(needle, sizeof needle, "PTR a=60 r=%u", reg);
	for (size_t i = 0; i < g_sim.events.size(); i++)
		if (g_sim.events[i] == needle) return 1;
	return 0;
}

/* Read-modify-write traffic: pointer, then the data read, then the data write.
 * The read line carries the device address, not the register (that is the PTR). */
static int ordered_rmw(uint8_t reg, uint8_t val)
{
	int ptr = -1, rd = -1, wr = -1;
	char pbuf[32];
	snprintf(pbuf, sizeof pbuf, "PTR a=60 r=%u", reg);
	for (int i = 0; i < (int)g_sim.events.size(); i++) {
		const std::string &e = g_sim.events[i];
		if (e == pbuf) ptr = i;
		if (e.compare(0, 9, "R a=60 n=") == 0) rd = i;
		unsigned r, n, b;
		if (sscanf(e.c_str(), "W a=60 r=%u n=%u [%x]", &r, &n, &b) == 3
		    && r == reg && n == 1 && b == val) wr = i;
	}
	return ptr >= 0 && rd > ptr && wr > rd;
}

int main(void)
{
	const si5351_drive drives[4] = {
		SI5351_DRIVE_2MA, SI5351_DRIVE_4MA, SI5351_DRIVE_6MA, SI5351_DRIVE_8MA
	};
	const uint8_t drive_bits[4] = {0x00, 0x01, 0x02, 0x03};

	/* Drive strength, every output, every legal level. Seed 0xA5. */
	for (int clk = 0; clk < 8; clk++) {
		for (int d = 0; d < 4; d++) {
			Si5351 *si = boot(0);
			uint8_t addr = (uint8_t)(SI5351_CLK0_CTRL + clk);
			g_sim.regs[addr] = 0xA5;
			g_sim.events.clear();
			si->drive_strength((si5351_clock)clk, drives[d]);
			uint8_t expect = (uint8_t)((0xA5 & ~0x03) | drive_bits[d]);
			uint8_t wr, wv;
			char buf[96];
			snprintf(buf, sizeof buf, "drive CLK%d %dmA reg", clk, 2 * (d + 1));
			CHECK(last_write(&wr, &wv) && wr == addr && wv == expect
			      && g_sim.regs[addr] == expect && ordered_rmw(addr, expect), buf);
			CHECK((g_sim.regs[addr] & ~0x03) == (0xA5 & ~0x03), "drive preserves other bits");
		}
	}

	/* Power and invert, both states, CLK0 and CLK7, from 0xA5. */
	for (int clk = 0; clk <= 7; clk += 7) {
		uint8_t addr = (uint8_t)(SI5351_CLK0_CTRL + clk);
		Si5351 *si = boot(0);
		g_sim.regs[addr] = 0xA5;
		g_sim.events.clear();
		si->set_clock_pwr((si5351_clock)clk, 1);
		CHECK(g_sim.regs[addr] == (uint8_t)(0xA5 & 0x7F), "pwr on clears only PDN");
		g_sim.events.clear();
		si->set_clock_pwr((si5351_clock)clk, 0);
		CHECK(g_sim.regs[addr] == 0xA5, "pwr off sets only PDN");
		g_sim.regs[addr] = 0xA5;
		g_sim.events.clear();
		si->set_clock_invert((si5351_clock)clk, 1);
		CHECK(g_sim.regs[addr] == (uint8_t)(0xA5 | SI5351_CLK_INVERT), "invert on");
		CHECK(saw_read(addr), "invert read before write");
		si->set_clock_invert((si5351_clock)clk, 0);
		CHECK(g_sim.regs[addr] == 0xA5, "invert off restores seed");
	}

	/* Clock source. CLK0 + MS0 returns without a write (defined, not a cast). */
	{
		const si5351_clock_source srcs[4] = {
			SI5351_CLK_SRC_XTAL, SI5351_CLK_SRC_CLKIN,
			SI5351_CLK_SRC_MS0, SI5351_CLK_SRC_MS
		};
		const uint8_t bits[4] = {
			SI5351_CLK_INPUT_XTAL, SI5351_CLK_INPUT_CLKIN,
			SI5351_CLK_INPUT_MULTISYNTH_0_4, SI5351_CLK_INPUT_MULTISYNTH_N
		};
		for (int s = 0; s < 4; s++) {
			Si5351 *si = boot(0);
			g_sim.regs[SI5351_CLK0_CTRL] = 0xA5;
			unsigned long w0 = g_sim.n_writes;
			g_sim.events.clear();
			si->set_clock_source(SI5351_CLK0, srcs[s]);
			if (srcs[s] == SI5351_CLK_SRC_MS0) {
				/* The read still happens; the early return must not write. */
				CHECK(g_sim.n_writes == w0 && g_sim.regs[SI5351_CLK0_CTRL] == 0xA5
				      && saw_read(SI5351_CLK0_CTRL), "CLK0 MS0 source reads and does not write");
			} else {
				uint8_t expect = (uint8_t)((0xA5 & ~SI5351_CLK_INPUT_MASK) | bits[s]);
				CHECK(g_sim.regs[SI5351_CLK0_CTRL] == expect, "CLK0 source field");
				CHECK((g_sim.regs[SI5351_CLK0_CTRL] & ~SI5351_CLK_INPUT_MASK)
				      == (0xA5 & ~SI5351_CLK_INPUT_MASK), "CLK0 source preserves other bits");
			}
		}
		/* CLK4 can take MS0 (the MS4 mirror encoding) and its own MS. */
		for (int s = 0; s < 4; s++) {
			Si5351 *si = boot(0);
			uint8_t addr = SI5351_CLK0_CTRL + 4;
			g_sim.regs[addr] = 0xA5;
			si->set_clock_source(SI5351_CLK4, srcs[s]);
			uint8_t expect = (uint8_t)((0xA5 & ~SI5351_CLK_INPUT_MASK) | bits[s]);
			CHECK(g_sim.regs[addr] == expect, "CLK4 source field");
		}
		/* CLK6/CLK7 own-multisynth and XTAL/CLKIN. MS0 is the MS4 code, legal. */
		for (int clk = 6; clk <= 7; clk++) {
			for (int s = 0; s < 4; s++) {
				Si5351 *si = boot(0);
				uint8_t addr = (uint8_t)(SI5351_CLK0_CTRL + clk);
				g_sim.regs[addr] = 0x5A;
				si->set_clock_source((si5351_clock)clk, srcs[s]);
				uint8_t expect = (uint8_t)((0x5A & ~SI5351_CLK_INPUT_MASK) | bits[s]);
				CHECK(g_sim.regs[addr] == expect, "CLK6/7 source field");
			}
		}
	}

	/* Disable state: 4 modes x 8 clocks. Two bits each, other pairs preserved. */
	{
		const si5351_clock_disable modes[4] = {
			SI5351_CLK_DISABLE_LOW, SI5351_CLK_DISABLE_HIGH,
			SI5351_CLK_DISABLE_HI_Z, SI5351_CLK_DISABLE_NEVER
		};
		for (int clk = 0; clk < 8; clk++) {
			for (int m = 0; m < 4; m++) {
				Si5351 *si = boot(0);
				uint8_t reg = (clk <= 3) ? SI5351_CLK3_0_DISABLE_STATE
				                         : SI5351_CLK7_4_DISABLE_STATE;
				int sh = (clk <= 3) ? clk * 2 : (clk - 4) * 2;
				g_sim.regs[reg] = 0xFF;
				g_sim.events.clear();
				si->set_clock_disable((si5351_clock)clk, modes[m]);
				uint8_t expect = (uint8_t)((0xFF & ~(0x3 << sh)) | (m << sh));
				uint8_t wr, wv;
				CHECK(g_sim.regs[reg] == expect, "disable-state field");
				CHECK(last_write(&wr, &wv) && wr == reg && wv == expect && ordered_rmw(reg, expect),
				      "disable-state ordered read then write");
			}
		}
	}

	/* Fanout: 3 sources x enable/disable. Ten coverage branches include the
	 * switch's untaken default, which would require an out-of-range enum. */
	{
		const si5351_clock_fanout fans[3] = {
			SI5351_FANOUT_CLKIN, SI5351_FANOUT_XO, SI5351_FANOUT_MS
		};
		const uint8_t fbit[3] = {
			SI5351_CLKIN_ENABLE, SI5351_XTAL_ENABLE, SI5351_MULTISYNTH_ENABLE
		};
		for (int f = 0; f < 3; f++) {
			Si5351 *si = boot(0);
			g_sim.regs[SI5351_FANOUT_ENABLE] = 0x2A;
			g_sim.events.clear();
			si->set_clock_fanout(fans[f], 1);
			CHECK(g_sim.regs[SI5351_FANOUT_ENABLE] == (uint8_t)(0x2A | fbit[f])
			      && ordered_rmw(SI5351_FANOUT_ENABLE, (uint8_t)(0x2A | fbit[f])), "fanout on");
			si->set_clock_fanout(fans[f], 0);
			CHECK(g_sim.regs[SI5351_FANOUT_ENABLE] == (uint8_t)(0x2A & ~fbit[f]), "fanout off");
			CHECK((g_sim.regs[SI5351_FANOUT_ENABLE] & ~fbit[f]) == (0x2A & ~fbit[f]),
			      "fanout preserves unrelated bits");
		}
	}

	/* Status decode. Register 0 = 0xAE, register 1 = 0x51. */
	{
		Si5351 *si = boot(0);
		g_sim.regs[SI5351_DEVICE_STATUS] = 0xAE;
		g_sim.regs[SI5351_INTERRUPT_STATUS] = 0x51;
		g_sim.events.clear();
		si->update_status();
		CHECK(si->dev_status.SYS_INIT == 1 && si->dev_status.LOL_B == 0
		      && si->dev_status.LOL_A == 1 && si->dev_status.LOS_CLKIN == 0
		      && si->dev_status.LOS_XTAL == 1 && si->dev_status.LOS == 1
		      && si->dev_status.REVID == 2, "sys status 0xAE");
		CHECK(si->dev_int_status.SYS_INIT_STKY == 0 && si->dev_int_status.LOL_B_STKY == 1
		      && si->dev_int_status.LOL_A_STKY == 0 && si->dev_int_status.LOS_CLKIN_STKY == 1
		      && si->dev_int_status.LOS_XTAL_STKY == 0 && si->dev_int_status.LOS_STKY == 0,
		      "int status 0x51");
		CHECK(saw_read(SI5351_DEVICE_STATUS) && saw_read(SI5351_INTERRUPT_STATUS),
		      "update_status reads reg 0 then reg 1");
		g_sim.regs[0] = 0x00;
		g_sim.regs[1] = 0xFF;
		si->update_status();
		CHECK(si->dev_status.SYS_INIT == 0 && si->dev_status.REVID == 0
		      && si->dev_status.LOS_XTAL == 0
		      && si->dev_int_status.SYS_INIT_STKY == 1
		      && si->dev_int_status.LOS_XTAL_STKY == 1
		      && si->dev_int_status.LOS_STKY == 1, "status zeros and ones");
	}

	/* NACK: device stops acknowledging. endTransmission returns 2. */
	{
		Si5351 *si = boot(0);
		g_sim.present = false;
		g_sim.events.clear();
		uint8_t v = si->si5351_read(SI5351_DEVICE_STATUS);
		uint8_t e1 = si->get_i2c_error();
		uint8_t e2 = si->get_i2c_error();
		CHECK(v == 0 && e1 == 2 && e2 == 0, "read NACK sets error 2 and get_i2c_error clears it");
		unsigned long w0 = g_sim.n_writes;
		uint8_t we = si->si5351_write(SI5351_CLK0_CTRL, 0x11);
		CHECK(we == 2 && si->get_i2c_error() == 2 && g_sim.n_writes == w0
		      && g_sim.regs[SI5351_CLK0_CTRL] != 0x11, "write NACK does not store the byte");
	}

	/* requestFrom returns no payload after the register pointer write is ACKed. */
	{
		Si5351 *si = boot(0);
		g_sim.empty_read = true;
		g_sim.regs[SI5351_DEVICE_STATUS] = 0xAE;
		uint8_t v = si->si5351_read(SI5351_DEVICE_STATUS);
		CHECK(v == 0 && si->get_i2c_error() == SI5351_I2C_ERR_NO_DATA,
		      "empty requestFrom is SI5351_I2C_ERR_NO_DATA");
		g_sim.empty_read = false;
	}

	printf("%s (%d failures)\n", fails ? "FAILED" : "ALL PASS", fails);
	return fails ? 1 : 0;
}
