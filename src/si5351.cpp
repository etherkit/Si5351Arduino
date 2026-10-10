/*
 * si5351.cpp - Si5351 library for Arduino
 *
 * Copyright (C) 2015 - 2019 Jason Milldrum <milldrum@gmail.com>
 *                           Dana H. Myers <k6jq@comcast.net>
 *
 * Some tuning algorithms derived from clk-si5351.c in the Linux kernel.
 * Sebastian Hesselbarth <sebastian.hesselbarth@gmail.com>
 * Rabeeh Khoury <rabeeh@solid-run.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdint.h>

#include "Arduino.h"
#include "Wire.h"
#include "si5351.h"


/*********************/
/* File-local helpers */
/*********************/

/* Pack p1/p2/p3 into the 8-byte Si5351 parameter block used by PLL/MS regs.
 * p1_extra is ORed into the p1[17:16] byte (byte 2); set_ms uses this to
 * preserve other bits already in that register. set_pll/set_vcxo pass 0. */
// Set while set_freq() re-plans a PLL: it resets that PLL once after all
// PLL and MS writes, so set_ms() must not issue its own DIVBY4-exit reset.
static uint8_t defer_pll_reset;

// Datasheet / AN619 maximum output (and MS0-5 output) frequency. The public
// SI5351_MULTISYNTH_MAX_FREQ / SI5351_CLKOUT_MAX_FREQ (225 MHz) are kept for
// compatibility but are above the spec; DIVBY4 covers 150 < Fout <= 200 MHz.
#define SI5351_FOUT_MAX			200000000ULL

// MS6/MS7 divide ratios are even integers 6..254 (AN619 sec. 2.2, 4.1.2), so
// with fVCO >= 600 MHz the MS6/7 output is at least 600 MHz / 254 and, with
// R = 128, the lowest CLK6/7 output is 600 MHz / 254 / 128 (rounded up here;
// SI5351_CLKOUT67_MIN_FREQ is rounded down and would give a ratio of 256).
#define SI5351_MS67_MIN_FREQ	((SI5351_PLL_VCO_MIN * SI5351_FREQ_MULT + SI5351_MULTISYNTH67_A_MAX - 1) / SI5351_MULTISYNTH67_A_MAX)
#define SI5351_CLKOUT67_MIN		((SI5351_MS67_MIN_FREQ + 127) / 128)

// Even-integer MS6/7 ratio (6..254) of pll_freq for an output freq, using the
// smallest R divider that brings the ratio down to 254. Returns the ratio and
// sets *r_div, or returns 0 if no valid ratio/R pair exists.
static uint8_t ms67_div(uint64_t pll_freq, uint64_t freq, uint8_t *r_div)
{
	uint64_t n;
	uint8_t r;

	if(freq == 0 || pll_freq % freq)
	{
		return 0;
	}
	n = pll_freq / freq;

	for(r = 0; r <= 7; r++)
	{
		if(n <= SI5351_MULTISYNTH67_A_MAX)
		{
			if(n < SI5351_MULTISYNTH_A_MIN || (n & 1))
			{
				return 0;
			}
			*r_div = r;
			return (uint8_t)n;
		}
		if(n & 1)
		{
			return 0;
		}
		n >>= 1;
	}

	return 0;
}

static void pack_reg_set(const struct Si5351RegSet *reg, uint8_t *params, uint8_t p1_extra)
{
	params[0] = (uint8_t)((reg->p3 >> 8) & 0xFF);
	params[1] = (uint8_t)(reg->p3 & 0xFF);
	params[2] = (uint8_t)(((reg->p1 >> 16) & 0x03) | p1_extra);
	params[3] = (uint8_t)((reg->p1 >> 8) & 0xFF);
	params[4] = (uint8_t)(reg->p1 & 0xFF);
	params[5] = (uint8_t)(((reg->p3 >> 12) & 0xF0) | ((reg->p2 >> 16) & 0x0F));
	params[6] = (uint8_t)((reg->p2 >> 8) & 0xFF);
	params[7] = (uint8_t)(reg->p2 & 0xFF);
}

/* Fractional feedback a + b/c -> {p1,p2,p3}; compute (128*b)/c once. */
static void frac_to_reg(uint32_t a, uint32_t b, uint32_t c, struct Si5351RegSet *reg)
{
	uint32_t rem = (128 * b) / c;
	reg->p1 = 128 * a + rem - 512;
	reg->p2 = 128 * b - c * rem;
	reg->p3 = c;
}



/********************/
/* Public functions */
/********************/

Si5351::Si5351(TwoWire *wire, uint8_t i2c_addr)
{
	setup_obj(wire, i2c_addr);
}

// Shared constructor body (no C++11 delegating constructor needed)
void Si5351::setup_obj(TwoWire *wire, uint8_t i2c_addr)
{
	i2c_bus_addr = i2c_addr;
	_wire = wire ? wire : &Wire;
	xtal_freq[0] = SI5351_XTAL_FREQ;

	// Start by using XO ref osc as default for each PLL
	plla_ref_osc = SI5351_PLL_INPUT_XO;
	pllb_ref_osc = SI5351_PLL_INPUT_XO;
	clkin_div = SI5351_CLKIN_DIV_1;
}

/*
 * init(uint8_t xtal_load_c, uint32_t ref_osc_freq, int32_t corr)
 *
 * Setup communications to the Si5351 and set the crystal
 * load capacitance.
 *
 * xtal_load_c - Crystal load capacitance. Use the SI5351_CRYSTAL_LOAD_*PF
 * defines in the header file
 * xo_freq - Crystal/reference oscillator frequency in 1 Hz increments.
 * Defaults to 25000000 if a 0 is used here.
 * corr - Frequency correction constant in parts-per-billion
 *
 * Returns a boolean that indicates whether a device was found on the desired
 * I2C address.
 *
 */
bool Si5351::init(uint8_t xtal_load_c, uint32_t xo_freq, int32_t corr)
{
	// Start I2C comms on the selected bus. Called with no arguments so that
	// pins chosen beforehand (e.g. ESP32 setPins()) are kept; on ESP32 a bus
	// already started by the sketch with begin(sda, scl, freq) is left as is.
	_wire->begin();

	// Check for a device on the bus, bail out if it is not there
	_wire->beginTransmission(i2c_bus_addr);
	uint8_t reg_val;
  reg_val = _wire->endTransmission();

	if(reg_val == 0)
	{
		// Wait for SYS_INIT flag to be clear, indicating that device is ready
		uint8_t status_reg = 0;
		do
		{
			status_reg = si5351_read(SI5351_DEVICE_STATUS);
		} while (status_reg >> 7 == 1);

		// Set crystal load capacitance. XTAL_CL = 00b (SI5351_CRYSTAL_LOAD_0PF)
		// is reserved ("do not select", AN619 reg 183), so use the smallest
		// valid internal load, 6 pF, instead.
		xtal_load_c &= SI5351_CRYSTAL_LOAD_MASK;
		if(xtal_load_c == SI5351_CRYSTAL_LOAD_0PF)
		{
			xtal_load_c = SI5351_CRYSTAL_LOAD_6PF;
		}
		si5351_write(SI5351_CRYSTAL_LOAD, xtal_load_c | 0b00010010);

		// Set up the XO and CLKIN reference frequencies
		if (xo_freq != 0)
		{
			set_ref_freq(xo_freq, SI5351_PLL_INPUT_XO);
            set_ref_freq(xo_freq, SI5351_PLL_INPUT_CLKIN);          //Also CLKIN
		}
		else
		{
			set_ref_freq(SI5351_XTAL_FREQ, SI5351_PLL_INPUT_XO);
            set_ref_freq(SI5351_XTAL_FREQ, SI5351_PLL_INPUT_CLKIN); //Also CLKIN
		}

		// Set the frequency calibrations for the XO and CLKIN
		set_correction(corr, SI5351_PLL_INPUT_XO);
        set_correction(corr, SI5351_PLL_INPUT_CLKIN);

		reset();

		return true;
	}
	else
	{
		return false;
	}
}

