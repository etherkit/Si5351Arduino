/*
 * si5351_phase.ino - Example for setting phase with Si5351Arduino library
 *
 * Copyright (C) 2015 - 2016 Jason Milldrum <milldrum@gmail.com>
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

/*
 * Setting the phase of a clock requires that you manually set the PLL and
 * take the PLL frequency into account when calculation the value to place
 * in the phase register. As shown on page 10 of Silicon Labs Application
 * Note 619 (AN619), the phase register is a 7-bit register, where a bit
 * represents a phase difference of 1/4 the PLL period. Therefore, the best
 * way to get an accurate phase setting is to make the PLL an even multiple
 * of the clock frequency, depending on what phase you need.
 *
 * If you need a 90 degree phase shift (as in many RF applications), then
 * it is quite easy to determine your parameters. Pick a PLL frequency that
 * is an even multiple of your clock frequency (remember that the PLL needs
 * to be in the range of 600 to 900 MHz). Then to set a 90 degree phase shift,
 * you simply enter that multiple into the phase register. Remember when
 * setting multiple outputs to be phase-related to each other, they each need
 * to be referenced to the same PLL.
 */

#include "si5351.h"
#include "Wire.h"

Si5351 si5351;

void setup()
{
  // Start serial and initialize the Si5351
  Serial.begin(57600);
  si5351.init(SI5351_CRYSTAL_LOAD_8PF, 0, 0);

  // We will output 14.1 MHz on CLK0 and CLK1.
  // A PLLA frequency of 705 MHz was chosen to give an even
  // divisor by 14.1 MHz.
  unsigned long long freq = 1410000000ULL;
  unsigned long long pll_freq = 70500000000ULL;

  // Set CLK0 and CLK1 to use PLLA as the MS source.
  // This is not explicitly necessary in v2 of this library,
  // as these are already the default assignments.
  // si5351.set_ms_source(SI5351_CLK0, SI5351_PLLA);
  // si5351.set_ms_source(SI5351_CLK1, SI5351_PLLA);

  // Set CLK0 and CLK1 to output 14.1 MHz with a fixed PLL frequency
  si5351.set_freq_manual(freq, pll_freq, SI5351_CLK0);
  si5351.set_freq_manual(freq, pll_freq, SI5351_CLK1);

  // Now we can set CLK1 to have a 90 deg phase shift by entering
  // 50 in the CLK1 phase register, since the ratio of the PLL to
  // the clock frequency is 50.
  si5351.set_phase(SI5351_CLK0, 0);
  si5351.set_phase(SI5351_CLK1, 50);

  // We need to reset the PLL before they will be in phase alignment
  si5351.pll_reset(SI5351_PLLA);

  // Query a status update and wait a bit to let the Si5351 populate the
  // status flags correctly.
  si5351.update_status();
  delay(500);
}

void loop()
{
  // Read the Status Register and print it every 10 seconds
  si5351.update_status();
  Serial.print("SYS_INIT: ");
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
