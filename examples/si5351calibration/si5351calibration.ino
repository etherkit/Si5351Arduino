#include <si5351.h>
#include "Wire.h"

Si5351 si5351;

void setup()
{
  // Initialize the Si5351
  // Change the 2nd parameter in init if using a ref osc other
  // than 25 MHz
  si5351.init(SI5351_CRYSTAL_LOAD_8PF, 0);
  
  // Set CLK0 output to 10.000 000 00 MHz with no correction factor
  si5351.set_correction(0);
  si5351.set_freq(1000000000, 0, SI5351_CLK0);
}

void loop()
{
}