/*
 * reset(void)
 *
 * Call to reset the Si5351 to the state initialized by the library.
 *
 */
void Si5351::reset(void)
{
	uint8_t i;

	// Initialize the CLK outputs according to flowchart in datasheet
	// First, turn them off
	for(i = 0; i < 8; i++)
	{
		si5351_write(SI5351_CLK0_CTRL + i, 0x80);
	}

	// Turn the clocks back on...
	for(i = 0; i < 8; i++)
	{
		si5351_write(SI5351_CLK0_CTRL + i, 0x0c);
	}

	// Set PLLA and PLLB to 800 MHz for automatic tuning
	set_pll(SI5351_PLL_FIXED, SI5351_PLLA);
	set_pll(SI5351_PLL_FIXED, SI5351_PLLB);

	// Make PLL to CLK assignments for automatic tuning
	// set_ms_source also updates pll_assignment[]
	for(i = 0; i < 6; i++)
	{
		set_ms_source((enum si5351_clock)i, SI5351_PLLA);
	}
	for(i = 6; i < 8; i++)
	{
		set_ms_source((enum si5351_clock)i, SI5351_PLLB);
	}

	// Reset the VCXO param
	si5351_write(SI5351_VXCO_PARAMETERS_LOW, 0);
	si5351_write(SI5351_VXCO_PARAMETERS_MID, 0);
	si5351_write(SI5351_VXCO_PARAMETERS_HIGH, 0);

	// Then reset the PLLs
	pll_reset(SI5351_PLLA);
	pll_reset(SI5351_PLLB);

	// Set initial frequencies
	for(i = 0; i < 8; i++)
	{
		clk_freq[i] = 0;
		output_enable((enum si5351_clock)i, 0);
		clk_first_set[i] = false;
	}
}

/*
 * set_freq(uint64_t freq, enum si5351_clock clk)
 *
 * Sets the clock frequency of the specified CLK output.
 * Frequency range of 8 kHz to 150 MHz
 *
 * freq - Output frequency in Hz
 * clk - Clock output
 *   (use the si5351_clock enum)
 */
uint8_t Si5351::set_freq(uint64_t freq, enum si5351_clock clk)
{
	struct Si5351RegSet ms_reg;
	uint64_t pll_freq;
	uint8_t int_mode = 0;
	uint8_t div_by_4 = 0;
	uint8_t r_div = 0;

	// Check which Multisynth is being set
	if((uint8_t)clk <= (uint8_t)SI5351_CLK5)
	{
		// MS0 through MS5 logic
		// ---------------------

		// Lower bounds check
		if(freq > 0 && freq < SI5351_CLKOUT_MIN_FREQ * SI5351_FREQ_MULT)
		{
			freq = SI5351_CLKOUT_MIN_FREQ * SI5351_FREQ_MULT;
		}

		// Upper bounds check
		if(freq > SI5351_FOUT_MAX * SI5351_FREQ_MULT)
		{
			freq = SI5351_FOUT_MAX * SI5351_FREQ_MULT;
		}

		// Is another output on the same PLL already >100 MHz (and so
		// holding the PLL at a frequency chosen for it)?
		uint8_t i;
		uint8_t pll_held = 0;
		for(i = 0; i < 6; i++)
		{
			if(clk_freq[i] > (SI5351_MULTISYNTH_SHARE_MAX * SI5351_FREQ_MULT)
				&& i != (uint8_t)clk && pll_assignment[i] == pll_assignment[clk])
			{
				pll_held = 1;
			}
		}

		// Enable the output on first set_freq only
		// (a >100 MHz request is refused below if the PLL is held)
		if(!(pll_held && freq > (SI5351_MULTISYNTH_SHARE_MAX * SI5351_FREQ_MULT))
			&& clk_first_set[(uint8_t)clk] == false)
		{
			output_enable(clk, 1);
			clk_first_set[(uint8_t)clk] = true;
		}

		// If requested freq >100 MHz and no other outputs are already >100 MHz,
		// we need to recalculate PLLA and then recalculate all other CLK outputs
		// on same PLL
		if(freq > (SI5351_MULTISYNTH_SHARE_MAX * SI5351_FREQ_MULT))
		{
			if(pll_held)
			{
				return 1; // won't set if any other clks already >100 MHz
			}

			// Set the freq in memory
			clk_freq[(uint8_t)clk] = freq;

			// Calculate the proper PLL frequency
			pll_freq = multisynth_calc(freq, 0, &ms_reg);
		}
		else
		{
			clk_freq[(uint8_t)clk] = freq;

			// Select the proper R div value
			r_div = select_r_div(&freq, SI5351_CLKOUT_MIN_FREQ);

			pll_freq = (pll_assignment[clk] == SI5351_PLLA) ? plla_freq : pllb_freq;

			// A >100 MHz setting can leave the PLL low (e.g. 620 MHz for 155 MHz),
			// making the MS ratio for this output < 8, outside the valid
			// fractional range (155 -> 96 MHz gave ~88.5 MHz, issue #65). If no
			// other output holds the PLL, re-plan it at the fixed 800 MHz below.
			uint8_t replan = 0;
			if(pll_freq < freq * 8)
			{
				replan = 1;
				if(pll_held)
				{
					// The PLL is held above 100 MHz by another output (#65:
					// 155 MHz on CLK0, then 99 MHz on CLK2), so move this output
					// to the other PLL: reuse it as is if it gives a valid ratio,
					// or re-plan it if no other output uses it. Otherwise keep
					// the old behaviour.
					enum si5351_pll other = (pll_assignment[clk] == SI5351_PLLA) ? SI5351_PLLB : SI5351_PLLA;
					uint64_t other_freq = (other == SI5351_PLLA) ? plla_freq : pllb_freq;
					uint8_t other_used = 0;
					for(i = 0; i < 8; i++)
					{
						if(i != (uint8_t)clk && clk_freq[i] != 0 && pll_assignment[i] == other)
						{
							other_used = 1;
						}
					}

					replan = 0;
					if(other_freq >= freq * 8)
					{
						set_ms_source(clk, other);
						pll_freq = other_freq;
					}
					else if(!other_used)
					{
						set_ms_source(clk, other);
						replan = 1;
					}
				}
			}

			if(!replan)
			{
				// Calculate the synth parameters
				multisynth_calc(freq, pll_freq, &ms_reg);

				// Set multisynth registers
				set_ms(clk, ms_reg, int_mode, r_div, div_by_4);

				return 0;
			}

			pll_freq = SI5351_PLL_FIXED;
		}

		// Set PLL
		set_pll(pll_freq, pll_assignment[clk]);

		// Recalculate params for other synths on same PLL
		defer_pll_reset = 1;
		for(i = 0; i < 6; i++)
		{
			if(clk_freq[i] != 0 && pll_assignment[i] == pll_assignment[clk])
			{
				struct Si5351RegSet temp_reg;
				uint64_t temp_freq;

				// Select the proper R div value
				temp_freq = clk_freq[i];
				r_div = select_r_div(&temp_freq, SI5351_CLKOUT_MIN_FREQ);

				multisynth_calc(temp_freq, pll_freq, &temp_reg);

				// If freq > 150 MHz, we need to use DIVBY4 and integer mode
				div_by_4 = (temp_freq >= SI5351_MULTISYNTH_DIVBY4_FREQ * SI5351_FREQ_MULT);

				// Set multisynth registers
				set_ms((enum si5351_clock)i, temp_reg, div_by_4, r_div, div_by_4);
			}
		}

		// Reset the PLL (once, after all PLL and MS writes)
		defer_pll_reset = 0;
		pll_reset(pll_assignment[clk]);

		return 0;
	}
	else
	{
		// MS6 and MS7 logic
		// -----------------
		uint8_t peer = (clk == SI5351_CLK6) ? (uint8_t)SI5351_CLK7 : (uint8_t)SI5351_CLK6;

		// There is no valid MS6/7 setting for 0 Hz
		if(freq == 0)
		{
			return 1;
		}

		// Lower bounds check (lowest valid MS6/7 ratio, VCO and R combination)
		if(freq < SI5351_CLKOUT67_MIN)
		{
			freq = SI5351_CLKOUT67_MIN;
		}

		// Upper bounds check
		if(freq >= SI5351_MULTISYNTH_DIVBY4_FREQ * SI5351_FREQ_MULT)
		{
			freq = SI5351_MULTISYNTH_DIVBY4_FREQ * SI5351_FREQ_MULT - 1;
		}

		// If one of CLK6 or CLK7 is already set when trying to set the other,
		// we have to ensure that it will also have an even integer division
		// ratio (6..254, using the R divider if needed) with the same PLL,
		// otherwise do not set it.
		if(clk_freq[peer] != 0)
		{
			ms_reg.p1 = ms67_div(pllb_freq, freq, &r_div);
			if(ms_reg.p1 == 0)
			{
				return 1;
			}
			ms_reg.p2 = 0;
			ms_reg.p3 = 0;

			// Set the freq in memory
			clk_freq[(uint8_t)clk] = freq;
		}
		else
		{
			// No previous assignment, so set PLLB based on this clock

			// Set the freq in memory
			clk_freq[(uint8_t)clk] = freq;

			// Smallest R divider that keeps the MS6/7 ratio <= 254
			while(freq < SI5351_MS67_MIN_FREQ && r_div < SI5351_OUTPUT_CLK_DIV_128)
			{
				freq <<= 1;
				r_div++;
			}

			pll_freq = multisynth67_calc(freq, 0, &ms_reg);
			// Preserve original asymmetry: CLK6 hardcodes PLLB; CLK7 uses assignment
			if(clk == SI5351_CLK6)
			{
				set_pll(pll_freq, SI5351_PLLB);
			}
			else
			{
				set_pll(pll_freq, pll_assignment[clk]);
			}
		}

		// Enable the output on first set_freq only, as for CLK0-5 (DEV-14).
		// reset() already powers up CLK6/7 (reg 22/23 = 0x0C).
		if(clk_first_set[(uint8_t)clk] == false)
		{
			output_enable(clk, 1);
			clk_first_set[(uint8_t)clk] = true;
		}

		div_by_4 = 0;
		int_mode = 0;

		// Set multisynth registers (MS must be set before PLL)
		set_ms(clk, ms_reg, int_mode, r_div, div_by_4);

		return 0;
	}
}

