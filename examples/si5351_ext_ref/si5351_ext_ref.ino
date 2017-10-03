/*
 * si5351_ext_ref.ino - Simple example of using an external reference
 *                      clock with the Si5351Arduino library
 *
 * Copyright (C) 2016 Jason Milldrum <milldrum@gmail.com>
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

#include "si5351.h"
#include "Wire.h"

Si5351 si5351;

void setup()
{
  // Start serial
  Serial.begin(57600);

  // Initialize the Si5351 to use a 25 MHz clock on the XO input
  si5351.init(SI5351_CRYSTAL_LOAD_0PF, 0, 0);

  // Set the CLKIN reference frequency to 10 MHz
  si5351.set_ref_freq(10000000UL, SI5351_PLL_INPUT_CLKIN);

  // Apply a correction factor to CLKIN
  si5351.set_correction(0, SI5351_PLL_INPUT_CLKIN);

  // Set PLLA and PLLB to use the signal on CLKIN instead of the XTAL
  si5351.set_pll_input(SI5351_PLLA, SI5351_PLL_INPUT_CLKIN);
  si5351.set_pll_input(SI5351_PLLB, SI5351_PLL_INPUT_CLKIN);

  // Set CLK0 to output 14 MHz
  si5351.set_freq(1400000000ULL, SI5351_CLK0);

  si5351.update_status();
  delay(500);
}

void loop()
{
  // Read the Status Register and print it every 10 seconds
  si5351.update_status();
  Serial.print("  SYS_INIT: ");
  Serial.print(si5351.dev_status.SYS_INIT);
  Serial.print("  LOL_A: ");
  Serial.print(si5351.dev_status.LOL_A);
  Serial.print("  LOL_B: ");
  Serial.print(si5351.dev_status.LOL_B);
  Serial.print("  LOS: ");
  Serial.print(si5351.dev_status.LOS);
  Serial.print("  REVID: ");
  Serial.println(si5351.dev_status.REVID);

  delay(10000);
}
