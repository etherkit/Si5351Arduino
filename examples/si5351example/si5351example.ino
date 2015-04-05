#include "si5351.h"
#include "Wire.h"

Si5351 si5351;

void setup()
{
  // Start serial and initialize the Si5351
  Serial.begin(57600);
  si5351.init(SI5351_CRYSTAL_LOAD_8PF);

  // Set CLK0 to output 14 MHz with a fixed PLL frequency
  si5351.set_pll(SI5351_PLL_FIXED, SI5351_PLLA);
  si5351.set_freq(14000000, SI5351_PLL_FIXED, SI5351_CLK0);

  // Set CLK1 to output 20 MHz
  si5351.set_freq(20000000, 0, SI5351_CLK1);
}

void loop()
{
  // Read the Status Register and print it every second
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

  delay(1000);
}
