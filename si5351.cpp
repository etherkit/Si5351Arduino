/*
 * si5351.cpp - Si5351 library for Arduino
 *
 * Copyright (C) 2014 Jason Milldrum <milldrum@gmail.com>
 *
 * Some tuning algorithms derived from clk-si5351.c in the Linux kernel.
 * Sebastian Hesselbarth <sebastian.hesselbarth@gmail.com>
 * Rabeeh Khoury <rabeeh@solid-run.com>
 *
 * rational_best_approximation() derived from lib/rational.c in
 * the Linux kernel.
 * Copyright (C) 2009 emlix GmbH, Oskar Schirmer <oskar@scara.com>
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
#include <avr/eeprom.h>

#include "Arduino.h"
#include "Wire.h"
#include "si5351.h"

uint32_t EEMEM ee_ref_correction = 0;

/********************/
/* Public functions */
/********************/

Si5351::Si5351(void)
{
}

/*
 * init(uint8_t xtal_load_c)
 *
 * Setup communications to the Si5351 and set the crystal
 * load capacitance.
 *
 * xtal_load_c - Crystal load capacitance. Use the SI5351_CRYSTAL_LOAD_*PF
 * defines in the header file
 *
 */
void Si5351::init(uint8_t xtal_load_c)
{
	// Start I2C comms
	Wire.begin();

	// Set crystal load capacitance
	si5351_write(SI5351_CRYSTAL_LOAD, xtal_load_c);

	// Get the correction factor from EEPROM
	ref_correction = eeprom_read_dword(&ee_ref_correction);
}

/*
 * set_freq(uint64_t freq, uint64_t pll_freq, enum si5351_clock output)
 *
 * Sets the clock frequency of the specified CLK output
 *
 * freq - Output frequency in Hz
 * pll_freq - Frequency of the PLL driving the Multisynth
 *   Use a 0 to have the function choose a PLL frequency
 * clk - Clock output
 *   (use the si5351_clock enum)
 */