/*
 * set_freq_manual(uint64_t freq, uint64_t pll_freq, enum si5351_clock clk)
 *
 * Sets the clock frequency of the specified CLK output using the given PLL
 * frequency. You must ensure that the MS is assigned to the correct PLL and
 * that the PLL is set to the correct frequency before using this method.
 *
 * It is important to note that if you use this method, you will have to
 * track that all settings are sane yourself.
 *
 * freq - Output frequency in Hz
 * pll_freq - Frequency of the PLL driving the Multisynth in Hz * 100
 * clk - Clock output
 *   (use the si5351_clock enum)
 */
uint8_t Si5351::set_freq_manual(uint64_t freq, uint64_t pll_freq, enum si5351_clock clk)
{
	struct Si5351RegSet ms_reg;
	uint8_t int_mode = 0;
	uint8_t div_by_4 = 0;

	// Lower bounds check
	if(freq > 0 && freq < SI5351_CLKOUT_MIN_FREQ * SI5351_FREQ_MULT)
	{
		freq = SI5351_CLKOUT_MIN_FREQ * SI5351_FREQ_MULT;
	}

	// Upper bounds check
	if(freq > SI5351_FOUT_MAX * SI5351_FREQ_MULT)
	{
		freq = SI5351_FOUT_MAX * SI5351_FREQ_MULT;
	}

	uint8_t r_div = 0;

	// MS6/7 only take even integer ratios 6..254 (AN619): find one with the
	// R divider, or refuse before writing anything if there is none
	if((uint8_t)clk >= (uint8_t)SI5351_CLK6)
	{
		ms_reg.p1 = ms67_div(pll_freq, freq, &r_div);
		if(ms_reg.p1 == 0)
		{
			return 1;
		}
		ms_reg.p2 = 0;
		ms_reg.p3 = 0;
	}

	clk_freq[(uint8_t)clk] = freq;

	set_pll(pll_freq, pll_assignment[clk]);

	// Enable the output
	output_enable(clk, 1);

	if((uint8_t)clk <= (uint8_t)SI5351_CLK5)
	{
		// Select the proper R div value
		r_div = select_r_div(&freq, SI5351_CLKOUT_MIN_FREQ);

		// Calculate the synth parameters
		multisynth_calc(freq, pll_freq, &ms_reg);

		// If freq > 150 MHz, we need to use DIVBY4 and integer mode
		if(freq >= SI5351_MULTISYNTH_DIVBY4_FREQ * SI5351_FREQ_MULT)
		{
			div_by_4 = 1;
			int_mode = 1;
		}
	}

	// Set multisynth registers (MS must be set before PLL)
	set_ms(clk, ms_reg, int_mode, r_div, div_by_4);

    return 0;
}

/*
 * set_pll(uint64_t pll_freq, enum si5351_pll target_pll)
 *
 * Set the specified PLL to a specific oscillation frequency
 *
 * pll_freq - Desired PLL frequency in Hz * 100
 * target_pll - Which PLL to set
 *     (use the si5351_pll enum)
 */
