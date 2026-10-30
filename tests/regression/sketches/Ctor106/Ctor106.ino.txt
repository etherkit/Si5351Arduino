// Issue #103/#106 review: every constructor form must compile on every core.
#include <Wire.h>
#include <si5351.h>

Si5351 s_default;            // default: &Wire, 0x60
Si5351 s_addr(0x61);         // address only (uint8_t)
Si5351 s_wire(&Wire);        // bus only
Si5351 s_zero(0);            // reviewer finding 1: must not be ambiguous
Si5351 s_null(NULL);         // reviewer finding 1: must not be ambiguous
#if defined(ESP32) || defined(ARDUINO_ARCH_MBED) || (defined(WIRE_INTERFACES_COUNT) && WIRE_INTERFACES_COUNT > 1) || defined(ARDUINO_ARCH_RP2040)
#define HAVE_WIRE1 1
Si5351 s_wire1(&Wire1, 0x61);
#elif defined(ARDUINO_ARCH_STM32)
#define HAVE_WIRE1 1
TwoWire Wire1x(PB11, PB10);
Si5351 s_wire1(&Wire1x, 0x61);
#endif

void setup() {
  s_default.init(SI5351_CRYSTAL_LOAD_8PF, 0, 0);
  s_addr.init(SI5351_CRYSTAL_LOAD_8PF, 0, 0);
  s_wire.init(SI5351_CRYSTAL_LOAD_8PF, 0, 0);
  s_zero.init(SI5351_CRYSTAL_LOAD_8PF, 0, 0);
  s_null.init(SI5351_CRYSTAL_LOAD_8PF, 0, 0);
#ifdef HAVE_WIRE1
  s_wire1.init(SI5351_CRYSTAL_LOAD_8PF, 0, 0);
#endif
  s_default.set_freq(1000000000ULL, SI5351_CLK0);
}
void loop() {}