void Si5351::set_freq(uint64_t freq, uint64_t pll_freq, enum si5351_clock clk)
{
	struct Si5351RegSet ms_reg, pll_reg;
	enum si5351_pll target_pll;
	uint8_t set_pll = 0;
	uint8_t r_div = SI5351_OUTPUT_CLK_DIV_1;
	uint8_t reg_val;
	uint8_t int_mode = 0;
	uint8_t div_by_4 = 0;

	// Bounds checking and see if we need to use the R divider

	// Lower bounds check	
	if(freq < SI5351_CLKOUT_MIN_FREQ * 100ULL)
	{
		freq = SI5351_CLKOUT_MIN_FREQ * 100ULL;
	}
	
	// Choose the correct R divider
	if((freq >= SI5351_CLKOUT_MIN_FREQ * 100ULL) && (freq < SI5351_CLKOUT_MIN_FREQ * 100ULL * 2))
	{
		r_div = SI5351_OUTPUT_CLK_DIV_128;
		freq *= 128ULL;
	}
	else if((freq >= SI5351_CLKOUT_MIN_FREQ * 100ULL * 2) && (freq < SI5351_CLKOUT_MIN_FREQ * 100ULL * 4))
	{
		r_div = SI5351_OUTPUT_CLK_DIV_64;
		freq *= 64ULL;
	}
	else if((freq >= SI5351_CLKOUT_MIN_FREQ * 100ULL * 4) && (freq < SI5351_CLKOUT_MIN_FREQ * 100ULL * 8))
	{
		r_div = SI5351_OUTPUT_CLK_DIV_32;
		freq *= 32ULL;
	}
	else if((freq >= SI5351_CLKOUT_MIN_FREQ * 100ULL * 8) && (freq < SI5351_CLKOUT_MIN_FREQ * 100ULL * 16))
	{
		r_div = SI5351_OUTPUT_CLK_DIV_16;
		freq *= 16ULL;
	}
	else if((freq >= SI5351_CLKOUT_MIN_FREQ * 100ULL * 16) && (freq < SI5351_CLKOUT_MIN_FREQ * 100ULL * 32))
	{
		r_div = SI5351_OUTPUT_CLK_DIV_8;
		freq *= 8ULL;
	}
	else if((freq >= SI5351_CLKOUT_MIN_FREQ * 100ULL * 32) && (freq < SI5351_CLKOUT_MIN_FREQ * 100ULL * 64))
	{
		r_div = SI5351_OUTPUT_CLK_DIV_4;
		freq *= 4ULL;
	}
	else if((freq >= SI5351_CLKOUT_MIN_FREQ * 100ULL * 64) && (freq < SI5351_CLKOUT_MIN_FREQ * 100ULL * 128))
	{
		r_div = SI5351_OUTPUT_CLK_DIV_2;
		freq *= 2ULL;
	}
	
	// Upper bounds check
	if(freq > SI5351_MULTISYNTH_MAX_FREQ * 100ULL)
	{
		freq = SI5351_MULTISYNTH_MAX_FREQ * 100ULL;
	}
	
	// Calculate the synth parameters
	// The PLL must be calculated and set by firmware when 150 MHz <= freq <= 160 MHz
	if(freq >= SI5351_MULTISYNTH_DIVBY4_FREQ * 100ULL)
	{
		// TODO: rechecking other MS dependent on this PLL
		pll_freq = multisynth_calc(freq, &ms_reg);
		set_pll = 1;
		int_mode = 1;
		div_by_4 = 1;

	}
	// If pll_freq is 0, let the algorithm pick a PLL frequency
	else if(pll_freq == 0)
	{
		pll_freq = multisynth_calc(freq, &ms_reg);
		set_pll = 1;
	}
	else
	{
		multisynth_recalc(freq, pll_freq, &ms_reg);
		set_pll = 0;
	}

	// Determine which PLL to use
	// CLK0 gets PLLA, CLK1 gets PLLB
	// CLK2 gets PLLB if necessary
	// Only good for Si5351A3 variant at the moment
	if(clk == SI5351_CLK0)
	{
		target_pll = SI5351_PLLA;
		si5351_set_ms_source(SI5351_CLK0, SI5351_PLLA);
		plla_freq = pll_freq;
	}
	else if(clk == SI5351_CLK1)
	{
		target_pll = SI5351_PLLB;
		si5351_set_ms_source(SI5351_CLK1, SI5351_PLLB);
		pllb_freq = pll_freq;
	}
	else
	{
	// need to account for CLK2 set before CLK1
		if(pllb_freq == 0)
		{
		  target_pll = SI5351_PLLB;
		  si5351_set_ms_source(SI5351_CLK2, SI5351_PLLB);
		  pllb_freq = pll_freq;
		}
		else
		{
		  target_pll = SI5351_PLLB;
		  si5351_set_ms_source(SI5351_CLK2, SI5351_PLLB);
		  pll_freq = pllb_freq;
		  multisynth_recalc(freq, pll_freq, &ms_reg);
		}
	}

	pll_calc(pll_freq, &pll_reg, ref_correction);

	// Derive the register values to write

	// Prepare an array for parameters to be written to
	uint8_t *params = new uint8_t[20];
	memset(params, 0, 20);
	uint8_t i = 0;
	uint8_t temp;

	// PLL parameters first

	if(set_pll == 1)
	{
		// Registers 26-27 for PLLA
		temp = ((pll_reg.p3 >> 8) & 0xFF);
		params[i++] = temp;

		temp = (uint8_t)(pll_reg.p3  & 0xFF);
		params[i++] = temp;

		// Register 28 for PLLA
		temp = (uint8_t)((pll_reg.p1 >> 16) & 0x03);
		params[i++] = temp;

		// Registers 29-30 for PLLA
		temp = (uint8_t)((pll_reg.p1 >> 8) & 0xFF);
		params[i++] = temp;

		temp = (uint8_t)(pll_reg.p1  & 0xFF);
		params[i++] = temp;

		// Register 31 for PLLA
		temp = (uint8_t)((pll_reg.p3 >> 12) & 0xF0);
		temp += (uint8_t)((pll_reg.p2 >> 16) & 0x0F);
		params[i++] = temp;

		// Registers 32-33 for PLLA
		temp = (uint8_t)((pll_reg.p2 >> 8) & 0xFF);
		params[i++] = temp;

		temp = (uint8_t)(pll_reg.p2  & 0xFF);
		params[i++] = temp;

		// Write the parameters
		if(target_pll == SI5351_PLLA)
		{
		  si5351_write_bulk(SI5351_PLLA_PARAMETERS, i, params);
		}
		else if(target_pll == SI5351_PLLB)
		{
		  si5351_write_bulk(SI5351_PLLB_PARAMETERS, i, params);
		}
	}

	delete params;

	// Now the multisynth parameters
	params = new uint8_t[20];
	i = 0;

	// Registers 42-43 for CLK0
	temp = (uint8_t)((ms_reg.p3 >> 8) & 0xFF);
	params[i++] = temp;

	temp = (uint8_t)(ms_reg.p3  & 0xFF);
	params[i++] = temp;

	// Register 44 for CLK0
	temp = (uint8_t)((ms_reg.p1 >> 16) & 0x03);
	params[i++] = temp;

	// Registers 45-46 for CLK0
	temp = (uint8_t)((ms_reg.p1 >> 8) & 0xFF);
	params[i++] = temp;

	temp = (uint8_t)(ms_reg.p1  & 0xFF);
	params[i++] = temp;

	// Register 47 for CLK0
	temp = (uint8_t)((ms_reg.p3 >> 12) & 0xF0);
	temp += (uint8_t)((ms_reg.p2 >> 16) & 0x0F);
	params[i++] = temp;

	// Registers 48-49 for CLK0
	temp = (uint8_t)((ms_reg.p2 >> 8) & 0xFF);
	params[i++] = temp;

	temp = (uint8_t)(ms_reg.p2  & 0xFF);
	params[i++] = temp;

	// Write the parameters
	switch(clk)
	{
		case SI5351_CLK0:
			si5351_write_bulk(SI5351_CLK0_PARAMETERS, i, params);
			break;
		case SI5351_CLK1:
			si5351_write_bulk(SI5351_CLK1_PARAMETERS, i, params);
			break;
		case SI5351_CLK2:
			si5351_write_bulk(SI5351_CLK2_PARAMETERS, i, params);
			break;
		case SI5351_CLK3:
			si5351_write_bulk(SI5351_CLK3_PARAMETERS, i, params);
			break;
		case SI5351_CLK4:
			si5351_write_bulk(SI5351_CLK4_PARAMETERS, i, params);
			break;
		case SI5351_CLK5:
			si5351_write_bulk(SI5351_CLK5_PARAMETERS, i, params);
			break;
		case SI5351_CLK6:
			si5351_write_bulk(SI5351_CLK6_PARAMETERS, i, params);
			break;
		case SI5351_CLK7:
			si5351_write_bulk(SI5351_CLK7_PARAMETERS, i, params);
			break;
	}
	
	si5351_set_int(clk, int_mode);
	si5351_ms_div(clk, r_div, div_by_4);

	delete params;
}