void Si5351::set_pll(uint64_t pll_freq, enum si5351_pll target_pll)
{
	struct Si5351RegSet pll_reg;
	uint8_t params[SI5351_PARAMETERS_LENGTH];

	if(target_pll == SI5351_PLLA)
	{
		pll_calc(SI5351_PLLA, pll_freq, &pll_reg, ref_correction[plla_ref_osc], 0);
	}
	else
	{
		pll_calc(SI5351_PLLB, pll_freq, &pll_reg, ref_correction[pllb_ref_osc], 0);
	}

	// Derive the register values to write
	pack_reg_set(&pll_reg, params, 0);

	// Write the parameters
	if(target_pll == SI5351_PLLA)
	{
		si5351_write_bulk(SI5351_PLLA_PARAMETERS, SI5351_PARAMETERS_LENGTH, params);
		plla_freq = pll_freq;
	}
	else if(target_pll == SI5351_PLLB)
	{
		si5351_write_bulk(SI5351_PLLB_PARAMETERS, SI5351_PARAMETERS_LENGTH, params);
		pllb_freq = pll_freq;
	}
}

/*
 * set_ms(enum si5351_clock clk, struct Si5351RegSet ms_reg, uint8_t int_mode, uint8_t r_div, uint8_t div_by_4)
 *
 * Set the specified multisynth parameters. Not normally needed, but public for advanced users.
 *
 * clk - Clock output
 *   (use the si5351_clock enum)
 * int_mode - Set integer mode
 *  Set to 1 to enable, 0 to disable
 * r_div - Desired r_div ratio
 * div_by_4 - Set Divide By 4 mode
 *   Set to 1 to enable, 0 to disable
 */
void Si5351::set_ms(enum si5351_clock clk, struct Si5351RegSet ms_reg, uint8_t int_mode, uint8_t r_div, uint8_t div_by_4)
{
	uint8_t params[SI5351_PARAMETERS_LENGTH];

	if((uint8_t)clk <= (uint8_t)SI5351_CLK5)
	{
		// Preserve non-p1 bits in the p1[17:16] parameter byte
		uint8_t reg_val = si5351_read((SI5351_CLK0_PARAMETERS + 2) + ((uint8_t)clk * 8));
		// MS currently in DIVBY4 mode and being switched out of it? (#65)
		uint8_t leaving_divby4 = ((reg_val & SI5351_OUTPUT_CLK_DIVBY4) == SI5351_OUTPUT_CLK_DIVBY4) && !div_by_4;
		reg_val &= ~(0x03);
		pack_reg_set(&ms_reg, params, reg_val);

		// CLK0..CLK5 parameter blocks are linear: base + clk*8
		si5351_write_bulk(SI5351_CLK0_PARAMETERS + ((uint8_t)clk * 8),
			SI5351_PARAMETERS_LENGTH, params);
		set_int(clk, int_mode);
		ms_div(clk, r_div, div_by_4);

		// A Multisynth leaving DIVBY4 mode produces no output until its PLL is
		// reset (issue #65, e.g. 156 MHz -> 80 MHz). Reset only on that
		// transition so normal tuning stays glitch-free. reset() leaves the
		// DIVBY4 bits set, so this also covers reset() followed by set_freq().
		if(leaving_divby4 && !defer_pll_reset)
		{
			pll_reset(pll_assignment[clk]);
		}
	}
	else if((uint8_t)clk <= (uint8_t)SI5351_CLK7)
	{
		// MS6 and MS7 only use one register (90 / 91).
		// Bounded to CLK7 so an out-of-range clk writes nothing, as the
		// original switch() did (otherwise clk=8 would clobber reg 92).
		si5351_write(SI5351_CLK6_PARAMETERS + ((uint8_t)clk - (uint8_t)SI5351_CLK6),
			(uint8_t)ms_reg.p1);
		ms_div(clk, r_div, div_by_4);
	}
}

/*
 * output_enable(enum si5351_clock clk, uint8_t enable)
 *
 * Enable or disable a chosen output
 * clk - Clock output
 *   (use the si5351_clock enum)
 * enable - Set to 1 to enable, 0 to disable
 */
void Si5351::output_enable(enum si5351_clock clk, uint8_t enable)
{
  uint8_t reg_val;

  reg_val = si5351_read(SI5351_OUTPUT_ENABLE_CTRL);

  if(enable == 1)
  {
    reg_val &= ~(1<<(uint8_t)clk);
  }
  else
  {
    reg_val |= (1<<(uint8_t)clk);
  }

  si5351_write(SI5351_OUTPUT_ENABLE_CTRL, reg_val);
}

/*
 * drive_strength(enum si5351_clock clk, enum si5351_drive drive)
 *
 * Sets the drive strength of the specified clock output
 *
 * clk - Clock output
 *   (use the si5351_clock enum)
 * drive - Desired drive level
 *   (use the si5351_drive enum)
 */
void Si5351::drive_strength(enum si5351_clock clk, enum si5351_drive drive)
{
  uint8_t reg_val;
  const uint8_t mask = 0x03;

  reg_val = si5351_read(SI5351_CLK0_CTRL + (uint8_t)clk);
  reg_val &= ~(mask);

  switch(drive)
  {
  case SI5351_DRIVE_2MA:
    reg_val |= 0x00;
    break;
  case SI5351_DRIVE_4MA:
   reg_val |= 0x01;
    break;
  case SI5351_DRIVE_6MA:
    reg_val |= 0x02;
    break;
  case SI5351_DRIVE_8MA:
    reg_val |= 0x03;
    break;
  default:
    break;
  }

  si5351_write(SI5351_CLK0_CTRL + (uint8_t)clk, reg_val);
}

/*
 * update_status(void)
 *
 * Call this to update the status structs, then access them
 * via the dev_status and dev_int_status global members.
 *
 * See the header file for the struct definitions. These
 * correspond to the flag names for registers 0 and 1 in
 * the Si5351 datasheet.
 */
void Si5351::update_status(void)
{
	update_sys_status(&dev_status);
	update_int_status(&dev_int_status);
}

/*
 * set_correction(int32_t corr, enum si5351_pll_input ref_osc)
 *
 * corr - Correction factor in ppb
 * ref_osc - Desired reference oscillator
 *     (use the si5351_pll_input enum)
 *
 * Use this to set the oscillator correction factor.
 * This value is a signed 32-bit integer of the
 * parts-per-billion value that the actual oscillation
 * frequency deviates from the specified frequency.
 *
 * The frequency calibration is done as a one-time procedure.
 * Any desired test frequency within the normal range of the
 * Si5351 should be set, then the actual output frequency
 * should be measured as accurately as possible. The
 * difference between the measured and specified frequencies
 * should be calculated in Hertz, then multiplied by 10 in
 * order to get the parts-per-billion value.
 *
 * Since the Si5351 itself has an intrinsic 0 PPM error, this
 * correction factor is good across the entire tuning range of
 * the Si5351. Once this calibration is done accurately, it
 * should not have to be done again for the same Si5351 and
 * crystal.
 */
void Si5351::set_correction(int32_t corr, enum si5351_pll_input ref_osc)
{
	ref_correction[(uint8_t)ref_osc] = corr;

	// Recalculate and set PLL freqs based on correction value
	set_pll(plla_freq, SI5351_PLLA);
	set_pll(pllb_freq, SI5351_PLLB);
}

