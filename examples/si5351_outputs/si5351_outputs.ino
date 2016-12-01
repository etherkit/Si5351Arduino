/*
 * si5351_outputs.ino - How to set different output sources
 *                      with the Si5351Arduino library
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
  // Start serial and initialize the Si5351
  Serial.begin(57600);
  si5351.init(SI5351_CRYSTAL_LOAD_8PF, 0, 0);

  // Set CLK0 to output 14 MHz
  si5351.set_freq(1400000000ULL, SI5351_CLK0);

  // Enable clock fanout for the XO
  si5351.set_clock_fanout(SI5351_FANOUT_XO, 1);

  // Enable clock fanout for MS
  si5351.set_clock_fanout(SI5351_FANOUT_MS, 1);

  // Set CLK1 to output the XO signal
  si5351.set_clock_source(SI5351_CLK1, SI5351_CLK_SRC_XTAL);
  si5351.output_enable(SI5351_CLK1, 1);

  // Set CLK2 to mirror the MS0 (CLK0) output
  si5351.set_clock_source(SI5351_CLK2, SI5351_CLK_SRC_MS0);
  si5351.output_enable(SI5351_CLK2, 1);

  // Change CLK0 output to 10 MHz, observe how CLK2 also changes
  si5351.set_freq(1000000000ULL, SI5351_CLK0);

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