/*
 * set_pll(uint64_t pll_freq, enum si5351_pll target_pll)
 *
 * Set the specified PLL to a specific oscillation frequency
 *
 * pll_freq - Desired PLL frequency
 * target_pll - Which PLL to set
 *     (use the si5351_pll enum)
 */
void Si5351::set_pll(uint64_t pll_freq, enum si5351_pll target_pll)
{
  struct Si5351RegSet pll_reg;

  pll_calc(pll_freq, &pll_reg, ref_correction);

  // Derive the register values to write 

  // Prepare an array for parameters to be written to
  uint8_t *params = new uint8_t[20];
  uint8_t i = 0;
  uint8_t temp;

  // Registers 26-27
  temp = ((pll_reg.p3 >> 8) & 0xFF);
  params[i++] = temp;

  temp = (uint8_t)(pll_reg.p3  & 0xFF);
  params[i++] = temp;

  // Register 28
  temp = (uint8_t)((pll_reg.p1 >> 16) & 0x03);
  params[i++] = temp;

  // Registers 29-30
  temp = (uint8_t)((pll_reg.p1 >> 8) & 0xFF);
  params[i++] = temp;

  temp = (uint8_t)(pll_reg.p1  & 0xFF);
  params[i++] = temp;

  // Register 31
  temp = (uint8_t)((pll_reg.p3 >> 12) & 0xF0);
  temp += (uint8_t)((pll_reg.p2 >> 16) & 0x0F);
  params[i++] = temp;

  // Registers 32-33
  temp = (uint8_t)((pll_reg.p2 >> 8) & 0xFF);
  params[i++] = temp;

  temp = (uint8_t)(pll_reg.p2  & 0xFF);
  params[i++] = temp;

  // Write the parameters
  if(target_pll == SI5351_PLLA)
  {
    si5351_write_bulk(SI5351_PLLA_PARAMETERS, i, params);
  }
  else if(target_pll == SI5351_PLLB)
  {
    si5351_write_bulk(SI5351_PLLB_PARAMETERS, i, params);
  }

  delete params;
}