/*
 * set_phase(enum si5351_clock clk, uint8_t phase)
 *
 * clk - Clock output
 *   (use the si5351_clock enum)
 * phase - 7-bit phase word
 *   (in units of VCO/4 period)
 *
 * Write the 7-bit phase register. This must be used
 * with a user-set PLL frequency so that the user can
 * calculate the proper tuning word based on the PLL period.
 */
void Si5351::set_phase(enum si5351_clock clk, uint8_t phase)
{
	// Only CLK0-5 have a phase offset register (165-170); 171+ are reserved
	if((uint8_t)clk > (uint8_t)SI5351_CLK5)
	{
		return;
	}

	// Mask off the upper bit since it is reserved
	phase = phase & 0b01111111;

	si5351_write(SI5351_CLK0_PHASE_OFFSET + (uint8_t)clk, phase);
}

/*
 * get_correction(enum si5351_pll_input ref_osc)
 *
 * ref_osc - Desired reference oscillator
 *     0: crystal oscillator (XO)
 *     1: external clock input (CLKIN)
 *
 * Returns the oscillator correction factor stored
 * in RAM.
 */
int32_t Si5351::get_correction(enum si5351_pll_input ref_osc)
{
	return ref_correction[(uint8_t)ref_osc];
}

/*
 * pll_reset(enum si5351_pll target_pll)
 *
 * target_pll - Which PLL to reset
 *     (use the si5351_pll enum)
 *
 * Apply a reset to the indicated PLL.
 */
void Si5351::pll_reset(enum si5351_pll target_pll)
{
	if(target_pll == SI5351_PLLA)
 	{
    	si5351_write(SI5351_PLL_RESET, SI5351_PLL_RESET_A);
	}
	else if(target_pll == SI5351_PLLB)
	{
	    si5351_write(SI5351_PLL_RESET, SI5351_PLL_RESET_B);
	}
}

/*
 * set_ms_source(enum si5351_clock clk, enum si5351_pll pll)
 *
 * clk - Clock output
 *   (use the si5351_clock enum)
 * pll - Which PLL to use as the source
 *     (use the si5351_pll enum)
 *
 * Set the desired PLL source for a multisynth.
 */
void Si5351::set_ms_source(enum si5351_clock clk, enum si5351_pll pll)
{
	uint8_t reg_val;

	reg_val = si5351_read(SI5351_CLK0_CTRL + (uint8_t)clk);

	if(pll == SI5351_PLLA)
	{
		reg_val &= ~(SI5351_CLK_PLL_SELECT);
	}
	else if(pll == SI5351_PLLB)
	{
		reg_val |= SI5351_CLK_PLL_SELECT;
	}

	si5351_write(SI5351_CLK0_CTRL + (uint8_t)clk, reg_val);

	pll_assignment[(uint8_t)clk] = pll;
}

/*
 * set_int(enum si5351_clock clk, uint8_t int_mode)
 *
 * clk - Clock output
 *   (use the si5351_clock enum)
 * enable - Set to 1 to enable, 0 to disable
 *
 * Set the indicated multisynth into integer mode.
 */
void Si5351::set_int(enum si5351_clock clk, uint8_t enable)
{
	uint8_t reg_val;

	// MS6/7 are always integer and have no MSx_INT bit: bit 6 of reg 22/23
	// is FBA_INT/FBB_INT (PLLA/PLLB feedback integer mode), so do nothing
	if((uint8_t)clk > (uint8_t)SI5351_CLK5)
	{
		return;
	}
	reg_val = si5351_read(SI5351_CLK0_CTRL + (uint8_t)clk);

	if(enable == 1)
	{
		reg_val |= (SI5351_CLK_INTEGER_MODE);
	}
	else
	{
		reg_val &= ~(SI5351_CLK_INTEGER_MODE);
	}

	si5351_write(SI5351_CLK0_CTRL + (uint8_t)clk, reg_val);

	// Integer mode indication
	/*
	switch(clk)
	{
	case SI5351_CLK0:
		clk0_int_mode = enable;
		break;
	case SI5351_CLK1:
		clk1_int_mode = enable;
		break;
	case SI5351_CLK2:
		clk2_int_mode = enable;
		break;
	default:
		break;
	}
	*/
}

/*
 * set_clock_pwr(enum si5351_clock clk, uint8_t pwr)
 *
 * clk - Clock output
 *   (use the si5351_clock enum)
 * pwr - Set to 1 to enable, 0 to disable
 *
 * Enable or disable power to a clock output (a power
 * saving feature).
 */
void Si5351::set_clock_pwr(enum si5351_clock clk, uint8_t pwr)
{
	uint8_t reg_val; //, reg;
	reg_val = si5351_read(SI5351_CLK0_CTRL + (uint8_t)clk);

	if(pwr == 1)
	{
		reg_val &= 0b01111111;
	}
	else
	{
		reg_val |= 0b10000000;
	}

	si5351_write(SI5351_CLK0_CTRL + (uint8_t)clk, reg_val);
}

/*
 * set_clock_invert(enum si5351_clock clk, uint8_t inv)
 *
 * clk - Clock output
 *   (use the si5351_clock enum)
 * inv - Set to 1 to enable, 0 to disable
 *
 * Enable to invert the clock output waveform.
 */
void Si5351::set_clock_invert(enum si5351_clock clk, uint8_t inv)
{
	uint8_t reg_val;
	reg_val = si5351_read(SI5351_CLK0_CTRL + (uint8_t)clk);

	if(inv == 1)
	{
		reg_val |= (SI5351_CLK_INVERT);
	}
	else
	{
		reg_val &= ~(SI5351_CLK_INVERT);
	}

	si5351_write(SI5351_CLK0_CTRL + (uint8_t)clk, reg_val);
}

/*
 * set_clock_source(enum si5351_clock clk, enum si5351_clock_source src)
 *
 * clk - Clock output
 *   (use the si5351_clock enum)
 * src - Which clock source to use for the multisynth
 *   (use the si5351_clock_source enum)
 *
 * Set the clock source for a multisynth (based on the options
 * presented for Registers 16-23 in the Silicon Labs AN619 document).
 * Choices are XTAL, CLKIN, MS0, or the multisynth associated with
 * the clock output.
 */
void Si5351::set_clock_source(enum si5351_clock clk, enum si5351_clock_source src)
{
	uint8_t reg_val;
	reg_val = si5351_read(SI5351_CLK0_CTRL + (uint8_t)clk);

	// Clear the bits first
	reg_val &= ~(SI5351_CLK_INPUT_MASK);

	switch(src)
	{
	case SI5351_CLK_SRC_XTAL:
		reg_val |= (SI5351_CLK_INPUT_XTAL);
		break;
	case SI5351_CLK_SRC_CLKIN:
		reg_val |= (SI5351_CLK_INPUT_CLKIN);
		break;
	case SI5351_CLK_SRC_MS0:
		if(clk == SI5351_CLK0)
		{
			return;
		}

		reg_val |= (SI5351_CLK_INPUT_MULTISYNTH_0_4);
		break;
	case SI5351_CLK_SRC_MS:
		reg_val |= (SI5351_CLK_INPUT_MULTISYNTH_N);
		break;
	default:
		return;
	}

	si5351_write(SI5351_CLK0_CTRL + (uint8_t)clk, reg_val);
}

