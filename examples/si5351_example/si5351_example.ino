/*
 * si5351_example.ino - Simple example of using Si5351Arduino library
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

#include "si5351.h"
#include "Wire.h"

Si5351 si5351;

void setup()
{
  bool i2c_found;

  // Start serial and initialize the Si5351
  Serial.begin(57600);
  i2c_found = si5351.init(SI5351_CRYSTAL_LOAD_8PF, 0, 0);
  if(!i2c_found)
  {
    Serial.println("Device not found on I2C bus!");
  }

  // Set CLK0 to output 14 MHz
  si5351.set_freq(1400000000ULL, SI5351_CLK0);

  // Set CLK1 to output 175 MHz
  si5351.set_ms_source(SI5351_CLK1, SI5351_PLLB);
  si5351.set_freq_manual(17500000000ULL, 70000000000ULL, SI5351_CLK1);

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