/*
 * clock_enable(enum si5351_clock clk, uint8_t enable)
 *
 * Enable or disable a chosen clock
 * clk - Clock output
 *   (use the si5351_clock enum)
 * enable - Set to 1 to enable, 0 to disable
 */
void Si5351::clock_enable(enum si5351_clock clk, uint8_t enable)
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

  switch(drive)
  {
  case SI5351_DRIVE_2MA:
    reg_val &= ~(mask);
    reg_val |= 0x00;
    break;
  case SI5351_DRIVE_4MA:
    reg_val &= ~(mask);
    reg_val |= 0x01;
    break;
  case SI5351_DRIVE_6MA:
    reg_val &= ~(mask);
    reg_val |= 0x02;
    break;
  case SI5351_DRIVE_8MA:
    reg_val &= ~(mask);
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
 * via the dev_status and dev_int_status global variables.
 *
 * See the header file for the struct definitions. These
 * correspond to the flag names for registers 0 and 1 in
 * the Si5351 datasheet.
 */
void Si5351::update_status(void)
{
	si5351_update_sys_status(&dev_status);
	si5351_update_int_status(&dev_int_status);
}

/*
 * set_correction(int32_t corr)
 *
 * Use this to set the oscillator correction factor to
 * EEPROM. This value is a signed 32-bit integer of the
 * parts-per-10 million value that the actual oscillation
 * frequency deviates from the specified frequency.
 *
 * The frequency calibration is done as a one-time procedure.
 * Any desired test frequency within the normal range of the
 * Si5351 should be set, then the actual output frequency
 * should be measured as accurately as possible. The
 * difference between the measured and specified frequencies
 * should be calculated in Hertz, then multiplied by 10 in
 * order to get the parts-per-10 million value.
 *
 * Since the Si5351 itself has an intrinsic 0 PPM error, this
 * correction factor is good across the entire tuning range of
 * the Si5351. Once this calibration is done accurately, it
 * should not have to be done again for the same Si5351 and
 * crystal. The library will read the correction factor from
 * EEPROM during initialization for use by the tuning
 * algorithms.
 */
void Si5351::set_correction(int32_t corr)
{
	int32_t temp = eeprom_read_dword(&ee_ref_correction);
	if(temp != corr)
	{
		eeprom_write_dword(&ee_ref_correction, corr);
	}
	ref_correction = corr;
}

/*
 * set_phase(enum si5351_clock clk, uint8_t phase)
 */
void Si5351::set_phase(enum si5351_clock clk, uint8_t phase)
{
	si5351_write(SI5351_CLK0_PHASE_OFFSET + (uint8_t)clk, phase);
	pll_reset(SI5351_PLLA);
	pll_reset(SI5351_PLLB);
}

/*
 * get_correction(void)
 *
 * Returns the oscillator correction factor stored
 * in EEPROM.
 */
int32_t Si5351::get_correction(void)
{
	return eeprom_read_dword(&ee_ref_correction);
}

/*
 * pll_reset(enum si5351_pll target_pll)
 *
 * target_pll - Which PLL to reset
 *     (use the si5351_pll enum)
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

uint8_t Si5351::si5351_write_bulk(uint8_t addr, uint8_t bytes, uint8_t *data)
{
	Wire.beginTransmission(SI5351_BUS_BASE_ADDR);
	Wire.write(addr);
	for(int i = 0; i < bytes; i++)
	{
		Wire.write(data[i]);
	}
	Wire.endTransmission();
}

uint8_t Si5351::si5351_write(uint8_t addr, uint8_t data)
{
	Wire.beginTransmission(SI5351_BUS_BASE_ADDR);
	Wire.write(addr);
	Wire.write(data);
	Wire.endTransmission();
}

uint8_t Si5351::si5351_read(uint8_t addr)
{
	Wire.beginTransmission(SI5351_BUS_BASE_ADDR);
	Wire.write(addr);
	Wire.endTransmission();

	Wire.requestFrom(SI5351_BUS_BASE_ADDR, 1);

	return Wire.read();
}

void Si5351::si5351_set_ms_source(enum si5351_clock clk, enum si5351_pll pll)
{
  uint8_t reg_val = 0x0c;
  uint8_t reg_val2;

  reg_val2 = si5351_read(SI5351_CLK0_CTRL + (uint8_t)clk);

  if(pll == SI5351_PLLA)
  {
    reg_val &= ~(SI5351_CLK_PLL_SELECT);
  }
  else if(pll == SI5351_PLLB)
  {
    reg_val |= SI5351_CLK_PLL_SELECT;
  }
  si5351_write(SI5351_CLK0_CTRL + (uint8_t)clk, reg_val);
}

void Si5351::si5351_set_int(enum si5351_clock clk, uint8_t int_mode)
{
	uint8_t reg_val;
	reg_val = si5351_read(SI5351_CLK0_CTRL + clk);
	
	if(int_mode == 1)
	{
		reg_val |= (SI5351_CLK_INTEGER_MODE);
	}
	else
	{
		reg_val &= ~(SI5351_CLK_INTEGER_MODE);
	}
	
	si5351_write(SI5351_CLK0_CTRL + clk, reg_val);
}

/*********************/
/* Private functions */
/*********************/

/*
 * Calculate best rational approximation for a given fraction
 * taking into account restricted register size, e.g. to find
 * appropriate values for a pll with 5 bit denominator and
 * 8 bit numerator register fields, trying to set up with a
 * frequency ratio of 3.1415, one would say:
 *
 * rational_best_approximation(31415, 10000,
 *              (1 << 8) - 1, (1 << 5) - 1, &n, &d);
 *
 * you may look at given_numerator as a fixed point number,
 * with the fractional part size described in given_denominator.
 *
 * for theoretical background, see:
 * http://en.wikipedia.org/wiki/Continued_fraction
 */

void Si5351::rational_best_approximation(
        unsigned long long given_numerator, unsigned long long given_denominator,
        unsigned long long max_numerator, unsigned long long max_denominator,
        unsigned long *best_numerator, unsigned long *best_denominator)
{
	unsigned long n, d, n0, d0, n1, d1;
	n = given_numerator;
	d = given_denominator;
	n0 = d1 = 0;
	n1 = d0 = 1;
	for (;;) {
		unsigned long t, a;
		if ((n1 > max_numerator) || (d1 > max_denominator)) {
			n1 = n0;
			d1 = d0;
			break;
		}
		if (d == 0)
			break;
		t = d;
		a = n / d;
		d = n % d;
		n = t;
		t = n0 + a * n1;
		n0 = n1;
		n1 = t;
		t = d0 + a * d1;
		d0 = d1;
		d1 = t;
	}
	*best_numerator = n1;
	*best_denominator = d1;
}

uint64_t Si5351::pll_calc(uint64_t freq, struct Si5351RegSet *reg, int32_t correction)
{
	uint64_t ref_freq = SI5351_XTAL_FREQ * 100ULL;
	uint32_t a, b, c, p1, p2, p3;
	uint64_t lltmp, rfrac, denom;
	int64_t ref_temp;

	// Factor calibration value into nominal crystal frequency
	// Measured in parts-per-ten million
	ref_temp = (int64_t)((double)(correction / 100000.0) * (double)ref_freq) + ref_freq;
	ref_freq = (uint64_t)ref_temp;

	// PLL bounds checking
	if (freq < SI5351_PLL_VCO_MIN * 100ULL)
	{
		freq = SI5351_PLL_VCO_MIN * 100ULL;
	}
	if (freq > SI5351_PLL_VCO_MAX * 100ULL)
	{
		freq = SI5351_PLL_VCO_MAX * 100ULL;
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
	denom = 1000ULL * 1000ULL;
	lltmp = freq % ref_freq;
	lltmp *= denom;
	do_div(lltmp, ref_freq);
	rfrac = lltmp;

	b = 0;
	c = 1;
	if (rfrac)
	{
		rational_best_approximation(rfrac, denom,
				    SI5351_PLL_B_MAX, SI5351_PLL_C_MAX, &b, &c);
	}

	// Calculate parameters
	p3  = c;
	p2  = (128 * b) % c;
	p1  = 128 * a;
	p1 += (128 * b / c);
	p1 -= 512;

	// Recalculate rate by fIN * (a + b/c)
	lltmp  = ref_freq;
	lltmp *= b;
	do_div(lltmp, c);
	freq = lltmp;
	freq += ref_freq * a;

	reg->p1 = p1;
	reg->p2 = p2;
	reg->p3 = p3;

	return freq;
}

uint64_t Si5351::multisynth_calc(uint64_t freq, struct Si5351RegSet *reg)
{
	uint64_t lltmp, pll_freq;
	uint32_t a, b, c, p1, p2, p3;
	uint8_t divby4;

	// Multisynth bounds checking
	if (freq > SI5351_MULTISYNTH_MAX_FREQ * 100ULL)
	{
		freq = SI5351_MULTISYNTH_MAX_FREQ * 100ULL;
	}
	if (freq < SI5351_MULTISYNTH_MIN_FREQ * 100ULL)
	{
		freq = SI5351_MULTISYNTH_MIN_FREQ * 100ULL;
	}

	divby4 = 0;
	if (freq >= SI5351_MULTISYNTH_DIVBY4_FREQ * 100ULL)
	{
		divby4 = 1;
	}

	// Find largest integer divider for max
	// VCO frequency and given target frequency
	if(divby4 == 0)
	{
		lltmp = SI5351_PLL_VCO_MAX * 100ULL;
		do_div(lltmp, freq);
		a = (uint32_t)lltmp;
	}
	else
	{
		a = 4;
	}

	b = 0;
	c = 1;
	pll_freq = a * freq;

	/* Calculate parameters */
	if (divby4 == 1)
	{
		p3 = 1;
		p2 = 0;
		p1 = 0;
	}
	else
	{
		p3  = c;
		p2  = (128 * b) % c;
		p1  = 128 * a;
		p1 += (128 * b / c);
		p1 -= 512;
	}

	reg->p1 = p1;
	reg->p2 = p2;
	reg->p3 = p3;

	return pll_freq;
}

uint64_t Si5351::multisynth_recalc(uint64_t freq, uint64_t pll_freq, struct Si5351RegSet *reg)
{
	uint64_t lltmp, rfrac, denom;
	uint32_t a, b, c, p1, p2, p3;
	uint8_t divby4;

	// Multisynth bounds checking
	if (freq > SI5351_MULTISYNTH_MAX_FREQ * 100ULL)
	{
		freq = SI5351_MULTISYNTH_MAX_FREQ * 100ULL;
	}
	if (freq < SI5351_MULTISYNTH_MIN_FREQ * 100ULL)
	{
		freq = SI5351_MULTISYNTH_MIN_FREQ * 100ULL;
	}

	divby4 = 0;
	if (freq > SI5351_MULTISYNTH_DIVBY4_FREQ * 100ULL)
	{
		divby4 = 1;
	}

	// Determine integer part of feedback equation
	a = pll_freq / freq;

	if (a < SI5351_MULTISYNTH_A_MIN)
	{
		freq = pll_freq / SI5351_MULTISYNTH_A_MIN;
	}
	if (a > SI5351_MULTISYNTH_A_MAX)
	{
		freq = pll_freq / SI5351_MULTISYNTH_A_MAX;
	}

	// Find best approximation for b/c
	denom = 1000ULL * 1000ULL * 1000ULL;
	lltmp = pll_freq % freq;
	lltmp *= denom;
	do_div(lltmp, freq);
	rfrac = lltmp;

	b = 0;
	c = 1;
	if (rfrac)
	{
		rational_best_approximation(rfrac, denom,
				    SI5351_MULTISYNTH_B_MAX, SI5351_MULTISYNTH_C_MAX, &b, &c);
	}

	// Recalculate output frequency by fOUT = fIN / (a + b/c)
	lltmp  = pll_freq;
	lltmp *= c;
	do_div(lltmp, a * c + b);
	freq  = lltmp;

	// Calculate parameters
	if (divby4)
	{
		p3 = 1;
		p2 = 0;
		p1 = 0;
	}
	else
	{
		p3  = c;
		p2  = (128 * b) % c;
		p1  = 128 * a;
		p1 += (128 * b / c);
		p1 -= 512;
	}

	reg->p1 = p1;
	reg->p2 = p2;
	reg->p3 = p3;

	return freq;
}

void Si5351::si5351_update_sys_status(struct Si5351Status *status)
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

void Si5351::si5351_update_int_status(struct Si5351IntStatus *int_status)
{
  uint8_t reg_val = 0;

  reg_val = si5351_read(SI5351_DEVICE_STATUS);

  // Parse the register
  int_status->SYS_INIT_STKY = (reg_val >> 7) & 0x01;
  int_status->LOL_B_STKY = (reg_val >> 6) & 0x01;
  int_status->LOL_A_STKY = (reg_val >> 5) & 0x01;
  int_status->LOS_STKY = (reg_val >> 4) & 0x01;
}

void Si5351::si5351_ms_div(enum si5351_clock clk, uint8_t r_div, uint8_t div_by_4)
{
	uint8_t reg_val, reg_addr;
	
	switch(clk)
	{
		case SI5351_CLK0:
			reg_addr = SI5351_CLK0_PARAMETERS + 2;
			break;
		case SI5351_CLK1:
			reg_addr = SI5351_CLK1_PARAMETERS + 2;
			break;
		case SI5351_CLK2:
			reg_addr = SI5351_CLK2_PARAMETERS + 2;
			break;
		case SI5351_CLK3:
			reg_addr = SI5351_CLK3_PARAMETERS + 2;
			break;
		case SI5351_CLK4:
			reg_addr = SI5351_CLK4_PARAMETERS + 2;
			break;
		case SI5351_CLK5:
			reg_addr = SI5351_CLK5_PARAMETERS + 2;
			break;
		case SI5351_CLK6:
			return;
		case SI5351_CLK7:
			return;
	}
	
	reg_val = si5351_read(reg_addr);
	
	if(div_by_4 == 0)
	{
		reg_val &= ~(SI5351_OUTPUT_CLK_DIVBY4);
	}
	else
	{
		reg_val |= (SI5351_OUTPUT_CLK_DIVBY4);
	}
	
	reg_val &= ~(0b111 << SI5351_OUTPUT_CLK_DIV_SHIFT);
	reg_val |= (r_div << SI5351_OUTPUT_CLK_DIV_SHIFT);
	
	si5351_write(reg_addr, reg_val);
}