/*
 * set_clock_disable(enum si5351_clock clk, enum si5351_clock_disable dis_state)
 *
 * clk - Clock output
 *   (use the si5351_clock enum)
 * dis_state - Desired state of the output upon disable
 *   (use the si5351_clock_disable enum)
 *
 * Set the state of the clock output when it is disabled. Per page 27
 * of AN619 (Registers 24 and 25), there are four possible values: low,
 * high, high impedance, and never disabled.
 */
void Si5351::set_clock_disable(enum si5351_clock clk, enum si5351_clock_disable dis_state)
{
	uint8_t reg_val, reg;

	if (clk >= SI5351_CLK0 && clk <= SI5351_CLK3)
	{
		reg = SI5351_CLK3_0_DISABLE_STATE;
	}
	else if(clk >= SI5351_CLK4 && clk <= SI5351_CLK7)
	{
		reg = SI5351_CLK7_4_DISABLE_STATE;
	}
	else return;

	reg_val = si5351_read(reg);

	if (clk >= SI5351_CLK0 && clk <= SI5351_CLK3)
	{
		reg_val &= ~(0b11 << (clk * 2));
		reg_val |= dis_state << (clk * 2);
	}
	else if(clk >= SI5351_CLK4 && clk <= SI5351_CLK7)
	{
		reg_val &= ~(0b11 << ((clk - 4) * 2));
		reg_val |= dis_state << ((clk - 4) * 2);
	}

	si5351_write(reg, reg_val);
}

/*
 * set_clock_fanout(enum si5351_clock_fanout fanout, uint8_t enable)
 *
 * fanout - Desired clock fanout
 *   (use the si5351_clock_fanout enum)
 * enable - Set to 1 to enable, 0 to disable
 *
 * Use this function to enable or disable the clock fanout options
 * for individual clock outputs. If you intend to output the XO or
 * CLKIN on the clock outputs, enable this first.
 *
 * By default, only the Multisynth fanout is enabled at startup.
 */
void Si5351::set_clock_fanout(enum si5351_clock_fanout fanout, uint8_t enable)
{
	uint8_t reg_val;
	reg_val = si5351_read(SI5351_FANOUT_ENABLE);

	switch(fanout)
	{
	case SI5351_FANOUT_CLKIN:
		if(enable)
		{
			reg_val |= SI5351_CLKIN_ENABLE;
		}
		else
		{
			reg_val &= ~(SI5351_CLKIN_ENABLE);
		}
		break;
	case SI5351_FANOUT_XO:
		if(enable)
		{
			reg_val |= SI5351_XTAL_ENABLE;
		}
		else
		{
			reg_val &= ~(SI5351_XTAL_ENABLE);
		}
		break;
	case SI5351_FANOUT_MS:
		if(enable)
		{
			reg_val |= SI5351_MULTISYNTH_ENABLE;
		}
		else
		{
			reg_val &= ~(SI5351_MULTISYNTH_ENABLE);
		}
		break;
	}

	si5351_write(SI5351_FANOUT_ENABLE, reg_val);
}

/*
 * set_pll_input(enum si5351_pll pll, enum si5351_pll_input input)
 *
 * pll - Which PLL to use as the source
 *     (use the si5351_pll enum)
 * input - Which reference oscillator to use as PLL input
 *     (use the si5351_pll_input enum)
 *
 * Set the desired reference oscillator source for the given PLL.
 */
void Si5351::set_pll_input(enum si5351_pll pll, enum si5351_pll_input input)
{
	uint8_t reg_val;
	reg_val = si5351_read(SI5351_PLL_INPUT_SOURCE);

	// CLKIN_DIV[1:0] is a 2-bit field shared by both PLLs: replace it (not
	// OR into it) when a PLL is switched to CLKIN, leave it alone for XO

	switch(pll)
	{
	case SI5351_PLLA:
		if(input == SI5351_PLL_INPUT_CLKIN)
		{
			reg_val |= SI5351_PLLA_SOURCE;
			reg_val = (reg_val & ~(SI5351_CLKIN_DIV_MASK)) | clkin_div;
			plla_ref_osc = SI5351_PLL_INPUT_CLKIN;
		}
		else
		{
			reg_val &= ~(SI5351_PLLA_SOURCE);
			plla_ref_osc = SI5351_PLL_INPUT_XO;
		}
		break;
	case SI5351_PLLB:
		if(input == SI5351_PLL_INPUT_CLKIN)
		{
			reg_val |= SI5351_PLLB_SOURCE;
			reg_val = (reg_val & ~(SI5351_CLKIN_DIV_MASK)) | clkin_div;
			pllb_ref_osc = SI5351_PLL_INPUT_CLKIN;
		}
		else
		{
			reg_val &= ~(SI5351_PLLB_SOURCE);
			pllb_ref_osc = SI5351_PLL_INPUT_XO;
		}
		break;
	default:
		return;
	}

	si5351_write(SI5351_PLL_INPUT_SOURCE, reg_val);

	set_pll(plla_freq, SI5351_PLLA);
	set_pll(pllb_freq, SI5351_PLLB);
}

/*
 * set_vcxo(uint64_t pll_freq, uint8_t ppm)
 *
 * pll_freq - Desired PLL base frequency in Hz * 100
 * ppm - VCXO pull limit in ppm
 *
 * Set the parameters for the VCXO on the Si5351B.
 */
void Si5351::set_vcxo(uint64_t pll_freq, uint8_t ppm)
{
	struct Si5351RegSet pll_reg;
	uint64_t vcxo_param;

	// Bounds check
	if(ppm < SI5351_VCXO_PULL_MIN)
	{
		ppm = SI5351_VCXO_PULL_MIN;
	}

	if(ppm > SI5351_VCXO_PULL_MAX)
	{
		ppm = SI5351_VCXO_PULL_MAX;
	}

	// Set PLLB params
	vcxo_param = pll_calc(SI5351_PLLB, pll_freq, &pll_reg, ref_correction[pllb_ref_osc], 1);

	// Derive the register values to write
	uint8_t params[SI5351_PARAMETERS_LENGTH];
	uint8_t temp;

	pack_reg_set(&pll_reg, params, 0);

	// Write the parameters
	si5351_write_bulk(SI5351_PLLB_PARAMETERS, SI5351_PARAMETERS_LENGTH, params);

	// Write the VCXO parameters
	vcxo_param = ((vcxo_param * ppm * SI5351_VCXO_MARGIN) / 100ULL) / 1000000ULL;

	temp = (uint8_t)(vcxo_param & 0xFF);
	si5351_write(SI5351_VXCO_PARAMETERS_LOW, temp);

	temp = (uint8_t)((vcxo_param >> 8) & 0xFF);
	si5351_write(SI5351_VXCO_PARAMETERS_MID, temp);

	temp = (uint8_t)((vcxo_param >> 16) & 0x3F);
	si5351_write(SI5351_VXCO_PARAMETERS_HIGH, temp);
}

