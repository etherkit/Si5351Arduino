/*
 * si5351_clk67_example.ino - Simple example of setting CLK6 and CLK7
 *                            outputs using Si5351Arduino library
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

   // Set CLK6 to 23 MHz, library sets PLL to 874 MHz
   si5351.set_freq(2300000000ULL, SI5351_CLK6);

   // This won't work since it is not an even multiple of the PLL (874 MHz)
   si5351.set_freq(1234567800ULL, SI5351_CLK7);

   // Setting CLK7 to 87.4 MHz will work
   si5351.set_freq(8740000000ULL, SI5351_CLK7);

   si5351.update_status();
   delay(500);
 }

 void loop()
 {
   // Read the Status Register and print it every 10 seconds
   si5351.update_status();
   Serial.print("PLLA: ");
   Serial.print((uint32_t)(si5351.plla_freq/100));
   Serial.print("   PLLB: ");
   Serial.print((uint32_t)(si5351.pllb_freq/100));
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
