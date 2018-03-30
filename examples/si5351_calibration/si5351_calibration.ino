/*
 * si5351_calibration.ino - Simple calibration routine for the Si5351
 *                          breakout board.
 *
 * Copyright 2015 - 2018 Paul Warren <pwarren@pwarren.id.au>
 *                       Jason Milldrum <milldrum@gmail.com>
 *
 * Uses code from https://github.com/darksidelemm/open_radio_miniconf_2015
 * and the old version of the calibration sketch
 *
 * This sketch  is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * Foobar is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License.
 * If not, see <http://www.gnu.org/licenses/>.
*/

#include "si5351.h"
#include "Wire.h"

Si5351 si5351;

int32_t cal_factor = 0;
int32_t old_cal = 0;

uint64_t rx_freq;
uint64_t target_freq = 1000000000ULL; // 10 MHz, in hundredths of hertz

void setup()
{
  // Start serial and initialize the Si5351
  Serial.begin(57600);

  // The crystal load value needs to match in order to have an accurate calibration
  si5351.init(SI5351_CRYSTAL_LOAD_8PF, 0, 0);

  // Start on target frequency
  si5351.set_correction(cal_factor, SI5351_PLL_INPUT_XO);
  si5351.set_pll(SI5351_PLL_FIXED, SI5351_PLLA);
  si5351.set_freq(target_freq, SI5351_CLK0);
}

void loop()
{
  si5351.update_status();
  if (si5351.dev_status.SYS_INIT == 1)
  {
     Serial.println(F("Initialising Si5351, you shouldn't see many of these!"));
     delay(500);
  }
  else
  {
    Serial.println();
    Serial.println(F("Adjust until your frequency counter reads as close to 10 MHz as possible."));
    Serial.println(F("Press 'q' when complete."));
    vfo_interface();
  }
}

static void flush_input(void)
{
  while (Serial.available() > 0)
  Serial.read();
}

static void vfo_interface(void)
{
  rx_freq = target_freq;
  cal_factor = old_cal;
  Serial.println(F("   Up:   r   t  y  u  i   o  p"));
  Serial.println(F(" Down:   f   g  h  j  k   l  ;"));
  Serial.println(F("   Hz: 0.01 0.1 1 10 100 1K 10k"));
  while (1)
  {
  if (Serial.available() > 0)
  {
    char c = Serial.read();
    switch (c)
    {
      case 'q':
        flush_input();
        Serial.println();
        Serial.print(F("Calibration factor is "));
        Serial.println(cal_factor);
        Serial.println(F("Setting calibration factor"));
        si5351.set_correction(cal_factor, SI5351_PLL_INPUT_XO);
        si5351.set_pll(SI5351_PLL_FIXED, SI5351_PLLA);
        Serial.println(F("Resetting target frequency"));
        si5351.set_freq(target_freq, SI5351_CLK0);
        old_cal = cal_factor;
        return;
      case 'r': rx_freq += 1; break;
      case 'f': rx_freq -= 1; break;
      case 't': rx_freq += 10; break;
      case 'g': rx_freq -= 10; break;
      case 'y': rx_freq += 100; break;
      case 'h': rx_freq -= 100; break;
      case 'u': rx_freq += 1000; break;
      case 'j': rx_freq -= 1000; break;
      case 'i': rx_freq += 10000; break;
      case 'k': rx_freq -= 10000; break;
      case 'o': rx_freq += 100000; break;
      case 'l': rx_freq -= 100000; break;
      case 'p': rx_freq += 1000000; break;
      case ';': rx_freq -= 1000000; break;
      default:
        // Do nothing
      continue;
    }

    cal_factor = (int32_t)(target_freq - rx_freq) + old_cal;
    si5351.set_correction(cal_factor, SI5351_PLL_INPUT_XO);
    si5351.set_pll(SI5351_PLL_FIXED, SI5351_PLLA);
    si5351.pll_reset(SI5351_PLLA);
    si5351.set_freq(target_freq, SI5351_CLK0);
    Serial.print(F("Current difference:"));
    Serial.println(cal_factor);
    }
  }
}