/*
 * set_ref_freq(uint32_t ref_freq, enum si5351_pll_input ref_osc)
 *
 * ref_freq - Reference oscillator frequency in Hz
 * ref_osc - Which reference oscillator frequency to set
 *    (use the si5351_pll_input enum)
 *
 * Set the reference frequency value for the desired reference oscillator
 */
void Si5351::set_ref_freq(uint32_t ref_freq, enum si5351_pll_input ref_osc)
{
	// uint8_t reg_val;
	//reg_val = si5351_read(SI5351_PLL_INPUT_SOURCE);

	// Clear the bits first
	//reg_val &= ~(SI5351_CLKIN_DIV_MASK);

	if(ref_freq <= 30000000UL)
	{
		xtal_freq[(uint8_t)ref_osc] = ref_freq;
		//reg_val |= SI5351_CLKIN_DIV_1;
		if(ref_osc == SI5351_PLL_INPUT_CLKIN)
		{
			clkin_div = SI5351_CLKIN_DIV_1;
		}
	}
	else if(ref_freq > 30000000UL && ref_freq <= 60000000UL)
	{
		xtal_freq[(uint8_t)ref_osc] = ref_freq / 2;
		//reg_val |= SI5351_CLKIN_DIV_2;
		if(ref_osc == SI5351_PLL_INPUT_CLKIN)
		{
			clkin_div = SI5351_CLKIN_DIV_2;
		}
	}
	else if(ref_freq > 60000000UL && ref_freq <= 100000000UL)
	{
		xtal_freq[(uint8_t)ref_osc] = ref_freq / 4;
		//reg_val |= SI5351_CLKIN_DIV_4;
		if(ref_osc == SI5351_PLL_INPUT_CLKIN)
		{
			clkin_div = SI5351_CLKIN_DIV_4;
		}
	}
	else
	{
		//reg_val |= SI5351_CLKIN_DIV_1;
	}

	//si5351_write(SI5351_PLL_INPUT_SOURCE, reg_val);
}

uint8_t Si5351::si5351_write_bulk(uint8_t addr, uint8_t bytes, uint8_t *data)
{
	_wire->beginTransmission(i2c_bus_addr);
	_wire->write(addr);
	for(int i = 0; i < bytes; i++)
	{
		_wire->write(data[i]);
	}
	return _wire->endTransmission();

}

uint8_t Si5351::si5351_write(uint8_t addr, uint8_t data)
{
	_wire->beginTransmission(i2c_bus_addr);
	_wire->write(addr);
	_wire->write(data);
	return _wire->endTransmission();
}

uint8_t Si5351::si5351_read(uint8_t addr)
{
	uint8_t reg_val = 0;

	_wire->beginTransmission(i2c_bus_addr);
	_wire->write(addr);
	_wire->endTransmission();

	_wire->requestFrom(i2c_bus_addr, (uint8_t)1);

	while(_wire->available())
	{
		reg_val = _wire->read();
	}

	return reg_val;
}

/*********************/
/* Private functions */
/*********************/

uint64_t Si5351::pll_calc(enum si5351_pll pll, uint64_t freq, struct Si5351RegSet *reg, int32_t correction, uint8_t vcxo)
{
	uint64_t ref_freq;
	if(pll == SI5351_PLLA)
	{
		ref_freq = xtal_freq[(uint8_t)plla_ref_osc] * SI5351_FREQ_MULT;
	}
	else
	{
		ref_freq = xtal_freq[(uint8_t)pllb_ref_osc] * SI5351_FREQ_MULT;
	}
	//ref_freq = 15974400ULL * SI5351_FREQ_MULT;
	uint32_t a, b, c;
	uint64_t lltmp; //, denom;

	// Factor calibration value into nominal crystal frequency
	// Measured in parts-per-billion

	ref_freq = ref_freq + (int32_t)((((((int64_t)correction) << 31) / 1000000000LL) * ref_freq) >> 31);

	// PLL bounds checking
	if (freq < SI5351_PLL_VCO_MIN * SI5351_FREQ_MULT)
	{
		freq = SI5351_PLL_VCO_MIN * SI5351_FREQ_MULT;
	}
	if (freq > SI5351_PLL_VCO_MAX * SI5351_FREQ_MULT)
	{
		freq = SI5351_PLL_VCO_MAX * SI5351_FREQ_MULT;
	}

	// Determine integer part of feedback equation
	a = freq / ref_freq;

	if (a < SI5351_PLL_A_MIN)
	{
		freq = ref_freq * SI5351_PLL_A_MIN;
	}
	if (a > SI5351_PLL_A_MAX)
	{
		freq = ref_freq * SI5351_PLL_A_MAX;
	}

	// Find best approximation for b/c = fVCO mod fIN
	// denom = 1000ULL * 1000ULL;
	// lltmp = freq % ref_freq;
	// lltmp *= denom;
	// do_div(lltmp, ref_freq);

	//b = (((uint64_t)(freq % ref_freq)) * RFRAC_DENOM) / ref_freq;
	if(vcxo)
	{
		b = (((uint64_t)(freq % ref_freq)) * 1000000ULL) / ref_freq;
		c = 1000000ULL;
	}
	else
	{
		b = (((uint64_t)(freq % ref_freq)) * RFRAC_DENOM) / ref_freq;
		c = b ? RFRAC_DENOM : 1;
	}

	// Calculate parameters
	frac_to_reg(a, b, c, reg);

	// Recalculate frequency as fIN * (a + b/c)
	lltmp = ref_freq;
	lltmp *= b;
	do_div(lltmp, c);
	freq = lltmp;
	freq += ref_freq * a;

	if(vcxo)
	{
		return (uint64_t)(128 * a * 1000000ULL + b);
	}
	else
	{
		return freq;
	}
}

uint64_t Si5351::multisynth_calc(uint64_t freq, uint64_t pll_freq, struct Si5351RegSet *reg)
{
	uint64_t lltmp;
	uint32_t a, b, c;
	uint8_t divby4 = 0;
	uint8_t ret_val = 0;

	// Multisynth bounds checking
	if (freq > SI5351_FOUT_MAX * SI5351_FREQ_MULT)
	{
		freq = SI5351_FOUT_MAX * SI5351_FREQ_MULT;
	}
	if (freq < SI5351_MULTISYNTH_MIN_FREQ * SI5351_FREQ_MULT)
	{
		freq = SI5351_MULTISYNTH_MIN_FREQ * SI5351_FREQ_MULT;
	}

	if (freq >= SI5351_MULTISYNTH_DIVBY4_FREQ * SI5351_FREQ_MULT)
	{
		divby4 = 1;
	}

	if(pll_freq == 0)
	{
		// Find largest integer divider for max
		// VCO frequency and given target frequency
		if(divby4 == 0)
		{
			lltmp = SI5351_PLL_VCO_MAX * SI5351_FREQ_MULT; // margin needed?
			do_div(lltmp, freq);
			if(lltmp == 5)
			{
				lltmp = 4;
			}
			else if(lltmp == 7)
			{
				lltmp = 6;
			}
			a = (uint32_t)lltmp;
		}
		else
		{
			a = 4;
		}

		b = 0;
		c = 1;
		pll_freq = a * freq;
	}
	else
	{
		// Preset PLL, so return the actual freq for these params instead of PLL freq
		ret_val = 1;

		// Determine integer part of feedback equation
		a = pll_freq / freq;

		if (a < SI5351_MULTISYNTH_A_MIN)
		{
			// Clamp to ratio 6; a must follow or ratio 5 is written (DEV-05)
			a = SI5351_MULTISYNTH_A_MIN;
			freq = pll_freq / SI5351_MULTISYNTH_A_MIN;
		}
		if (a > SI5351_MULTISYNTH_A_MAX)
		{
			freq = pll_freq / SI5351_MULTISYNTH_A_MAX;
		}

		b = (pll_freq % freq * RFRAC_DENOM) / freq;
		c = b ? RFRAC_DENOM : 1;
	}

	// Calculate parameters
	if (divby4 == 1)
	{
		reg->p1 = 0;
		reg->p2 = 0;
		reg->p3 = 1;
	}
	else
	{
		frac_to_reg(a, b, c, reg);
	}

	if(ret_val == 0)
	{
		return pll_freq;
	}
	else
	{
		return freq;
	}
}

uint64_t Si5351::multisynth67_calc(uint64_t freq, uint64_t pll_freq, struct Si5351RegSet *reg)
{
	//uint8_t p1;
	// uint8_t ret_val = 0;
	uint32_t a;
	uint64_t lltmp;

	// Multisynth bounds checking
	if(freq > SI5351_MULTISYNTH67_MAX_FREQ * SI5351_FREQ_MULT)
	{
		freq = SI5351_MULTISYNTH67_MAX_FREQ * SI5351_FREQ_MULT;
	}
	if(freq < SI5351_MULTISYNTH_MIN_FREQ * SI5351_FREQ_MULT)
	{
		freq = SI5351_MULTISYNTH_MIN_FREQ * SI5351_FREQ_MULT;
	}

	if(pll_freq == 0)
	{
		// Find largest integer divider for max
		// VCO frequency and given target frequency
		lltmp = (SI5351_PLL_VCO_MAX * SI5351_FREQ_MULT) - 100000000UL; // margin needed?
		do_div(lltmp, freq);
		a = (uint32_t)lltmp;

		// Divisor has to be even
		if(a % 2 != 0)
		{
			a++;
		}

		// Divisor bounds check
		if(a < SI5351_MULTISYNTH_A_MIN)
		{
			a = SI5351_MULTISYNTH_A_MIN;
		}
		if(a > SI5351_MULTISYNTH67_A_MAX)
		{
			a = SI5351_MULTISYNTH67_A_MAX;
		}

		pll_freq = a * freq;

		// PLL bounds checking
		if(pll_freq > (SI5351_PLL_VCO_MAX * SI5351_FREQ_MULT))
		{
			a -= 2;
			pll_freq = a * freq;
		}
		else if(pll_freq < (SI5351_PLL_VCO_MIN * SI5351_FREQ_MULT))
		{
			a += 2;
			pll_freq = a * freq;
		}

		reg->p1 = (uint8_t)a;
		reg->p2 = 0;
		reg->p3 = 0;
		return pll_freq;
	}
	else
	{
		// Multisynth frequency must be integer division of PLL
		if(pll_freq % freq)
		{
			// No good
			return 0;
		}
		else
		{
			a = pll_freq / freq;

			// Division ratio bounds check
			if(a < SI5351_MULTISYNTH_A_MIN || a > SI5351_MULTISYNTH67_A_MAX)
			{
				// No bueno
				return 0;
			}
			else
			{
				reg->p1 = (uint8_t)a;
				reg->p2 = 0;
				reg->p3 = 0;
				return 1;
			}
		}
	}
}

void Si5351::update_sys_status(struct Si5351Status *status)
{
  uint8_t reg_val = 0;

  reg_val = si5351_read(SI5351_DEVICE_STATUS);

  // Parse the register
  status->SYS_INIT = (reg_val >> 7) & 0x01;
  status->LOL_B = (reg_val >> 6) & 0x01;
  status->LOL_A = (reg_val >> 5) & 0x01;
  status->LOS = (reg_val >> 4) & 0x01;
  status->REVID = reg_val & 0x03;
}

void Si5351::update_int_status(struct Si5351IntStatus *int_status)
{
  uint8_t reg_val = 0;

  reg_val = si5351_read(SI5351_INTERRUPT_STATUS);

  // Parse the register
  int_status->SYS_INIT_STKY = (reg_val >> 7) & 0x01;
  int_status->LOL_B_STKY = (reg_val >> 6) & 0x01;
  int_status->LOL_A_STKY = (reg_val >> 5) & 0x01;
  int_status->LOS_STKY = (reg_val >> 4) & 0x01;
}

void Si5351::ms_div(enum si5351_clock clk, uint8_t r_div, uint8_t div_by_4)
{
	uint8_t reg_val = 0;
	uint8_t reg_addr = 0;

	if((uint8_t)clk <= (uint8_t)SI5351_CLK5)
	{
		// CLK0..CLK5: parameter byte 2 is linear with clk
		reg_addr = SI5351_CLK0_PARAMETERS + 2 + ((uint8_t)clk * 8);
	}
	else
	{
		reg_addr = SI5351_CLK6_7_OUTPUT_DIVIDER;
	}

	reg_val = si5351_read(reg_addr);

	if((uint8_t)clk <= (uint8_t)SI5351_CLK5)
	{
		// Clear the relevant bits
		reg_val &= ~(0x7c);

		if(div_by_4 == 0)
		{
			reg_val &= ~(SI5351_OUTPUT_CLK_DIVBY4);
		}
		else
		{
			reg_val |= (SI5351_OUTPUT_CLK_DIVBY4);
		}

		reg_val |= (r_div << SI5351_OUTPUT_CLK_DIV_SHIFT);
	}
	else if(clk == SI5351_CLK6)
	{
		// Clear the relevant bits
		reg_val &= ~(0x07);

		reg_val |= r_div;
	}
	else if(clk == SI5351_CLK7)
	{
		// Clear the relevant bits
		reg_val &= ~(0x70);

		reg_val |= (r_div << SI5351_OUTPUT_CLK_DIV_SHIFT);
	}

	si5351_write(reg_addr, reg_val);
}

uint8_t Si5351::select_r_div(uint64_t *freq, uint32_t min_freq)
{
	/* Unified R-divider selection for CLK0-5 (min_freq = CLKOUT_MIN_FREQ)
	 * and CLK6-7 (min_freq = CLKOUT67_MIN_FREQ). Same band thresholds as
	 * the former select_r_div / select_r_div_ms67 pair.
	 * SI5351_OUTPUT_CLK_DIV_n enum values equal log2(n). */
	uint64_t lower = (uint64_t)min_freq * SI5351_FREQ_MULT;
	int8_t shift;

	for(shift = 7; shift >= 1; shift--)
	{
		uint64_t upper = lower << 1;
		if((*freq >= lower) && (*freq < upper))
		{
			*freq *= (1ULL << shift);
			return (uint8_t)shift;
		}
		lower = upper;
	}

	return SI5351_OUTPUT_CLK_DIV_1;
}
